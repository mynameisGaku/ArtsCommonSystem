// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS FAssetPack — FAcpakWriter 実装 (Win32 I/O + CRC32 計算)
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
// 制約:
//   ・data の所有権は呼び出し側に残る — Writer はコピーを取らない。
//     呼び出し側は Finalize 完了まで data ポインタを生かしておくこと。
// =============================================================================
#include "assetpack/AcpakWriter.h"

#include "foundation/Platform.h"
#include "foundation/Log.h"
#include "memory/Memory.h"

#include "assetpack/AcpakCrypto.h" // AES-256-GCM 暗号化
#include "assetpack/AcpakLz4.h"    // LZ4 圧縮

namespace acs::assetpack {

namespace {

/**
 * CRC32 lookup table (poly 0xEDB88320) を遅延構築して返す。
 *
 * @details
 * FAcpakReader.cpp と同じ実装。link 単位を分けているので重複する。256-entry
 * table を最初の呼び出し時に組み立てる (Meyers singleton)。
 * @return 256 要素の CRC32 lookup table。
 */
const u32* GetCrc32Table() noexcept {
    static u32 table[256] = {};
    static bool initialized = false;
    if (!initialized) {
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (u32 k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        initialized = true;
    }
    return table;
}

/**
 * バイト列の CRC32 を計算する。
 *
 * @details init = 0xFFFFFFFF, xorout = 0xFFFFFFFF (Zlib / PNG と同じ規約)。
 * @param data CRC を計算する対象バイト列。
 * @param size data のバイト数。
 * @return 計算した CRC32。
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
 * 現在の file pointer 位置 (= これから書く位置 = "tell") を返す。
 *
 * @param h 対象のファイルハンドル。
 * @return 現在の file pointer のオフセット (取得失敗時は 0)。
 */
u64 Tell(HANDLE h) noexcept {
    const LARGE_INTEGER zero{};
    LARGE_INTEGER cur{};
    if (!::SetFilePointerEx(h, zero, &cur, FILE_CURRENT)) {
        return 0;
    }
    return static_cast<u64>(cur.QuadPart);
}

/**
 * file pointer を指定オフセットに移動する (SetFilePointerEx, FILE_BEGIN)。
 *
 * @param h 対象のファイルハンドル。
 * @param offset 移動先のファイルオフセット。
 * @param err 失敗時に GetLastError の値を格納する (成功時 0)。
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
 * src の size バイトをすべて書き込む (>4GiB を考慮した WriteFile ラッパ)。
 *
 * @details
 * WriteFile は DWORD (32bit) 単位でしか書けないため、4GiB 超はチャンク分割して
 * ループする。size == 0 は何もせず成功を返す。
 * @param h 書き込み対象のファイルハンドル。
 * @param src 書き込むバイト列。
 * @param size src のバイト数。
 * @param err 失敗時に GetLastError の値を格納する (成功時 0)。
 * @return 成功なら true、失敗なら false。
 */
bool WriteAll(HANDLE h, const void* src, u64 size, DWORD& err) noexcept {
    err = 0;
    if (size == 0) return true;
    const u8* p = static_cast<const u8*>(src);
    u64 remaining = size;
    while (remaining > 0) {
        const DWORD chunk = (remaining > 0x7FFFFFFFu)
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
 * u32 を 4 バイト LE で書き出す (strict-aliasing 安全)。
 *
 * @param dst 書き込み先 (4 バイト)。
 * @param v 書き込む u32。
 */
void WriteU32LE(u8* dst, u32 v) noexcept {
    MemCopy(dst, &v, sizeof(u32));
}

/**
 * u64 を 8 バイト LE で書き出す (strict-aliasing 安全)。
 *
 * @param dst 書き込み先 (8 バイト)。
 * @param v 書き込む u64。
 */
void WriteU64LE(u8* dst, u64 v) noexcept {
    MemCopy(dst, &v, sizeof(u64));
}

/**
 * 自前 wcslen で NUL 終端 wchar_t 列の長さを返す (依存ヘッダを増やさない)。
 *
 * @param s 長さを測る文字列 (nullptr なら 0)。
 * @return NUL を含まない wchar_t 数。
 */
u32 LenW(const wchar_t* s) noexcept {
    u32 n = 0;
    if (s == nullptr) return 0;
    while (s[n]) ++n;
    return n;
}

} // namespace

/** 破棄時に Close を呼んで後始末する。 */
FAcpakWriter::~FAcpakWriter() noexcept {
    Close();
}

/** ハンドルを閉じ、ResetState で内部状態をクリアする (多重 Close は no-op)。 */
void FAcpakWriter::Close() noexcept {
    if (m_FileHandle != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(m_FileHandle));
        m_FileHandle = nullptr;
    }
    ResetState();
}

/** flags / Finalize フラグ / pending list / 鍵情報をクリアする。 */
void FAcpakWriter::ResetState() noexcept {
    m_Flags     = 0;
    m_Finalized = false;
    m_Pending.Clear();

    // 鍵 defensive zero
    MemSet(m_Key.bytes, 0, sizeof(m_Key.bytes));
    m_HasKey = false;
}

/** 暗号化鍵を内部にコピーし、鍵設定済みフラグを立てる。 */
void FAcpakWriter::SetKey(const FAcpakKey& key) noexcept {
    MemCopy(m_Key.bytes, key.bytes, sizeof(m_Key.bytes));
    m_HasKey = true;
}

/** 出力ファイルを開き、ヘッダのプレースホルダ (36B) を書き出す。 */
TResult<void> FAcpakWriter::Open(const wchar_t* output_path, EAcpakFlags flags) noexcept {
    if (m_FileHandle != nullptr) {
        return ACS_ERR(IO, kAcpakSubAlreadyOpen,
                       "FAcpakWriter::Open: writer already open");
    }
    if (output_path == nullptr) {
        return ACS_ERR(IO, kAcpakSubIOFailure,
                       "FAcpakWriter::Open: output_path is null");
    }

    // encrypted / compressed は対応済。未知 flag bit のみ拒否。
    const u32 known_flags = static_cast<u32>(AcpakFlagEncrypted) |
                            static_cast<u32>(AcpakFlagCompressed);
    if ((static_cast<u32>(flags) & ~known_flags) != 0) {
        return ACS_ERR(Asset, kAcpakSubBadFlags,
                       "FAcpakWriter::Open: unknown flag bits");
    }

    const HANDLE h = ::CreateFileW(output_path, GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return ACS_ERR_OS(IO, kAcpakSubIOFailure,
                          "FAcpakWriter::Open: CreateFileW failed",
                          ::GetLastError());
    }

    m_FileHandle = h;
    m_Flags       = static_cast<u32>(flags);
    m_Finalized   = false;
    m_Pending.Clear();

    // ---- ヘッダプレースホルダを書く (Finalize で上書きする) -----------------
    u8 header[kAcpakHeaderDiskSize] = {};
    MemCopy(header, kAcpakMagic, 8);
    WriteU32LE(header + 8,  kAcpakVersion);
    WriteU32LE(header + 12, m_Flags);
    WriteU32LE(header + 16, 0);            // file_count placeholder
    WriteU32LE(header + 20, 0);            // padding = 0
    WriteU64LE(header + 24, 0);            // file_table_offset placeholder
    WriteU32LE(header + 32, 0);            // reserved = 0

    DWORD err = 0;
    if (!WriteAll(h, header, kAcpakHeaderDiskSize, err)) {
        ::CloseHandle(h);
        m_FileHandle = nullptr;
        ResetState();
        return ACS_ERR_OS(IO, kAcpakSubIOFailure,
                          "FAcpakWriter::Open: WriteFile (header) failed", err);
    }

    return Ok();
}

/** 1 ファイルを pending list に積む (実書き込みは Finalize)。 */
TResult<void> FAcpakWriter::AddFile(const wchar_t* virtual_path,
                                  const void*    data,
                                  u64            size) noexcept {
    if (m_FileHandle == nullptr) {
        return ACS_ERR(IO, kAcpakSubNotOpen,
                       "FAcpakWriter::AddFile: writer not open");
    }
    if (m_Finalized) {
        return ACS_ERR(IO, kAcpakSubNotOpen,
                       "FAcpakWriter::AddFile: writer already finalized");
    }
    if (virtual_path == nullptr) {
        return ACS_ERR(IO, kAcpakSubIOFailure,
                       "FAcpakWriter::AddFile: virtual_path is null");
    }
    if (data == nullptr && size > 0) {
        return ACS_ERR(IO, kAcpakSubIOFailure,
                       "FAcpakWriter::AddFile: data is null but size > 0");
    }

    PendingEntry e{};
    e.path = virtual_path;
    e.data = data;
    e.size = size;
    m_Pending.PushBack(e);
    return Ok();
}

/** pending entry 群をディスクに書き出し、最後にヘッダを更新して pak を確定する。 */
TResult<void> FAcpakWriter::Finalize() noexcept {
    // 各 entry に対し compress-then-encrypt パイプラインを通す:
    //   1. (元データ ptr, size) → CRC32 計算 (元バイト基準)
    //   2. compressed 立ち → LZ4 圧縮
    //   3. encrypted 立ち → CSPRNG nonce 生成 → AES-256-GCM 暗号化 (tag 出力)
    //   4. ディスクに stored bytes を書く
    //   5. file table に (offset, size_unc, size_st, crc32, nonce?, tag?) を書く
    //
    // Reader はファイル単位の per-entry "compressed?" 判定を持たず header flags
    // のみ参照するため、compressed flag 立ち時は entry が短くても必ず LZ4 を通し、
    // たとえ size_stored > size_uncompressed でも保存する。圧縮 + 暗号化が両方
    // 立つ場合も、LZ4 で生格納に倒れた entry を含め暗号化は通常通り行う。
    if (m_FileHandle == nullptr) {
        return ACS_ERR(IO, kAcpakSubNotOpen,
                       "FAcpakWriter::Finalize: writer not open");
    }
    if (m_Finalized) {
        return ACS_ERR(IO, kAcpakSubNotOpen,
                       "FAcpakWriter::Finalize: already finalized");
    }

    const HANDLE h = static_cast<HANDLE>(m_FileHandle);
    DWORD  err = 0;

    // ---- 既にヘッダ 36B は Open で書いてあり、file pointer は 36 にある -----
    // 念のため明示的に seek (古いヘッダ書き込みが flush されていない場合に
    // file pointer が想定外位置にいるリスクを下げる)。
    if (!SeekTo(h, kAcpakHeaderDiskSize, err)) {
        return ACS_ERR_OS(IO, kAcpakSubIOFailure,
                          "FAcpakWriter::Finalize: SetFilePointerEx failed", err);
    }

    const bool is_encrypted  = (m_Flags & static_cast<u32>(AcpakFlagEncrypted))  != 0u;
    const bool is_compressed = (m_Flags & static_cast<u32>(AcpakFlagCompressed)) != 0u;

    // 暗号化が立っているのに鍵未設定なら早期に弾く (失敗のソースを明確化)。
    if (is_encrypted && !m_HasKey) {
        return ACS_ERR(Asset, kAcpakSubCryptoKey,
                       "FAcpakWriter::Finalize: encrypted flag set but no key");
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
    written.Reserve(m_Pending.Size());

    // 中間バッファは entry 間で再利用する (TArray<u8> を 2 つ用意)。
    TArray<u8> stage_compress;   // LZ4 圧縮出力先
    TArray<u8> stage_encrypt;    // AES-GCM 暗号化出力先 (in-place 可だが安全のため別)

    // ---- ファイルデータ書き出し -------------------------------------------
    for (usize i = 0; i < m_Pending.Size(); ++i) {
        const PendingEntry& p = m_Pending[i];

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
                               "FAcpakWriter::Finalize: input > 4GiB (LZ4 limit)");
            }
            const u32 src_size = static_cast<u32>(p.size);
            const u32 dst_cap  = FAcpakLz4::MaxCompressedSize(src_size);
            stage_compress.Resize(dst_cap);
            const auto cr = FAcpakLz4::Compress(static_cast<const u8*>(p.data),
                                         src_size,
                                         stage_compress.Data(),
                                         dst_cap);
            if (cr.IsErr()) return cr.Error();
            stage_ptr  = stage_compress.Data();
            stage_size = cr.Value();
        }

        // ---- 暗号化 (compress-then-encrypt の 2 段目) -------------------
        if (is_encrypted) {
            // CSPRNG nonce 生成。RNG 失敗時はゼロ nonce での AES-GCM 暗号化
            // (= nonce 再利用 → 認証鍵漏洩) を絶対に避けるため、エラーを伝播し
            // 暗号化を中止する。
            const auto nr = FAcpakCrypto::GenerateRandomNonce(w.cipher_nonce);
            if (nr.IsErr()) return nr.Error();

            // 出力バッファ (= 同 size の別領域、in-place も可だが分離で安全)
            stage_encrypt.Resize(static_cast<usize>(stage_size));
            const auto er = FAcpakCrypto::Encrypt(m_Key,
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
                                  "FAcpakWriter::Finalize: WriteFile (data) failed", err);
            }
        }

