// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework Pillar J — FSaveArchive 実装 (Win32 I/O + CRC32)
// -----------------------------------------------------------------------------
// FSaveArchive.h で宣言した 4 メソッド (WriteToFile / ReadFromFile /
// PeekVersion / PeekPayloadSize) の本体。Win32 ファイル API を直接叩いて
// `.acssave` の bit-precise format を読み書きする。
//
// 実装上のポイント:
//   ・<windows.h> は foundation/Platform.h 経由で 1 箇所に閉じ込める
//     (Yield / CreateFile / GetMessage 等のマクロ汚染対策はここで吸収済)。
//   ・CRC32 ルーチンは assetpack/FAcpakReader.cpp / FAcpakWriter.cpp と同じ実装
//     (poly 0xEDB88320, init/xorout 0xFFFFFFFF, table 256 entry)。将来 Hash.cpp
//     に共通化する余地は残しているが、Phase 1 では link 単位を独立させたい
//     (assetpack を依存に持たないテストビルドでも FSaveArchive を使えるように)
//     ため、ここでも単独に持つ。
//   ・little-endian 読み書きは MemCopy 経由で strict-aliasing 違反を避ける。
//     ACS 対応プラットフォームは Win/x64 と ARM64 (LE) のみ前提。
//   ・>4GiB の payload を扱う想定は無いが、WriteFile/ReadFile は DWORD (32bit)
//     単位でしか扱えないため一応 chunk ループを残す (assetpack と同じ流儀)。
// =============================================================================
#include "gameframework/SaveArchive.h"

#include "foundation/Platform.h"   // <windows.h>
#include "foundation/Log.h"
#include "memory/Memory.h"         // MemCopy / MemCmp / MemSet

namespace acs::game {

// ============================================================================
// magic バイト列の定義 (header に置く 8 バイト = ASCII "ACSSAVE\0")
// ============================================================================
const u8 FSaveArchive::kMagicBytes[FSaveArchive::kMagicSize] = {
    'A', 'C', 'S', 'S', 'A', 'V', 'E', '\0'
};

// ============================================================================
// 名前無し名前空間: CRC32 + Win32 helper
// ============================================================================
namespace {

// ---- CRC32 (poly 0xEDB88320, init/xorout 0xFFFFFFFF) ----------------------
// Meyer's singleton で thread-safe な lookup table 初期化を行う。
// (assetpack/FAcpakReader.cpp と同一実装)
const u32* GetCrc32Table() noexcept {
    static u32 m_Table[256] = {};
    static bool m_Initialized = false;
    if (!m_Initialized) {
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (u32 k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            m_Table[i] = c;
        }
        m_Initialized = true;
    }
    return m_Table;
}

// バイト列の CRC32 を計算する (Zlib / PNG 規約)。
u32 ComputeCrc32(const void* data, u64 size) noexcept {
    const u32* table = GetCrc32Table();
    const u8*  p     = static_cast<const u8*>(data);
    u32        crc   = 0xFFFFFFFFu;
    for (u64 i = 0; i < size; ++i) {
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

// ---- little-endian 読み書き helper (strict-aliasing 安全) -----------------
// reinterpret_cast での読み書きは UB なので、MemCopy 経由でバイトコピーする。
// ホスト側も LE 前提なので memcpy された生バイトがそのまま整数になる。
void WriteU32LE(u8* dst, u32 v) noexcept { MemCopy(dst, &v, sizeof(u32)); }
void WriteU64LE(u8* dst, u64 v) noexcept { MemCopy(dst, &v, sizeof(u64)); }

u32 ReadU32LE(const u8* src) noexcept {
    u32 v = 0; MemCopy(&v, src, sizeof(u32)); return v;
}
u64 ReadU64LE(const u8* src) noexcept {
    u64 v = 0; MemCopy(&v, src, sizeof(u64)); return v;
}

// ---- Win32 read/write/seek ラッパ -----------------------------------------
// >4GiB を扱うため DWORD 単位の chunk ループにしておく (assetpack と同流儀)。
// 成功で true、失敗で err に GetLastError を入れて false。

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

// ---- enum class → u16 (FErrorCode.subcode に入れるための reduction) ------
// ESaveArchiveSubCode は u32 だが FErrorCode.subcode は u16。Phase 1 の値は
// 1..7 のみなので無問題だが、明示的に縮約しておく。
constexpr u16 SubU16(ESaveArchiveSubCode sc) noexcept {
    return static_cast<u16>(static_cast<u32>(sc));
}

// ---- header buffer (24 バイト) のパースを 1 箇所に集約 -------------------
// 戻り値:
//   true  — magic 一致、header.payload_size / header.version を out に詰める
//   false — magic 不一致 (out_version / out_payload_size の値は不定)
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

// ---- open helper: ファイルを read で開く (file not found は専用 subcode) ---
// CreateFileW の失敗を ERROR_FILE_NOT_FOUND と他で分け、より上位のセーブ UI
// が「continue 表示」を出すかを判断しやすくする。
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

// ============================================================================
// FSaveArchive::WriteToFile
// ----------------------------------------------------------------------------
// header (24B) → payload (payload_size B) の順で書く。書き込み途中の失敗時
// (= electricity loss / disk full) はファイルが破損する可能性があるが、
// atomic rename は呼び出し側の FSaveSlot が ".tmp" 経由で担当する設計。
// ============================================================================
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

    // ---- header (24B) を build ------------------------------------------
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

    // ---- payload (size==0 でも noop で良い) -----------------------------
    if (payload_size > 0) {
        if (!WriteAll(h, payload, payload_size, err)) {
            ::CloseHandle(h);
            return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                              "FSaveArchive::WriteToFile: WriteFile (payload) failed",
                              err);
        }
    }

    // CloseHandle は flush も兼ねる (OS buffer cache に書き出すだけで、
    // 真の persistence は FlushFileBuffers が必要だが、Phase 1 では呼ばない —
    // 上位 FSaveSlot 層が rename を行う前に明示 flush することを期待)。
    if (!::CloseHandle(h)) {
        DWORD close_err = ::GetLastError();
        return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                          "FSaveArchive::WriteToFile: CloseHandle failed", close_err);
    }
    return Ok();
}

// ============================================================================
// FSaveArchive::ReadFromFile
// ----------------------------------------------------------------------------
// header (24B) を読んで magic + version + payload_size + crc を取り出し、
// out_capacity 検査 → version 検査 → payload 読込 → CRC 計算 → 一致確認、
// の順で fail-fast する。
// ============================================================================
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

