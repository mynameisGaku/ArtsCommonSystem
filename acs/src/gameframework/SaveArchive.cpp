// SPDX-License-Identifier: Apache-2.0
// FSaveArchive 実装 (Win32 I/O + CRC32)
//
// FSaveArchive.h で宣言した 4 メソッド (WriteToFile / ReadFromFile /
// PeekVersion / PeekPayloadSize) の本体。Win32 ファイル API を直接叩いて
// `.acssave` の bit-precise format を読み書きする。
//
// 実装上のポイント:
//   ・<windows.h> は foundation/Platform.h 経由で 1 箇所に閉じ込める
//     (Yield / CreateFile / GetMessage 等のマクロ汚染対策はここで吸収済)。
//   ・CRC32 ルーチンは assetpack/FAcpakReader.cpp / FAcpakWriter.cpp と同じ実装
//     (poly 0xEDB88320, init/xorout 0xFFFFFFFF, table 256 entry)。link 単位を
//     独立させたい (assetpack を依存に持たないテストビルドでも FSaveArchive を
//     使えるように) ため、ここでも単独に持つ。
//   ・little-endian 読み書きは MemCopy 経由で strict-aliasing 違反を避ける。
//     ACS 対応プラットフォームは Win/x64 と ARM64 (LE) のみ前提。
//   ・>4GiB の payload を扱う想定は無いが、WriteFile/ReadFile は DWORD (32bit)
//     単位でしか扱えないため一応 chunk ループを残す (assetpack と同じ流儀)。
#include "gameframework/SaveArchive.h"

#include "foundation/Platform.h"   // <windows.h>
#include "foundation/Log.h"
#include "memory/Memory.h"         // MemCopy / MemCmp / MemSet