        written.PushBack(w);
    }

    // ---- file table 書き出し ---------------------------------------------
    const u64 file_table_offset = Tell(h);

    for (usize i = 0; i < m_Pending.Size(); ++i) {
        const PendingEntry& p    = m_Pending[i];
        const WrittenEntry&  w   = written[i];
        const u32            len = LenW(p.path);

        // path_len (4) + path (len*2) + offset (8) + size_unc (8) + size_st (8)
        // + crc32 (4) を 1 つの一時バッファに詰めて書く。
        // 暗号化のとき末尾に cipher_nonce(12) + cipher_tag(16) を続ける。
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
        u8* const tail = buf + 4 + path_bytes;
        WriteU64LE(tail + 0,  w.offset);
        WriteU64LE(tail + 8,  w.size_unc);     // size_uncompressed
        WriteU64LE(tail + 16, w.size_stored);  // size_stored (圧縮+暗号化後)
        WriteU32LE(tail + 24, w.crc32);
        if (is_encrypted) {
            MemCopy(tail + 28,      w.cipher_nonce, 12);
            MemCopy(tail + 28 + 12, w.cipher_tag,   16);
        }

        if (!WriteAll(h, buf, entry_bytes, err)) {
            return ACS_ERR_OS(IO, kAcpakSubIOFailure,
                              "FAcpakWriter::Finalize: WriteFile (entry) failed", err);
        }
    }

