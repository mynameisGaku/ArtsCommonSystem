// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS AssetPack — AcpakWriter 実装 (Win32 I/O + CRC32 計算)
// -----------------------------------------------------------------------------
// 書き出しレイアウト:
//   1. ヘッダ (36 バイト固定、AcpakFormat.h の kAcpakHeaderDiskSize 参照)
//      ・file_table_offset と file_count は Finalize 後にしか確定できないため、
//        Open 時点ではプレースホルダ 0 で書き、Finalize で再度 seek + 上書き
//        する 2 パス書き込みを行う。
//   2. file 0 の生バイト、file 1 の生バイト、...
//      ・各ファイルの offset / crc32 をオンザフライで計算しながら書く。
//   3. file table (各 entry: path_len(4) + path(path_len*2) + offset(8) +
//      size_uncompressed(8) + size_stored(8) + crc32(4))
//   4. ヘッダ書き戻し (file_count + file_table_offset)
//
// Phase 1 制約:
//   ・flags = AcpakFlagNone のみ実装。Encrypted/Compressed bit を渡すと
//     NotImplemented を返す (Open 時点で即弾く)。
//   ・size_stored == size_uncompressed (= 生バイトをそのまま書く)。
//   ・data の所有権は呼び出し側に残る — Writer はコピーを取らない。
//     呼び出し側は Finalize 完了まで data ポインタを生かしておくこと。
// =============================================================================
#include "assetpack/AcpakWriter.h"

#include "foundation/Platform.h"
#include "foundation/Log.h"
#include "memory/Memory.h"