namespace acs::game {

/** header 先頭に置く magic バイト列 "ACSSAVE\0" の実体。 */
const u8 FSaveArchive::kMagicBytes[FSaveArchive::kMagicSize] = {
    'A', 'C', 'S', 'S', 'A', 'V', 'E', '\0'
};

namespace {

/**
 * CRC32 (poly 0xEDB88320) の lookup table を返す。
 *
 * @details Meyer's singleton で thread-safe に 256 entry の table を遅延初期化する。
 * @return 256 要素の CRC32 lookup table。
 */
const u32* GetCrc32Table() noexcept
{
    struct FCrc32Table {
        FCrc32Table() noexcept
        {
            for (u32 Index = 0; Index < 256; ++Index) {
                u32 Value = Index;
                for (u32 Bit = 0; Bit < 8; ++Bit) {
                    Value = (Value & 1u) ? (0xEDB88320u ^ (Value >> 1)) : (Value >> 1);
                }
                Values[Index] = Value;
            }
        }

        u32 Values[256] = {};
    };

    static const FCrc32Table Table;
    return Table.Values;
}

/**
 * バイト列の CRC32 を計算する (Zlib / PNG 規約)。
 *
 * @param data 入力バイト列の先頭。
 * @param size 入力バイト数。
 * @return 計算した CRC32 値 (init/xorout 0xFFFFFFFF)。
 */
u32 ComputeCrc32(const void* data, u64 size) noexcept {
    const u32* table = GetCrc32Table();
    const u8*  p     = static_cast<const u8*>(data);
    u32        crc   = 0xFFFFFFFFu;
    for (u64 i = 0; i < size; ++i) {
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/**
 * u32 を little-endian で書き込む (strict-aliasing 安全)。
 *
 * @details reinterpret_cast での書き込みは UB なので MemCopy 経由でバイトコピーする。ホストも LE 前提。
 * @param dst 書き込み先 (4 バイト以上)。
 * @param v 書き込む値。
 */
void WriteU32LE(u8* dst, u32 v) noexcept { MemCopy(dst, &v, sizeof(u32)); }

/**
 * u64 を little-endian で書き込む (strict-aliasing 安全)。
 *
 * @details reinterpret_cast での書き込みは UB なので MemCopy 経由でバイトコピーする。ホストも LE 前提。
 * @param dst 書き込み先 (8 バイト以上)。
 * @param v 書き込む値。
 */
void WriteU64LE(u8* dst, u64 v) noexcept { MemCopy(dst, &v, sizeof(u64)); }

/**
 * little-endian の u32 を読み込む (strict-aliasing 安全)。
 *
 * @param src 読み込み元 (4 バイト以上)。
 * @return 読み込んだ u32 値。
 */
u32 ReadU32LE(const u8* src) noexcept {
    u32 v = 0; MemCopy(&v, src, sizeof(u32)); return v;
}

/**
 * little-endian の u64 を読み込む (strict-aliasing 安全)。
 *
 * @param src 読み込み元 (8 バイト以上)。
 * @return 読み込んだ u64 値。
 */
u64 ReadU64LE(const u8* src) noexcept {
    u64 v = 0; MemCopy(&v, src, sizeof(u64)); return v;
}

/**
 * ファイルポインタを先頭からの offset へ移動する。
 *
 * @param h 対象ファイルハンドル。
 * @param offset 先頭からのバイトオフセット。
 * @param err 失敗時に GetLastError を入れる出力引数 (成功時は 0)。
 * @return 成功なら true、失敗なら false。
 */
bool SeekTo(HANDLE h, u64 offset, DWORD& err) noexcept {
    err = 0;
    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(offset);
    if (!::SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) {
        err = ::GetLastError();
        return false;
    }
    return true;
}

/**
 * size バイトを完全に読み込む (DWORD 単位の chunk ループ)。
 *
 * @details >4GiB を扱うため DWORD 単位で分割読みする (assetpack と同流儀)。
 * @param h 対象ファイルハンドル。
 * @param dst 読み込み先バッファ。
 * @param size 読み込むバイト数。
 * @param err 失敗時に GetLastError (EOF 時は ERROR_HANDLE_EOF) を入れる出力引数。
 * @return 全バイト読めたら true、失敗なら false。
 */
bool ReadAll(HANDLE h, void* dst, u64 size, DWORD& err) noexcept {
    err = 0;
    if (size == 0) return true;
    u8* p = static_cast<u8*>(dst);
    u64 remaining = size;
    while (remaining > 0) {
        DWORD chunk = (remaining > 0x7FFFFFFFu)
                          ? 0x7FFFFFFFu
                          : static_cast<DWORD>(remaining);
        DWORD got = 0;
        if (!::ReadFile(h, p, chunk, &got, nullptr) || got == 0) {
            err = ::GetLastError();
            if (err == 0) err = ERROR_HANDLE_EOF;
            return false;
        }
        p += got;
        remaining -= got;
    }
    return true;
}

/**
 * size バイトを完全に書き込む (DWORD 単位の chunk ループ)。
 *
 * @details >4GiB を扱うため DWORD 単位で分割書きする (assetpack と同流儀)。
 * @param h 対象ファイルハンドル。
 * @param src 書き込むバイト列の先頭。
 * @param size 書き込むバイト数。
 * @param err 失敗時に GetLastError (部分書き込み時は ERROR_WRITE_FAULT) を入れる出力引数。
 * @return 全バイト書けたら true、失敗なら false。
 */
bool WriteAll(HANDLE h, const void* src, u64 size, DWORD& err) noexcept {
    err = 0;
    if (size == 0) return true;
    const u8* p = static_cast<const u8*>(src);
    u64 remaining = size;
    while (remaining > 0) {
        DWORD chunk = (remaining > 0x7FFFFFFFu)
                          ? 0x7FFFFFFFu
                          : static_cast<DWORD>(remaining);
        DWORD wrote = 0;
        if (!::WriteFile(h, p, chunk, &wrote, nullptr) || wrote != chunk) {
            err = ::GetLastError();
            if (err == 0) err = ERROR_WRITE_FAULT;
            return false;
        }
        p += wrote;
        remaining -= wrote;
    }
    return true;
}

/**
 * ESaveArchiveSubCode を FErrorCode.subcode 用の u16 に縮約する。
 *
 * @details
 * ESaveArchiveSubCode は u32 だが FErrorCode.subcode は u16。値は 1..7 のみなので
 * 情報落ちは無いが、明示的に縮約しておく。
 * @param sc 縮約するサブコード。
 * @return u16 に縮約したサブコード値。
 */
constexpr u16 SubU16(ESaveArchiveSubCode sc) noexcept {
    return static_cast<u16>(static_cast<u32>(sc));
}

/**
 * 24 バイトの header buffer を検証してフィールドを取り出す。
 *
 * @details magic が一致した場合のみ out_* を埋める。不一致時の out_* の値は不定。
 * @param header_buf 24 バイトの header バッファ。
 * @param out_version magic 一致時に version を受け取る出力引数。
 * @param out_payload_size magic 一致時に payload サイズを受け取る出力引数。
 * @param out_crc32 magic 一致時に payload の CRC32 を受け取る出力引数。
 * @return magic が一致すれば true、不一致なら false。
 */
bool ParseHeader(const u8* header_buf,
                 u32&      out_version,
                 u64&      out_payload_size,
                 u32&      out_crc32) noexcept {
    if (MemCmp(header_buf, FSaveArchive::kMagicBytes, FSaveArchive::kMagicSize) != 0) {
        return false;
    }
    out_version      = ReadU32LE(header_buf + 8);
    out_payload_size = ReadU64LE(header_buf + 12);
    out_crc32        = ReadU32LE(header_buf + 20);
    return true;
}

/**
 * ファイルを読み込み専用で開く (file not found は専用 subcode)。
 *
 * @details
 * CreateFileW の失敗を ERROR_FILE_NOT_FOUND とそれ以外で分け、上位のセーブ UI が
 * 「続きから表示」を出すか判断しやすくする。
 * @param file_path 開くファイルパス (nullptr はエラー)。
 * @return 成功なら開いたハンドルを持つ TResult、失敗ならエラー。
 */
TResult<HANDLE> OpenForRead(const wchar_t* file_path) noexcept {
    if (file_path == nullptr) {
        return ACS_ERR(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                       "FSaveArchive: file_path is null");
    }
    HANDLE h = ::CreateFileW(file_path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = ::GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubFileNotFound),
                              "FSaveArchive: file not found", err);
        }
        return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                          "FSaveArchive: CreateFileW (read) failed", err);
    }
    return TResult<HANDLE>(OkInit, h);
}

} // namespace

