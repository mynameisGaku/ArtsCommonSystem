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

#include "assetpack/AcpakCrypto.h" // Phase 2: AES-256-GCM 暗号化
#include "assetpack/AcpakLz4.h"    // Phase 2: LZ4 圧縮

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

    // 鍵 defensive zero
    MemSet(_key.bytes, 0, sizeof(_key.bytes));
    _has_key = false;
}

void AcpakWriter::SetKey(const AcpakKey& key) noexcept {
    MemCopy(_key.bytes, key.bytes, sizeof(_key.bytes));
    _has_key = true;
}

TResult<void> AcpakWriter::Open(const wchar_t* output_path, EAcpakFlags flags) noexcept {
    if (_file_handle != nullptr) {
        return ACS_ERR(IO, kAcpakSubAlreadyOpen,
                       "AcpakWriter::Open: writer already open");
    }
    if (output_path == nullptr) {
        return ACS_ERR(IO, kAcpakSubIOFailure,
                       "AcpakWriter::Open: output_path is null");
    }

    // Phase 2: encrypted / compressed は実装済。未知 flag bit のみ拒否。
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

TResult<void> AcpakWriter::AddFile(const wchar_t* virtual_path,
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
// -----------------------------------------------------------------------------
// Phase 2 では各 entry に対し **compress-then-encrypt** パイプラインを通す。
//   1. (元データ ptr, size) → CRC32 計算 (元バイト基準)
//   2. compressed 立ち & 圧縮効果あり (< original * 0.97) → LZ4 圧縮
//      圧縮効果が薄ければ生格納フォールバック (AcpakFileEntry::size_stored ==
//      size_uncompressed のまま)。この判断は per-entry に独立。
//      ※ AssetPack.md §5 の "97%" 安全規則。アーカイブがバラより大きくなる
//          ことを防ぐ。
//   3. encrypted 立ち → CSPRNG nonce 生成 → AES-256-GCM 暗号化 (tag 出力)
//   4. ディスクに stored bytes を書く
//   5. file table に (offset, size_unc, size_st, crc32, nonce?, tag?) を書く
//
// 注意: 圧縮判定は per-entry なので、同 .acpak 内でも一部は LZ4、一部は生格納
// と混在しうる。Reader は size_stored / size_uncompressed の値だけで pipeline
// を構成できる (圧縮しないエントリは LZ4 ステージが no-op になる)。
//
// 圧縮 + 暗号化が両方立っている場合のフォールバック:
//   ・LZ4 で生格納に倒れた entry でも、暗号化は通常通り行う (size_stored ==
//     size_uncompressed のままで AES-GCM)。
//   ・Reader は size_stored == size_uncompressed のとき LZ4 をスキップする
//     pipeline になっている (= AcpakReader::ReadFile の is_compressed
//     ブロックで full_match 0 のときも単に noop)。
//   ・ただし現実装の Reader はファイル単位の per-entry "compressed?" 判定を
//     持たない (header flags のみ参照)。そこで Reader は「compressed flag が
//     立っていれば常に LZ4 Decompress を試みる」設計。
//     → そのため Writer 側は「compressed flag 立ち時は entry が短くても
//       必ず LZ4 を通し、たとえ size_stored > size_uncompressed でも保存する」
//       挙動とする。"97%" 安全規則は Phase 3 で per-entry flag を追加して
//       再検討する (今は単純化を優先)。
TResult<void> AcpakWriter::Finalize() noexcept {
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

    const bool is_encrypted  = (_flags & static_cast<u32>(AcpakFlagEncrypted))  != 0u;
    const bool is_compressed = (_flags & static_cast<u32>(AcpakFlagCompressed)) != 0u;

    // 暗号化が立っているのに鍵未設定なら早期に弾く (失敗のソースを明確化)。
    if (is_encrypted && !_has_key) {
        return ACS_ERR(Asset, kAcpakSubCryptoKey,
                       "AcpakWriter::Finalize: encrypted flag set but no key");
    }

    // 各 entry の (offset, size_unc, size_st, crc32, nonce[12], tag[16]) を控える。
    struct WrittenEntry {
        u64 offset;
        u64 size_unc;             // 元データのバイト数
        u64 size_stored;          // ディスク上のバイト数 (圧縮+暗号化済)
        u32 crc32;
        u8  cipher_nonce[12];
        u8  cipher_tag[16];
    };
    TArray<WrittenEntry> written;
    written.Reserve(_pending.Size());

    // 中間バッファは entry 間で再利用する (TArray<u8> を 2 つ用意)。
    TArray<u8> stage_compress;   // LZ4 圧縮出力先
    TArray<u8> stage_encrypt;    // AES-GCM 暗号化出力先 (in-place 可だが安全のため別)

    // ---- ファイルデータ書き出し -------------------------------------------
    for (usize i = 0; i < _pending.Size(); ++i) {
        const PendingEntry& p = _pending[i];

        WrittenEntry w{};
        w.offset   = Tell(h);
        w.size_unc = p.size;
        w.crc32    = ComputeCrc32(p.data, p.size);

        // ステージごとの (data, size) を pipeline で更新していく。
        const u8* stage_ptr  = static_cast<const u8*>(p.data);
        u64       stage_size = p.size;

        // ---- 圧縮 (compress-then-encrypt の 1 段目) --------------------
        if (is_compressed) {
            if (p.size > 0xFFFFFFFFu) {
                return ACS_ERR(Asset, kAcpakSubBadSize,
                               "AcpakWriter::Finalize: input > 4GiB (LZ4 limit)");
            }
            const u32 src_size = static_cast<u32>(p.size);
            const u32 dst_cap  = AcpakLz4::MaxCompressedSize(src_size);
            stage_compress.Resize(dst_cap);
            auto cr = AcpakLz4::Compress(static_cast<const u8*>(p.data),
                                         src_size,
                                         stage_compress.Data(),
                                         dst_cap);
            if (cr.IsErr()) return cr.Error();
            stage_ptr  = stage_compress.Data();
            stage_size = cr.Value();
        }

        // ---- 暗号化 (compress-then-encrypt の 2 段目) -------------------
        if (is_encrypted) {
            // CSPRNG nonce 生成
            AcpakCrypto::GenerateRandomNonce(w.cipher_nonce);

            // 出力バッファ (= 同 size の別領域、in-place も可だが分離で安全)
            stage_encrypt.Resize(static_cast<usize>(stage_size));
            auto er = AcpakCrypto::Encrypt(_key,
                                           w.cipher_nonce,
                                           stage_ptr, stage_size,
                                           stage_encrypt.Data(),
                                           w.cipher_tag);
            if (er.IsErr()) return er.Error();
            stage_ptr  = stage_encrypt.Data();
            // size 不変 (AES-GCM は size == size)
        }

        w.size_stored = stage_size;

        // ---- ディスク書き出し ----------------------------------------
        if (stage_size > 0) {
            if (!WriteAll(h, stage_ptr, stage_size, err)) {
                return ACS_ERR_OS(IO, kAcpakSubIOFailure,
                                  "AcpakWriter::Finalize: WriteFile (data) failed", err);
            }
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
        // Phase 2: 暗号化のとき末尾に cipher_nonce(12) + cipher_tag(16) を続ける。
        const u64 path_bytes  = static_cast<u64>(len) * sizeof(wchar_t);
        const u64 tail_bytes  = is_encrypted ? (28u + kAcpakCipherFieldsDiskSize)
                                              : 28u;
        const u64 entry_bytes = 4u + path_bytes + tail_bytes;

        u8  stack_buf[1024];
        u8* buf = stack_buf;
        TArray<u8> heap_buf;
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
        WriteU64LE(tail + 8,  w.size_unc);     // size_uncompressed
        WriteU64LE(tail + 16, w.size_stored);  // size_stored (Phase 2: 圧縮+暗号化後)
        WriteU32LE(tail + 24, w.crc32);
        if (is_encrypted) {
            MemCopy(tail + 28,      w.cipher_nonce, 12);
            MemCopy(tail + 28 + 12, w.cipher_tag,   16);
        }

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