namespace acs::assetpack {

// ============================================================================
// 名前無し名前空間: CRC32 + Win32 write helper
// ============================================================================
namespace {

// AcpakReader.cpp と同じ実装。link 単位を分けているので重複する。
// (Hash.cpp に共通 CRC32 を出す案は Phase 2 でやる)
const u32* GetCrc32Table() noexcept {
    static u32 _table[256] = {};
    static bool _initialized = false;
    if (!_initialized) {
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (u32 k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            _table[i] = c;
        }
        _initialized = true;
    }
    return _table;
}

u32 ComputeCrc32(const void* data, u64 size) noexcept {
    const u32* table = GetCrc32Table();
    const u8*  p     = static_cast<const u8*>(data);
    u32        crc   = 0xFFFFFFFFu;
    for (u64 i = 0; i < size; ++i) {
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

// 現在の file pointer 位置 (= これから書く位置 = "tell") を取る。
u64 Tell(HANDLE h) noexcept {
    LARGE_INTEGER zero{};
    LARGE_INTEGER cur{};
    if (!::SetFilePointerEx(h, zero, &cur, FILE_CURRENT)) {
        return 0;
    }
    return static_cast<u64>(cur.QuadPart);
}

// 指定位置に SeekFile。失敗時 false。
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

// ReadFile と同様、>4GiB を考慮した WriteFile ラッパ。
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

// ---- little-endian バイト列書き込み (strict-aliasing 安全) ----------------
void WriteU32LE(u8* dst, u32 v) noexcept {
    MemCopy(dst, &v, sizeof(u32));
}

void WriteU64LE(u8* dst, u64 v) noexcept {
    MemCopy(dst, &v, sizeof(u64));
}

// 自前 wcslen (依存ヘッダを増やさない)。
u32 LenW(const wchar_t* s) noexcept {
    u32 n = 0;
    if (s == nullptr) return 0;
    while (s[n]) ++n;
    return n;
}

} // namespace

// ============================================================================
// AcpakWriter 実装
// ============================================================================

AcpakWriter::~AcpakWriter() noexcept {
    Close();
}

void AcpakWriter::Close() noexcept {
    if (_file_handle != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(_file_handle));
        _file_handle = nullptr;
    }
    ResetState();
}

void AcpakWriter::ResetState() noexcept {
    _flags     = 0;
    _finalized = false;
    _pending.Clear();
}

Result<void> AcpakWriter::Open(const wchar_t* output_path, EAcpakFlags flags) noexcept {
    if (_file_handle != nullptr) {
        return ACS_ERR(IO, kAcpakSubAlreadyOpen,
                       "AcpakWriter::Open: writer already open");
    }
    if (output_path == nullptr) {
        return ACS_ERR(IO, kAcpakSubIOFailure,
                       "AcpakWriter::Open: output_path is null");
    }

    // Phase 1: encrypted / compressed は未実装
    if (static_cast<u32>(flags) & (static_cast<u32>(AcpakFlagEncrypted) |
                                   static_cast<u32>(AcpakFlagCompressed))) {
        return ACS_ERR(Asset, kAcpakSubNotImplemented,
                       "AcpakWriter::Open: encrypted/compressed not implemented (Phase 2)");
    }
    // 未知 flag bit があれば拒否
    const u32 known_flags = static_cast<u32>(AcpakFlagEncrypted) |
                            static_cast<u32>(AcpakFlagCompressed);
    if ((static_cast<u32>(flags) & ~known_flags) != 0) {
        return ACS_ERR(Asset, kAcpakSubBadFlags,
                       "AcpakWriter::Open: unknown flag bits");
    }

    HANDLE h = ::CreateFileW(output_path, GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return ACS_ERR_OS(IO, kAcpakSubIOFailure,
                          "AcpakWriter::Open: CreateFileW failed",
                          ::GetLastError());
    }

    _file_handle = h;
    _flags       = static_cast<u32>(flags);
    _finalized   = false;
    _pending.Clear();

    // ---- ヘッダプレースホルダを書く (Finalize で上書きする) -----------------
    u8 header[kAcpakHeaderDiskSize] = {};
    MemCopy(header, kAcpakMagic, 8);
    WriteU32LE(header + 8,  kAcpakVersion);
    WriteU32LE(header + 12, _flags);
    WriteU32LE(header + 16, 0);            // file_count placeholder
    WriteU32LE(header + 20, 0);            // padding = 0
    WriteU64LE(header + 24, 0);            // file_table_offset placeholder
    WriteU32LE(header + 32, 0);            // reserved = 0

    DWORD err = 0;
    if (!WriteAll(h, header, kAcpakHeaderDiskSize, err)) {
        ::CloseHandle(h);
        _file_handle = nullptr;
        ResetState();
        return ACS_ERR_OS(IO, kAcpakSubIOFailure,
                          "AcpakWriter::Open: WriteFile (header) failed", err);
    }

    return Ok();
}

Result<void> AcpakWriter::AddFile(const wchar_t* virtual_path,
                                  const void*    data,
                                  u64            size) noexcept {
    if (_file_handle == nullptr) {
        return ACS_ERR(IO, kAcpakSubNotOpen,
                       "AcpakWriter::AddFile: writer not open");
    }
    if (_finalized) {
        return ACS_ERR(IO, kAcpakSubNotOpen,
                       "AcpakWriter::AddFile: writer already finalized");
    }
    if (virtual_path == nullptr) {
        return ACS_ERR(IO, kAcpakSubIOFailure,
                       "AcpakWriter::AddFile: virtual_path is null");
    }
    if (data == nullptr && size > 0) {
        return ACS_ERR(IO, kAcpakSubIOFailure,
                       "AcpakWriter::AddFile: data is null but size > 0");
    }

    PendingEntry e{};
    e.path = virtual_path;
    e.data = data;
    e.size = size;
    _pending.PushBack(e);
    return Ok();
}

// ----------------------------------------------------------------------------
// Finalize — pending entry 群をディスクに書き出し、最後にヘッダを更新
// ----------------------------------------------------------------------------
Result<void> AcpakWriter::Finalize() noexcept {
    if (_file_handle == nullptr) {
        return ACS_ERR(IO, kAcpakSubNotOpen,
                       "AcpakWriter::Finalize: writer not open");
    }
    if (_finalized) {
        return ACS_ERR(IO, kAcpakSubNotOpen,
                       "AcpakWriter::Finalize: already finalized");
    }

    HANDLE h = static_cast<HANDLE>(_file_handle);
    DWORD  err = 0;

    // ---- 既にヘッダ 36B は Open で書いてあり、file pointer は 36 にある -----
    // 念のため明示的に seek (古いヘッダ書き込みが flush されていない場合に
    // file pointer が想定外位置にいるリスクを下げる)。
    if (!SeekTo(h, kAcpakHeaderDiskSize, err)) {
        return ACS_ERR_OS(IO, kAcpakSubIOFailure,
                          "AcpakWriter::Finalize: SetFilePointerEx failed", err);
    }

    // 各 entry の offset と crc32 を控える小さな構造体。
    struct WrittenEntry {
        u64 offset;
        u64 size;
        u32 crc32;
    };
    Array<WrittenEntry> written;
    written.Reserve(_pending.Size());

    // ---- ファイルデータ書き出し -------------------------------------------
    for (usize i = 0; i < _pending.Size(); ++i) {
        const PendingEntry& p = _pending[i];

        WrittenEntry w{};
        w.offset = Tell(h);
        w.size   = p.size;
        w.crc32  = ComputeCrc32(p.data, p.size);

        if (!WriteAll(h, p.data, p.size, err)) {
            return ACS_ERR_OS(IO, kAcpakSubIOFailure,
                              "AcpakWriter::Finalize: WriteFile (data) failed", err);
        }
        written.PushBack(w);
    }

    // ---- file table 書き出し ---------------------------------------------
    const u64 file_table_offset = Tell(h);

    for (usize i = 0; i < _pending.Size(); ++i) {
        const PendingEntry& p    = _pending[i];
        const WrittenEntry&  w   = written[i];
        const u32            len = LenW(p.path);

        // path_len (4) + path (len*2) + offset (8) + size_unc (8) + size_st (8)
        // + crc32 (4) を 1 つの一時バッファに詰めて書く。
        // long path 対応で path_bytes は可変なので、大きめのスタックバッファ
        // (260 wchar = 520 + 32 = 552B) を使う。それ以上は heap で。
        const u64 path_bytes  = static_cast<u64>(len) * sizeof(wchar_t);
        const u64 entry_bytes = 4u + path_bytes + 28u;

        u8  stack_buf[1024];
        u8* buf = stack_buf;
        Array<u8> heap_buf;
        if (entry_bytes > sizeof(stack_buf)) {
            heap_buf.Resize(static_cast<usize>(entry_bytes));
            buf = heap_buf.Data();
        }

        WriteU32LE(buf + 0, len);
        if (path_bytes > 0) {
            MemCopy(buf + 4, p.path, static_cast<usize>(path_bytes));
        }
        u8* tail = buf + 4 + path_bytes;
        WriteU64LE(tail + 0,  w.offset);
        WriteU64LE(tail + 8,  w.size);  // size_uncompressed
        WriteU64LE(tail + 16, w.size);  // size_stored (Phase 1: 同値)
        WriteU32LE(tail + 24, w.crc32);

        if (!WriteAll(h, buf, entry_bytes, err)) {
            return ACS_ERR_OS(IO, kAcpakSubIOFailure,
                              "AcpakWriter::Finalize: WriteFile (entry) failed", err);
        }
    }

    // ---- ヘッダ書き戻し (file_count + file_table_offset を確定) ------------
    if (!SeekTo(h, 0, err)) {
        return ACS_ERR_OS(IO, kAcpakSubIOFailure,
                          "AcpakWriter::Finalize: SetFilePointerEx (rewind) failed", err);
    }

    u8 header[kAcpakHeaderDiskSize] = {};
    MemCopy(header, kAcpakMagic, 8);
    WriteU32LE(header + 8,  kAcpakVersion);
    WriteU32LE(header + 12, _flags);
    WriteU32LE(header + 16, static_cast<u32>(_pending.Size()));
    WriteU32LE(header + 20, 0);            // padding = 0
    WriteU64LE(header + 24, file_table_offset);
    WriteU32LE(header + 32, 0);            // reserved = 0

    if (!WriteAll(h, header, kAcpakHeaderDiskSize, err)) {
        return ACS_ERR_OS(IO, kAcpakSubIOFailure,
                          "AcpakWriter::Finalize: WriteFile (header rewrite) failed", err);
    }

    // ディスクに反映 (クラッシュ耐性は完璧ではないが、最低限 flush 要求)
    ::FlushFileBuffers(h);

    _finalized = true;
    return Ok();
}

} // namespace acs::assetpack