/**
 * header (24B) → payload の順で .acssave ファイルを書き出す。
 *
 * @details
 * 書き込み途中の失敗時 (停電 / disk full) はファイルが破損し得るが、atomic rename は
 * 呼び出し側の FSaveSlot が ".tmp" 経由で担当する設計。
 */
TResult<void> FSaveArchive::WriteToFile(const wchar_t* file_path,
                                      u32            version,
                                      const void*    payload,
                                      u64            payload_size) noexcept {
    if (file_path == nullptr) {
        return ACS_ERR(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                       "FSaveArchive::WriteToFile: file_path is null");
    }
    if (payload == nullptr && payload_size > 0) {
        return ACS_ERR(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                       "FSaveArchive::WriteToFile: payload is null but size > 0");
    }

    // CreateFileW(CREATE_ALWAYS) は既存ファイルを truncate して開く。
    HANDLE h = ::CreateFileW(file_path,
                             GENERIC_WRITE,
                             0,           // 排他: 書き込み中に他者が読めないように
                             nullptr,
                             CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = ::GetLastError();
        return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                          "FSaveArchive::WriteToFile: CreateFileW failed", err);
    }

    // header (24B) を build
    u8 header_buf[kHeaderSize] = {};
    MemCopy(header_buf, kMagicBytes, kMagicSize);
    WriteU32LE(header_buf + 8,  version);
    WriteU64LE(header_buf + 12, payload_size);
    const u32 crc = (payload_size > 0) ? ComputeCrc32(payload, payload_size)
                                       : 0xFFFFFFFFu ^ 0xFFFFFFFFu;  // 空 payload の CRC32 = 0
    WriteU32LE(header_buf + 20, crc);

    DWORD err = 0;
    if (!WriteAll(h, header_buf, kHeaderSize, err)) {
        ::CloseHandle(h);
        return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                          "FSaveArchive::WriteToFile: WriteFile (header) failed", err);
    }

    // payload (size==0 でも noop で良い)
    if (payload_size > 0) {
        if (!WriteAll(h, payload, payload_size, err)) {
            ::CloseHandle(h);
            return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                              "FSaveArchive::WriteToFile: WriteFile (payload) failed",
                              err);
        }
    }

    // CloseHandle は flush も兼ねる (OS buffer cache に書き出すだけで、
    // 真の persistence は FlushFileBuffers が必要だが、ここでは呼ばない —
    // 上位 FSaveSlot 層が rename を行う前に明示 flush することを期待)。
    if (!::CloseHandle(h)) {
        DWORD close_err = ::GetLastError();
        return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                          "FSaveArchive::WriteToFile: CloseHandle failed", close_err);
    }
    return Ok();
}

