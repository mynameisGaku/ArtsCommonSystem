// SPDX-License-Identifier: Apache-2.0
// Concrete GameFramework bridge for `.acpak`.
#include "assetpack/AcpakGameBridge.h"

#include "foundation/Error.h"

#include <Windows.h>

namespace acs::assetpack {

namespace {

inline constexpr u16 kSubAcpakBridgeBadUtf8 = 1391;
inline constexpr u16 kSubAcpakBridgePathTooLong = 1392;
inline constexpr u16 kSubAcpakBridgeBadArgument = 1393;

TResult<const wchar_t*> ConvertUtf8ToWide(const char* Text,
                                          wchar_t* Out,
                                          u32 OutCapacity) noexcept {
    if (Text == nullptr || Text[0] == 0 || Out == nullptr || OutCapacity == 0) {
        return TResult<const wchar_t*>(ACS_ERR(IO, kSubAcpakBridgeBadArgument,
                                              "Acpak bridge path is empty"));
    }

    const int Required = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                               Text, -1, nullptr, 0);
    if (Required <= 0) {
        return TResult<const wchar_t*>(ACS_ERR(IO, kSubAcpakBridgeBadUtf8,
                                              "Acpak bridge path is not valid UTF-8"));
    }
    if (static_cast<u32>(Required) > OutCapacity) {
        return TResult<const wchar_t*>(ACS_ERR(IO, kSubAcpakBridgePathTooLong,
                                              "Acpak bridge path is too long"));
    }

    const int Written = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                              Text, -1, Out,
                                              static_cast<int>(OutCapacity));
    if (Written <= 0) {
        return TResult<const wchar_t*>(ACS_ERR(IO, kSubAcpakBridgeBadUtf8,
                                              "Acpak bridge path conversion failed"));
    }
    return TResult<const wchar_t*>(OkInit, Out);
}

TResult<const char*> ConvertWideToUtf8(const wchar_t* Text,
                                       char* Out,
                                       u32 OutCapacity) noexcept {
    if (Text == nullptr || Out == nullptr || OutCapacity == 0) {
        return TResult<const char*>(ACS_ERR(IO, kSubAcpakBridgeBadArgument,
                                           "Acpak bridge file name is empty"));
    }

    const int Required = ::WideCharToMultiByte(CP_UTF8, 0, Text, -1, nullptr, 0,
                                               nullptr, nullptr);
    if (Required <= 0) {
        return TResult<const char*>(ACS_ERR(IO, kSubAcpakBridgeBadUtf8,
                                           "Acpak bridge file name conversion failed"));
    }
    if (static_cast<u32>(Required) > OutCapacity) {
        return TResult<const char*>(ACS_ERR(IO, kSubAcpakBridgePathTooLong,
                                           "Acpak bridge file name is too long"));
    }

    const int Written = ::WideCharToMultiByte(CP_UTF8, 0, Text, -1, Out,
                                              static_cast<int>(OutCapacity),
                                              nullptr, nullptr);
    if (Written <= 0) {
        return TResult<const char*>(ACS_ERR(IO, kSubAcpakBridgeBadUtf8,
                                           "Acpak bridge file name conversion failed"));
    }
    return TResult<const char*>(OkInit, Out);
}

} // namespace

TResult<const wchar_t*> FAcpakGameReader::ToWideScratch(const char* Text) noexcept {
    return ConvertUtf8ToWide(Text, m_WideScratch, kPathCapacity);
}

TResult<const char*> FAcpakGameReader::ToUtf8Scratch(const wchar_t* Text) noexcept {
    return ConvertWideToUtf8(Text, m_Utf8Scratch, kPathCapacity);
}

TResult<void> FAcpakGameReader::Mount(const char* PackPath) noexcept {
    auto WidePath = ToWideScratch(PackPath);
    if (WidePath.IsErr()) {
        return WidePath.Error();
    }
    return m_Reader.Open(WidePath.Value());
}

void FAcpakGameReader::Unmount() noexcept {
    m_Reader.Close();
}

bool FAcpakGameReader::IsMounted() const noexcept {
    return m_Reader.IsOpen();
}

TResult<u32> FAcpakGameReader::FileCount() noexcept {
    if (!m_Reader.IsOpen()) {
        return TResult<u32>(ACS_ERR(IO, game::kSubAssetPackNotMounted,
                                   "FAcpakGameReader::FileCount before Mount"));
    }
    return TResult<u32>(OkInit, m_Reader.FileCount());
}

TResult<const char*> FAcpakGameReader::FileName(u32 Index) noexcept {
    if (!m_Reader.IsOpen()) {
        return TResult<const char*>(ACS_ERR(IO, game::kSubAssetPackNotMounted,
                                           "FAcpakGameReader::FileName before Mount"));
    }
    const FAcpakFileEntry* Entry = m_Reader.GetEntry(Index);
    if (Entry == nullptr) {
        return TResult<const char*>(ACS_ERR(IO, kAcpakSubNotFound,
                                           "FAcpakGameReader::FileName index not found"));
    }
    return ToUtf8Scratch(Entry->path);
}

TResult<u64> FAcpakGameReader::FileSize(const char* Name) noexcept {
    auto WideName = ToWideScratch(Name);
    if (WideName.IsErr()) {
        return TResult<u64>(WideName.Error());
    }
    return m_Reader.GetUncompressedSize(WideName.Value());
}

TResult<void> FAcpakGameReader::ReadFile(const char* Name,
                                         u8* OutBuffer,
                                         u64 BufferSize) noexcept {
    auto WideName = ToWideScratch(Name);
    if (WideName.IsErr()) {
        return WideName.Error();
    }
    auto Read = m_Reader.ReadFile(WideName.Value(), OutBuffer, BufferSize);
    if (Read.IsErr()) {
        return Read.Error();
    }
    return Ok();
}

TResult<const wchar_t*> FAcpakGameWriter::ToWideScratch(const char* Text) noexcept {
    return ConvertUtf8ToWide(Text, m_WideScratch, kPathCapacity);
}

TResult<void> FAcpakGameWriter::BeginPack(const char* OutputPath) noexcept {
    auto WidePath = ToWideScratch(OutputPath);
    if (WidePath.IsErr()) {
        return WidePath.Error();
    }
    return m_Writer.Open(WidePath.Value(), AcpakFlagNone);
}

TResult<void> FAcpakGameWriter::AddFile(const char* VirtualName,
                                        const u8* Data,
                                        u64 Size) noexcept {
    auto WideName = ToWideScratch(VirtualName);
    if (WideName.IsErr()) {
        return WideName.Error();
    }
    return m_Writer.AddFile(WideName.Value(), Data, Size);
}

TResult<void> FAcpakGameWriter::FinishPack() noexcept {
    auto Finish = m_Writer.Finalize();
    m_Writer.Close();
    return Finish;
}

} // namespace acs::assetpack
