// SPDX-License-Identifier: Apache-2.0
// Concrete GameFramework bridge for `.acpak`.
#include "assetpack/AcpakGameBridge.h"

#include "foundation/Error.h"

#include <Windows.h>

namespace acs::assetpack {

namespace {

/** 入力が妥当な UTF-8 でない (MultiByteToWideChar / WideCharToMultiByte 失敗)。 */
inline constexpr u16 kSubAcpakBridgeBadUtf8 = 1391;

/** 変換後の文字列が出力スクラッチ容量を超える。 */
inline constexpr u16 kSubAcpakBridgePathTooLong = 1392;

/** 引数が null / 空文字列など不正。 */
inline constexpr u16 kSubAcpakBridgeBadArgument = 1393;

/**
 * UTF-8 文字列を UTF-16 に変換して Out バッファに書き込む。
 *
 * @details
 * MultiByteToWideChar を 2 回呼び (必要長算出 → 実変換)、MB_ERR_INVALID_CHARS で
 * 不正バイト列を弾く。NUL 終端も含めて書き込む。Out の容量が足りない場合は変換せず
 * エラーを返す。
 * @param Text 変換元の UTF-8 文字列 (null / 空はエラー)。
 * @param Out 変換結果の出力先 UTF-16 バッファ。
 * @param OutCapacity Out の wchar_t 単位の容量。
 * @return Out を指すポインタ、引数不正 / 不正 UTF-8 / 容量不足ならエラー。
 */
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

/**
 * UTF-16 文字列を UTF-8 に変換して Out バッファに書き込む。
 *
 * @details
 * WideCharToMultiByte を 2 回呼び (必要長算出 → 実変換)、NUL 終端も含めて書き込む。
 * Out の容量が足りない場合は変換せずエラーを返す。
 * @param Text 変換元の UTF-16 文字列 (null はエラー)。
 * @param Out 変換結果の出力先 UTF-8 バッファ。
 * @param OutCapacity Out の char 単位の容量。
 * @return Out を指すポインタ、引数不正 / 変換失敗 / 容量不足ならエラー。
 */
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

/** UTF-8 を m_WideScratch (UTF-16) に変換する。 */
TResult<const wchar_t*> FAcpakGameReader::ToWideScratch(const char* Text) noexcept {
    return ConvertUtf8ToWide(Text, m_WideScratch, kPathCapacity);
}

/** UTF-16 を m_Utf8Scratch (UTF-8) に変換する。 */
TResult<const char*> FAcpakGameReader::ToUtf8Scratch(const wchar_t* Text) noexcept {
    return ConvertWideToUtf8(Text, m_Utf8Scratch, kPathCapacity);
}

/** UTF-8 パスを UTF-16 に変換して `.acpak` を開く。 */
TResult<void> FAcpakGameReader::Mount(const char* PackPath) noexcept {
    const auto WidePath = ToWideScratch(PackPath);
    if (WidePath.IsErr()) {
        return WidePath.Error();
    }
    return m_Reader.Open(WidePath.Value());
}

/** 現在の pak をアンマウントする。 */
void FAcpakGameReader::Unmount() noexcept {
    m_Reader.Close();
}

/** pak がマウント済みかを返す。 */
bool FAcpakGameReader::IsMounted() const noexcept {
    return m_Reader.IsOpen();
}

/** マウント中の pak のファイル数を返す (未 Mount はエラー)。 */
TResult<u32> FAcpakGameReader::FileCount() noexcept {
    if (!m_Reader.IsOpen()) {
        return TResult<u32>(ACS_ERR(IO, game::kSubAssetPackNotMounted,
                                   "FAcpakGameReader::FileCount before Mount"));
    }
    return TResult<u32>(OkInit, m_Reader.FileCount());
}

/** Index 番目のファイル名を UTF-8 で返す (未 Mount / 範囲外はエラー)。 */
TResult<const char*> FAcpakGameReader::FileName(u32 Index) noexcept {
    if (!m_Reader.IsOpen()) {
        return TResult<const char*>(ACS_ERR(IO, game::kSubAssetPackNotMounted,
                                           "FAcpakGameReader::FileName before Mount"));
    }
    const FAcpakFileEntry* const Entry = m_Reader.GetEntry(Index);
    if (Entry == nullptr) {
        return TResult<const char*>(ACS_ERR(IO, kAcpakSubNotFound,
                                           "FAcpakGameReader::FileName index not found"));
    }
    return ToUtf8Scratch(Entry->path);
}

/** 仮想ファイル名から復号 + 解凍後のサイズを返す。 */
TResult<u64> FAcpakGameReader::FileSize(const char* Name) noexcept {
    const auto WideName = ToWideScratch(Name);
    if (WideName.IsErr()) {
        return TResult<u64>(WideName.Error());
    }
    return m_Reader.GetUncompressedSize(WideName.Value());
}

/** 仮想ファイルを復号 + 解凍して OutBuffer にコピーする。 */
TResult<void> FAcpakGameReader::ReadFile(const char* Name,
                                         u8* OutBuffer,
                                         u64 BufferSize) noexcept {
    const auto WideName = ToWideScratch(Name);
    if (WideName.IsErr()) {
        return WideName.Error();
    }
    const auto Read = m_Reader.ReadFile(WideName.Value(), OutBuffer, BufferSize);
    if (Read.IsErr()) {
        return Read.Error();
    }
    return Ok();
}

/** UTF-8 を m_WideScratch (UTF-16) に変換する。 */
TResult<const wchar_t*> FAcpakGameWriter::ToWideScratch(const char* Text) noexcept {
    return ConvertUtf8ToWide(Text, m_WideScratch, kPathCapacity);
}

/** UTF-8 パスを UTF-16 に変換し AcpakFlagNone で出力 pak を開く。 */
TResult<void> FAcpakGameWriter::BeginPack(const char* OutputPath) noexcept {
    const auto WidePath = ToWideScratch(OutputPath);
    if (WidePath.IsErr()) {
        return WidePath.Error();
    }
    return m_Writer.Open(WidePath.Value(), AcpakFlagNone);
}

/** 仮想名を UTF-16 に変換して 1 ファイルを pak に追加する。 */
TResult<void> FAcpakGameWriter::AddFile(const char* VirtualName,
                                        const u8* Data,
                                        u64 Size) noexcept {
    const auto WideName = ToWideScratch(VirtualName);
    if (WideName.IsErr()) {
        return WideName.Error();
    }
    return m_Writer.AddFile(WideName.Value(), Data, Size);
}

/** pak を Finalize して Close し、Finalize の結果を返す。 */
TResult<void> FAcpakGameWriter::FinishPack() noexcept {
    const auto Finish = m_Writer.Finalize();
    m_Writer.Close();
    return Finish;
}

} // namespace acs::assetpack