/**
 * .acssave ファイルを読み込んで payload を out_payload に展開する。
 *
 * @details
 * header (24B) を読んで magic + version + payload_size + crc を取り出し、file_size 検査 →
 * version 検査 → 容量検査 → payload 読込 → CRC 検証の順で fail-fast する。
 */
TResult<u32> FSaveArchive::ReadFromFile(const wchar_t* file_path,
                                      void*          out_payload,
                                      u64            out_capacity,
                                      u32            expected_version,
                                      u64&           out_payload_size) noexcept {
    out_payload_size = 0;

    if (out_payload == nullptr && out_capacity > 0) {
        return ACS_ERR(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                       "FSaveArchive::ReadFromFile: out_payload is null but capacity > 0");
    }

    const auto open_r = OpenForRead(file_path);
    if (open_r.IsErr()) return open_r.Error();
    HANDLE h = open_r.Value();

    // file_size の sanity check
    LARGE_INTEGER fsize{};
    if (!::GetFileSizeEx(h, &fsize)) {
        DWORD err = ::GetLastError();
        ::CloseHandle(h);
        return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                          "FSaveArchive::ReadFromFile: GetFileSizeEx failed", err);
    }
    const u64 file_size = static_cast<u64>(fsize.QuadPart);
    if (file_size < kHeaderSize) {
        ::CloseHandle(h);
        return ACS_ERR(IO, SubU16(ESaveArchiveSubCode::kSubBadMagic),
                       "FSaveArchive::ReadFromFile: file smaller than header");
    }

    // header (24B) を読む
    u8 header_buf[kHeaderSize] = {};
    DWORD err = 0;
    if (!ReadAll(h, header_buf, kHeaderSize, err)) {
        ::CloseHandle(h);
        return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                          "FSaveArchive::ReadFromFile: ReadFile (header) failed", err);
    }

    u32 version       = 0;
    u64 payload_size  = 0;
    u32 expected_crc  = 0;
    if (!ParseHeader(header_buf, version, payload_size, expected_crc)) {
        ::CloseHandle(h);
        return ACS_ERR(Asset, SubU16(ESaveArchiveSubCode::kSubBadMagic),
                       "FSaveArchive::ReadFromFile: magic mismatch (not an .acssave)");
    }

    // payload_size が file_size - kHeaderSize と一致するか
    if (payload_size > file_size - kHeaderSize) {
        ::CloseHandle(h);
        return ACS_ERR(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                       "FSaveArchive::ReadFromFile: payload_size > file_size - header");
    }

    out_payload_size = payload_size;

    // version 不一致は kSubMigrationNeeded で先に返す。
    // 「buffer に書き込まない」のが migration ハンドラから見ると最も扱いやすい。
    if (version != expected_version) {
        ACS_LOG_WARN("FSaveArchive::ReadFromFile: version mismatch (file=%u, expected=%u)",
                     version, expected_version);
        ::CloseHandle(h);
        return ACS_ERR(Asset, SubU16(ESaveArchiveSubCode::kSubMigrationNeeded),
                       "FSaveArchive::ReadFromFile: version mismatch (migration needed)");
    }

    // buffer 容量 check
    if (out_capacity < payload_size) {
        ::CloseHandle(h);
        return ACS_ERR(IO, SubU16(ESaveArchiveSubCode::kSubBufferTooSmall),
                       "FSaveArchive::ReadFromFile: out_capacity < payload_size");
    }

    // payload を out_payload に読み込む
    if (payload_size > 0) {
        if (!ReadAll(h, out_payload, payload_size, err)) {
            ::CloseHandle(h);
            return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                              "FSaveArchive::ReadFromFile: ReadFile (payload) failed", err);
        }
    }

    // CRC32 検証
    const u32 actual_crc = (payload_size > 0) ? ComputeCrc32(out_payload, payload_size)
                                              : (0xFFFFFFFFu ^ 0xFFFFFFFFu);
    if (actual_crc != expected_crc) {
        ACS_LOG_WARN("FSaveArchive::ReadFromFile: CRC mismatch (expected=0x%08x, actual=0x%08x)",
                     expected_crc, actual_crc);
        ::CloseHandle(h);
        return ACS_ERR(Asset, SubU16(ESaveArchiveSubCode::kSubChecksumFail),
                       "FSaveArchive::ReadFromFile: CRC32 mismatch (corrupt or tampered)");
    }

    ::CloseHandle(h);
    return TResult<u32>(OkInit, version);
}

