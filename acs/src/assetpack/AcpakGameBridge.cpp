// SPDX-License-Identifier: Apache-2.0
// Concrete GameFramework bridge for `.acpak`.
#include "assetpack/AcpakGameBridge.h"

#include "foundation/Error.h"
#include "threading/ScopedLock.h"

#include <Windows.h>

namespace acs::assetpack {

namespace {

/** 入力が妥当な UTF-8 でない。 */
inline constexpr u16 kSubAcpakBridgeBadUtf8 = 1391;

/** 変換後の文字列が許容長を超える。 */
inline constexpr u16 kSubAcpakBridgePathTooLong = 1392;

/** 引数が null / 空文字列など不正。 */
inline constexpr u16 kSubAcpakBridgeBadArgument = 1393;

/** UTF-8 名 pool の防御的な最大バイト数。 */
inline constexpr usize kAcpakMaxUtf8NamePoolBytes = kAcpakMaxPathPoolBytes * 2u;

/** UTF-8 文字列を呼び出し側所有の UTF-16 バッファへ変換する。 */
TResult<const wchar_t*> ConvertUtf8ToWide(const char* Text, wchar_t* Out, u32 OutCapacity) noexcept
{
    if (Text == nullptr || Text[0] == 0 || Out == nullptr || OutCapacity == 0) {
        return TResult<const wchar_t*>(ACS_ERR(IO, kSubAcpakBridgeBadArgument, "Acpak bridge path is empty"));
    }

    const int Required = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text, -1, nullptr, 0);
    if (Required <= 0) {
        return TResult<const wchar_t*>(ACS_ERR(IO, kSubAcpakBridgeBadUtf8, "Acpak bridge path is not valid UTF-8"));
    }
    if (static_cast<u32>(Required) > OutCapacity) {
        return TResult<const wchar_t*>(ACS_ERR(IO, kSubAcpakBridgePathTooLong, "Acpak bridge path is too long"));
    }

    const int Written = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text, -1, Out,
                                              static_cast<int>(OutCapacity));
    if (Written <= 0) {
        return TResult<const wchar_t*>(ACS_ERR(IO, kSubAcpakBridgeBadUtf8, "Acpak bridge path conversion failed"));
    }
    return TResult<const wchar_t*>(OkInit, Out);
}

} // namespace

FAcpakGameReader::FAcpakGameReader() noexcept : FAcpakGameReader(DefaultAllocator())
{
}

FAcpakGameReader::FAcpakGameReader(FAllocator& Allocator) noexcept
    : m_Reader(Allocator), m_Utf8NamePool(Allocator), m_Utf8NameOffsets(Allocator)
{
}

FAcpakGameReader::~FAcpakGameReader() noexcept
{
    Unmount();
}

TResult<void> FAcpakGameReader::BuildFileNamePool() noexcept
{
    const u32 FileCount = m_Reader.FileCount();
    if (!m_Utf8NameOffsets.TryResize(FileCount)) {
        return ACS_ERR(Memory, kAcpakSubOutOfMemory, "FAcpakGameReader: file name offset allocation failed");
    }

    usize TotalBytes = 0;
    for (u32 Index = 0; Index < FileCount; ++Index) {
        const FAcpakFileEntry* Entry = m_Reader.GetEntry(Index);
        if (Entry == nullptr) {
            return ACS_ERR(IO, kAcpakSubBadSize, "FAcpakGameReader: entry disappeared while mounting");
        }

        const int Required = ::WideCharToMultiByte(CP_UTF8, 0, Entry->path, -1, nullptr, 0, nullptr, nullptr);
        if (Required <= 0) {
            return ACS_ERR(IO, kSubAcpakBridgeBadUtf8, "FAcpakGameReader: file name conversion failed");
        }
        const usize RequiredBytes = static_cast<usize>(Required);
        if (RequiredBytes > kAcpakMaxUtf8NamePoolBytes - TotalBytes) {
            return ACS_ERR(IO, kAcpakSubBadSize, "FAcpakGameReader: UTF-8 file name pool exceeds limit");
        }
        m_Utf8NameOffsets[Index] = TotalBytes;
        TotalBytes += RequiredBytes;
    }

    if (!m_Utf8NamePool.TryResize(TotalBytes)) {
        return ACS_ERR(Memory, kAcpakSubOutOfMemory, "FAcpakGameReader: file name pool allocation failed");
    }

    for (u32 Index = 0; Index < FileCount; ++Index) {
        const FAcpakFileEntry* Entry = m_Reader.GetEntry(Index);
        char* Destination = m_Utf8NamePool.Data() + m_Utf8NameOffsets[Index];
        const usize AvailableBytes = TotalBytes - m_Utf8NameOffsets[Index];
        if (Entry == nullptr) {
            return ACS_ERR(IO, kAcpakSubBadSize, "FAcpakGameReader: invalid file name pool layout");
        }

        const int Written = ::WideCharToMultiByte(CP_UTF8, 0, Entry->path, -1, Destination,
                                                  static_cast<int>(AvailableBytes), nullptr, nullptr);
        if (Written <= 0) {
            return ACS_ERR(IO, kSubAcpakBridgeBadUtf8, "FAcpakGameReader: file name conversion failed");
        }
    }

    return Ok();
}