    // ---- ヘッダ書き戻し (file_count + file_table_offset を確定) ------------
    if (!SeekTo(h, 0, err)) {
        return ACS_ERR_OS(IO, kAcpakSubIOFailure,
                          "FAcpakWriter::Finalize: SetFilePointerEx (rewind) failed", err);
    }

    u8 header[kAcpakHeaderDiskSize] = {};
    MemCopy(header, kAcpakMagic, 8);
    WriteU32LE(header + 8,  kAcpakVersion);
    WriteU32LE(header + 12, m_Flags);
    WriteU32LE(header + 16, static_cast<u32>(m_Pending.Size()));
    WriteU32LE(header + 20, 0);            // padding = 0
    WriteU64LE(header + 24, file_table_offset);
    WriteU32LE(header + 32, 0);            // reserved = 0

    if (!WriteAll(h, header, kAcpakHeaderDiskSize, err)) {
        return ACS_ERR_OS(IO, kAcpakSubIOFailure,
                          "FAcpakWriter::Finalize: WriteFile (header rewrite) failed", err);
    }

    // ディスクに反映 (クラッシュ耐性は完璧ではないが、最低限 flush 要求)
    ::FlushFileBuffers(h);

    m_Finalized = true;
    return Ok();
}

} // namespace acs::assetpack