/** header (24B) のみ読み、payload を読まずに version を返す。 */
TResult<u32> FSaveArchive::PeekVersion(const wchar_t* file_path) noexcept {
    const auto open_r = OpenForRead(file_path);
    if (open_r.IsErr()) return open_r.Error();
    HANDLE h = open_r.Value();

    u8 header_buf[kHeaderSize] = {};
    DWORD err = 0;
    if (!ReadAll(h, header_buf, kHeaderSize, err)) {
        ::CloseHandle(h);
        return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                          "FSaveArchive::PeekVersion: ReadFile (header) failed", err);
    }
    ::CloseHandle(h);

    u32 version = 0, dummy_crc = 0;
    u64 dummy_size = 0;
    if (!ParseHeader(header_buf, version, dummy_size, dummy_crc)) {
        return ACS_ERR(Asset, SubU16(ESaveArchiveSubCode::kSubBadMagic),
                       "FSaveArchive::PeekVersion: magic mismatch (not an .acssave)");
    }
    return TResult<u32>(OkInit, version);
}

/** header (24B) のみ読み、payload を読まずに payload_size を返す。 */
TResult<u64> FSaveArchive::PeekPayloadSize(const wchar_t* file_path) noexcept {
    const auto open_r = OpenForRead(file_path);
    if (open_r.IsErr()) return open_r.Error();
    HANDLE h = open_r.Value();

    u8 header_buf[kHeaderSize] = {};
    DWORD err = 0;
    if (!ReadAll(h, header_buf, kHeaderSize, err)) {
        ::CloseHandle(h);
        return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                          "FSaveArchive::PeekPayloadSize: ReadFile (header) failed", err);
    }
    ::CloseHandle(h);

    u32 dummy_version = 0, dummy_crc = 0;
    u64 payload_size  = 0;
    if (!ParseHeader(header_buf, dummy_version, payload_size, dummy_crc)) {
        return ACS_ERR(Asset, SubU16(ESaveArchiveSubCode::kSubBadMagic),
                       "FSaveArchive::PeekPayloadSize: magic mismatch (not an .acssave)");
    }
    return TResult<u64>(OkInit, payload_size);
}

} // namespace acs::game