    auto open_r = OpenForRead(file_path);
    if (open_r.IsErr()) return open_r.Error();
    HANDLE h = open_r.Value();

    // ---- file_size の sanity check ------------------------------------
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

    // ---- header (24B) を読む -------------------------------------------
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

    // ---- version 不一致は kSubMigrationNeeded で先に返す ---------------
    // 「buffer に書き込まない」のが migration ハンドラから見ると最も扱いやすい。
    if (version != expected_version) {
        ACS_LOG_WARN("FSaveArchive::ReadFromFile: version mismatch (file=%u, expected=%u)",
                     version, expected_version);
        ::CloseHandle(h);
        return ACS_ERR(Asset, SubU16(ESaveArchiveSubCode::kSubMigrationNeeded),
                       "FSaveArchive::ReadFromFile: version mismatch (migration needed)");
    }

    // ---- buffer 容量 check --------------------------------------------
    if (out_capacity < payload_size) {
        ::CloseHandle(h);
        return ACS_ERR(IO, SubU16(ESaveArchiveSubCode::kSubBufferTooSmall),
                       "FSaveArchive::ReadFromFile: out_capacity < payload_size");
    }

    // ---- payload を out_payload に読み込む -----------------------------
    if (payload_size > 0) {
        if (!ReadAll(h, out_payload, payload_size, err)) {
            ::CloseHandle(h);
            return ACS_ERR_OS(IO, SubU16(ESaveArchiveSubCode::kSubIoError),
                              "FSaveArchive::ReadFromFile: ReadFile (payload) failed", err);
        }
    }

    // ---- CRC32 検証 ----------------------------------------------------
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

// ============================================================================
// FSaveArchive::PeekVersion — header のみ読んで version を返す
// ============================================================================
TResult<u32> FSaveArchive::PeekVersion(const wchar_t* file_path) noexcept {
    auto open_r = OpenForRead(file_path);
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

// ============================================================================
// FSaveArchive::PeekPayloadSize — header のみ読んで payload_size を返す
// ============================================================================
TResult<u64> FSaveArchive::PeekPayloadSize(const wchar_t* file_path) noexcept {
    auto open_r = OpenForRead(file_path);
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