void FAcpakGameReader::ReleaseFileNamePool() noexcept
{
    m_Utf8NameOffsets.ReleaseStorage();
    m_Utf8NamePool.ReleaseStorage();
}

TResult<void> FAcpakGameReader::Mount(const char* PackPath) noexcept
{
    ScopedExclusiveLock Lock(m_LifecycleLock);
    m_Reader.Close();
    ReleaseFileNamePool();

    wchar_t WidePath[kPathCapacity] = {};
    const auto ConvertedPath = ConvertUtf8ToWide(PackPath, WidePath, kPathCapacity);
    if (ConvertedPath.IsErr()) {
        return ConvertedPath.Error();
    }

    const auto OpenResult = m_Reader.Open(ConvertedPath.Value());
    if (OpenResult.IsErr()) {
        return OpenResult.Error();
    }

    const auto PoolResult = BuildFileNamePool();
    if (PoolResult.IsErr()) {
        m_Reader.Close();
        ReleaseFileNamePool();
        return PoolResult.Error();
    }
    return Ok();
}

void FAcpakGameReader::Unmount() noexcept
{
    ScopedExclusiveLock Lock(m_LifecycleLock);
    m_Reader.Close();
    ReleaseFileNamePool();
}

bool FAcpakGameReader::IsMounted() const noexcept
{
    ScopedSharedLock Lock(m_LifecycleLock);
    return m_Reader.IsOpen();
}

TResult<u32> FAcpakGameReader::FileCount() noexcept
{
    ScopedSharedLock Lock(m_LifecycleLock);
    if (!m_Reader.IsOpen()) {
        return TResult<u32>(ACS_ERR(IO, game::kSubAssetPackNotMounted, "FAcpakGameReader::FileCount before Mount"));
    }
    return TResult<u32>(OkInit, m_Reader.FileCount());
}

TResult<const char*> FAcpakGameReader::FileName(u32 Index) noexcept
{
    ScopedSharedLock Lock(m_LifecycleLock);
    if (!m_Reader.IsOpen()) {
        return TResult<const char*>(
            ACS_ERR(IO, game::kSubAssetPackNotMounted, "FAcpakGameReader::FileName before Mount"));
    }
    if (static_cast<usize>(Index) >= m_Utf8NameOffsets.Size()) {
        return TResult<const char*>(ACS_ERR(IO, kAcpakSubNotFound, "FAcpakGameReader::FileName index not found"));
    }
    return TResult<const char*>(OkInit, m_Utf8NamePool.Data() + m_Utf8NameOffsets[Index]);
}

TResult<u64> FAcpakGameReader::FileSize(const char* Name) noexcept
{
    ScopedSharedLock Lock(m_LifecycleLock);
    wchar_t WideName[kPathCapacity] = {};
    const auto ConvertedName = ConvertUtf8ToWide(Name, WideName, kPathCapacity);
    if (ConvertedName.IsErr()) {
        return TResult<u64>(ConvertedName.Error());
    }
    return m_Reader.GetUncompressedSize(ConvertedName.Value());
}

TResult<void> FAcpakGameReader::ReadFile(const char* Name, u8* OutBuffer, u64 BufferSize) noexcept
{
    ScopedSharedLock Lock(m_LifecycleLock);
    wchar_t WideName[kPathCapacity] = {};
    const auto ConvertedName = ConvertUtf8ToWide(Name, WideName, kPathCapacity);
    if (ConvertedName.IsErr()) {
        return ConvertedName.Error();
    }
    const auto ReadResult = m_Reader.ReadFile(ConvertedName.Value(), OutBuffer, BufferSize);
    if (ReadResult.IsErr()) {
        return ReadResult.Error();
    }
    return Ok();
}

TResult<void> FAcpakGameWriter::BeginPack(const char* OutputPath) noexcept
{
    wchar_t WidePath[kPathCapacity] = {};
    const auto ConvertedPath = ConvertUtf8ToWide(OutputPath, WidePath, kPathCapacity);
    if (ConvertedPath.IsErr()) {
        return ConvertedPath.Error();
    }
    return m_Writer.Open(ConvertedPath.Value(), AcpakFlagNone);
}

TResult<void> FAcpakGameWriter::AddFile(const char* VirtualName, const u8* Data, u64 Size) noexcept
{
    wchar_t WideName[kPathCapacity] = {};
    const auto ConvertedName = ConvertUtf8ToWide(VirtualName, WideName, kPathCapacity);
    if (ConvertedName.IsErr()) {
        return ConvertedName.Error();
    }
    return m_Writer.AddFile(ConvertedName.Value(), Data, Size);
}

TResult<void> FAcpakGameWriter::FinishPack() noexcept
{
    const auto FinishResult = m_Writer.Finalize();
    m_Writer.Close();
    return FinishResult;
}

} // namespace acs::assetpack
