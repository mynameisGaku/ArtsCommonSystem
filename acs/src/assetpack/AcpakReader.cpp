// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS FAssetPack — FAcpakReader 実装 (Win32 I/O + CRC32 検証)
// -----------------------------------------------------------------------------
// 実装範囲:
//   ・magic / version / flags 検証
//   ・file table をエントリ毎に逐次読み出して `TArray<FAcpakFileEntry>` を構築
//   ・各 path は `m_StringPool` に NUL 終端付きで連結保存し、entry.path はその
//     先頭へのポインタを保持する
//   ・CRC32 (poly 0xEDB88320, init 0xFFFFFFFF, xorout 0xFFFFFFFF) 検証
//   ・flags = 0 のみ正常パス、Encrypted / Compressed bit は NotImplemented
//
// 内部設計のポイント:
//   ・file table を「2 パス」で読む。1 パス目で各 entry の path 長 + 数値メタ
//     データだけ拾い、m_StringPool を 1 度だけ最終サイズで Reserve する。
//     2 パス目で path 文字列を pool にコピーし、entry.path を pool の最終
//     アドレスにバインドする。
//   ・これにより「pool が PushBack 中に grow して既存 entry.path が dangling
//     になる」問題を構造的に防ぐ (= grow が起きない条件で操作する)。
// =============================================================================
#include "assetpack/AcpakReader.h"

#include "foundation/Platform.h"   // <windows.h> を 1 箇所に隠す
#include "foundation/Log.h"
#include "memory/Memory.h"         // MemCopy / MemSet

#include "assetpack/AcpakCrypto.h" // AES-256-GCM 復号
#include "assetpack/AcpakLz4.h"    // LZ4 解凍

namespace acs::assetpack {

namespace {

/**
 * CRC32 lookup table (poly 0xEDB88320) を遅延構築して返す。
 *
 * @details 256-entry table を最初の呼び出し時に組み立てる (Meyers singleton)。
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
 * 指定オフセットから size バイトを dst に読み出す (SetFilePointerEx + ReadFile)。
 *
 * @details
 * ReadFile は DWORD (32bit) 単位でしか読めないため、4GiB 超はチャンク分割して
 * ループする。size == 0 は何もせず成功を返す。
 * @param h 読み出し対象のファイルハンドル。
 * @param offset 読み出し開始のファイルオフセット。
 * @param dst 読み出し先バッファ。
 * @param size 読み出すバイト数。
 * @param err 失敗時に GetLastError の値を格納する (成功時 0)。
 * @return 成功なら true、失敗なら false。
 */
bool ReadAt(HANDLE h, u64 offset, void* dst, u64 size, DWORD& err) noexcept {
    err = 0;
    if (size == 0) return true;
    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(offset);
    if (!::SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) {
        err = ::GetLastError();
        return false;
    }
    // ReadFile は DWORD (32bit) 単位でしか読めないため、>4GiB をループする。
    u8* dst_b = static_cast<u8*>(dst);
    u64 remaining = size;
    while (remaining > 0) {
        const DWORD chunk = (remaining > 0x7FFFFFFFu)
                          ? 0x7FFFFFFFu
                          : static_cast<DWORD>(remaining);
        DWORD got = 0;
        if (!::ReadFile(h, dst_b, chunk, &got, nullptr) || got == 0) {
            err = ::GetLastError();
            if (err == 0) err = ERROR_HANDLE_EOF;
            return false;
        }
        dst_b += got;
        remaining -= got;
    }
    return true;
}

/**
 * 4 バイトを u32 (LE) として strict-aliasing 安全に読む。
 *
 * @details
 * reinterpret_cast<u32*> は strict-aliasing 違反になり得るため memcpy で読む。
 * ACS 対応プラットフォームは全て LE なので生バイト並びがそのまま LE 整数になる。
 * @param src 読み出し元 (4 バイト)。
 * @return 読み出した u32。
 */
u32 ReadU32LE(const u8* src) noexcept {
    u32 v = 0;
    MemCopy(&v, src, sizeof(u32));
    return v;
}

/**
 * 8 バイトを u64 (LE) として strict-aliasing 安全に読む。
 *
 * @param src 読み出し元 (8 バイト)。
 * @return 読み出した u64。
 */
u64 ReadU64LE(const u8* src) noexcept {
    u64 v = 0;
    MemCopy(&v, src, sizeof(u64));
    return v;
}

/**
 * 自前 wcscmp で 2 つの NUL 終端 wchar_t 列を比較する (依存ヘッダを増やさない)。
 *
 * @param a 比較対象の文字列 (NUL 終端)。
 * @param b 比較対象の文字列 (NUL 終端)。
 * @return 一致なら 0、a<b で負、a>b で正。
 */
int CompareW(const wchar_t* a, const wchar_t* b) noexcept {
    while (*a && (*a == *b)) { ++a; ++b; }
    return static_cast<int>(*a) - static_cast<int>(*b);
}

} // namespace

/** 破棄時に Close を呼んで後始末する。 */
FAcpakReader::~FAcpakReader() noexcept {
    Close();
}

/** ハンドルを閉じ、文字列 pool + entry 配列 + 鍵情報を解放/0 クリアする。 */
void FAcpakReader::Close() noexcept {
    if (m_FileHandle != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(m_FileHandle));
        m_FileHandle = nullptr;
    }
    m_FileSize    = 0;
    m_Flags        = 0;
    m_TableOffset = 0;
    m_Entries.Clear();
    m_StringPool.Clear();

    // 鍵情報の defensive zero (再 Open のときに古い鍵が漏れないよう)。
    MemSet(m_Key.bytes, 0, sizeof(m_Key.bytes));
    m_HasKey = false;
}

/** 暗号化 pak の復号鍵を内部にコピーし、鍵設定済みフラグを立てる。 */
void FAcpakReader::SetKey(const FAcpakKey& key) noexcept {
    MemCopy(m_Key.bytes, key.bytes, sizeof(m_Key.bytes));
    m_HasKey = true;
}

/** `.acpak` を開き、header と file table を読み出す (失敗時は Close 相当に戻す)。 */
TResult<void> FAcpakReader::Open(const wchar_t* file_path) noexcept {
    // 二度目以降の Open は前回を黙って閉じる (gameframework の Mount 流儀)。
    if (IsOpen()) Close();

    if (file_path == nullptr) {
        return ACS_ERR(IO, kAcpakSubIOFailure,
                       "FAcpakReader::Open: file_path is null");
    }

    const HANDLE h = ::CreateFileW(file_path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return ACS_ERR_OS(IO, kAcpakSubIOFailure,
                          "FAcpakReader::Open: CreateFileW failed",
                          ::GetLastError());
    }

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(h, &size)) {
        const DWORD err = ::GetLastError();
        ::CloseHandle(h);
        return ACS_ERR_OS(IO, kAcpakSubIOFailure,
                          "FAcpakReader::Open: GetFileSizeEx failed", err);
    }

    m_FileHandle = h;
    m_FileSize   = static_cast<u64>(size.QuadPart);

    const auto r = LoadHeaderAndTable();
    if (r.IsErr()) {
        Close();
        return r.Error();
    }
    return Ok();
}

/** header (36B) と file table の全 entry を 2 パスで読み出す。 */
TResult<void> FAcpakReader::LoadHeaderAndTable() noexcept {
    // 2 パス構成:
    //   Pass 1: 各 entry の (path_len, offset, size_uncompressed, size_stored,
    //           crc32, path_pool_offset) を読み出す。path 文字列は paths_temp に
    //           連結保存し、m_StringPool の最終サイズを確定する。
    //   Pass 2: m_StringPool に paths_temp をコピーし、entries_raw を m_Entries
    //           に変換 (entry.path = pool_base + path_pool_offset)。
    // このやり方なら m_StringPool は Pass 2 で 1 度だけ Resize するので、
    // PushBack 中の re-grow による dangling pointer が起きない。
    const HANDLE h = static_cast<HANDLE>(m_FileHandle);

    if (m_FileSize < kAcpakHeaderDiskSize) {
        return ACS_ERR(IO, kAcpakSubBadSize,
                       "FAcpakReader::Open: file smaller than header");
    }

    // ---- ヘッダを 36 バイト読む ------------------------------------------
    u8 header_buf[kAcpakHeaderDiskSize] = {};
    DWORD err = 0;
    if (!ReadAt(h, 0, header_buf, kAcpakHeaderDiskSize, err)) {
        return ACS_ERR_OS(IO, kAcpakSubIOFailure,
                          "FAcpakReader::Open: ReadFile (header) failed", err);
    }

    // magic 比較 (バイト列、strict-aliasing 安全)
    for (u32 i = 0; i < 8; ++i) {
        if (header_buf[i] != kAcpakMagic[i]) {
            return ACS_ERR(Asset, kAcpakSubBadMagic,
                           "FAcpakReader::Open: magic mismatch (not an .acpak)");
        }
    }

    const u32 version           = ReadU32LE(header_buf + 8);
    const u32 flags             = ReadU32LE(header_buf + 12);
    const u32 file_count        = ReadU32LE(header_buf + 16);
    // padding (offset 20) は 0 で予約
    const u64 file_table_offset = ReadU64LE(header_buf + 24);
    // reserved (offset 32) は 0 で予約

    if (version != kAcpakVersion) {
        ACS_LOG_WARN("FAcpakReader::Open: version mismatch (file=%u, expected=%u)",
                     version, kAcpakVersion);
        return ACS_ERR(Asset, kAcpakSubBadVersion,
                       "FAcpakReader::Open: unsupported .acpak version");
    }

    // Encrypted / Compressed bit は実装済。未知 bit のみ拒否する。
    const u32 known_flags = static_cast<u32>(AcpakFlagEncrypted) |
                            static_cast<u32>(AcpakFlagCompressed);
    if ((flags & ~known_flags) != 0) {
        return ACS_ERR(Asset, kAcpakSubBadFlags,
                       "FAcpakReader::Open: unknown flag bits in header");
    }

    if (file_count > 0 && file_table_offset >= m_FileSize) {
        return ACS_ERR(IO, kAcpakSubBadSize,
                       "FAcpakReader::Open: file_table_offset out of range");
    }

    m_Flags        = flags;
    m_TableOffset = file_table_offset;

    // file_count == 0 ならテーブル読み出しは不要 (空 pak)。
    if (file_count == 0) {
        return Ok();
    }

    // ---- Pass 1: 各 entry のメタデータと path 長を読み、path 文字列を
    //              `paths_temp` (TArray<wchar_t>) に連結する -----------------
    // 内部表現: entry のうち path だけ後で resolve するので、index 配列を別途
    // 保持する。encrypted pak のときは追加で cipher_nonce/tag も読む。
    struct RawEntry {
        u32 path_len;          // wchar_t 数
        u32 path_pool_offset;  // m_StringPool 内の先頭オフセット (wchar_t 単位)
        u64 offset;
        u64 size_uncompressed;
        u64 size_stored;
        u32 crc32;
        u8  cipher_nonce[12];  // encrypted pak でのみ有効
        u8  cipher_tag[16];    // encrypted pak でのみ有効
    };

    const bool is_encrypted = (flags & static_cast<u32>(AcpakFlagEncrypted)) != 0u;
    const bool is_compressed = (flags & static_cast<u32>(AcpakFlagCompressed)) != 0u;
    (void)is_compressed;  // ReadFile 側でのみ参照する (記号として残す)

    TArray<RawEntry>   raws;
    TArray<wchar_t>    paths_temp;
    raws.Reserve(file_count);
    // 平均 64 wchar_t + NUL 想定で 65 * file_count を予約 (= 適度な見積もり)
    paths_temp.Reserve(static_cast<usize>(file_count) * 65u);

    u64 cursor = file_table_offset;

    for (u32 i = 0; i < file_count; ++i) {
        // path_len (u32)
        if (cursor + 4u > m_FileSize) {
            return ACS_ERR(IO, kAcpakSubBadSize,
                           "FAcpakReader::Open: file table truncated (path_len)");
        }
        u8 pl_buf[4];
        if (!ReadAt(h, cursor, pl_buf, 4, err)) {
            return ACS_ERR_OS(IO, kAcpakSubIOFailure,
                              "FAcpakReader::Open: ReadFile (path_len) failed", err);
        }
        const u32 path_len = ReadU32LE(pl_buf);
        cursor += 4;

        // path 長を許容範囲に制限する (悪意のあるアーカイブで OOM を防ぐ)。
        // Win32 long path 対応のため 4096 wchar_t = 8KB まで許容する。
        if (path_len > 4096u) {
            return ACS_ERR(IO, kAcpakSubBadSize,
                           "FAcpakReader::Open: path_len too large (>4096)");
        }

        const u64 path_bytes = static_cast<u64>(path_len) * sizeof(wchar_t);
        // entry の残りサイズ: path + offset(8) + size_uncompressed(8) +
        // size_stored(8) + crc32(4) = path_bytes + 28
        // encrypted のときはさらに cipher_nonce(12) + cipher_tag(16) = 28
        const u64 tail_bytes = is_encrypted
                                   ? (28u + kAcpakCipherFieldsDiskSize)
                                   : 28u;
        if (cursor + path_bytes + tail_bytes > m_FileSize) {
            return ACS_ERR(IO, kAcpakSubBadSize,
                           "FAcpakReader::Open: file table truncated (entry)");
        }

        // path を paths_temp に書き込む (NUL 込みで連結)。
        // path_len の現在末尾オフセットを記録する。
        const u32 pool_off = static_cast<u32>(paths_temp.Size());

        // PushBack ループ — TArray は exponential grow なので O(amortized 1)/wchar_t
        if (path_len > 0) {
            // 効率のため Resize して直接 ReadFile する
            const usize prev_size = paths_temp.Size();
            paths_temp.Resize(prev_size + static_cast<usize>(path_len));
            if (!ReadAt(h, cursor, paths_temp.Data() + prev_size, path_bytes, err)) {
                return ACS_ERR_OS(IO, kAcpakSubIOFailure,
                                  "FAcpakReader::Open: ReadFile (path) failed", err);
            }
        }
        paths_temp.PushBack(L'\0');
        cursor += path_bytes;

        // offset / size_uncompressed / size_stored / crc32 (28 バイト)
        // + 暗号化時は nonce(12) + tag(16) を続けて読む。
        u8 tail[28 + 12 + 16];
        const u32 tail_read = static_cast<u32>(tail_bytes);
        if (!ReadAt(h, cursor, tail, tail_read, err)) {
            return ACS_ERR_OS(IO, kAcpakSubIOFailure,
                              "FAcpakReader::Open: ReadFile (entry tail) failed", err);
        }
        cursor += tail_read;

        RawEntry r{};
        r.path_len          = path_len;
        r.path_pool_offset  = pool_off;
        r.offset            = ReadU64LE(tail + 0);
        r.size_uncompressed = ReadU64LE(tail + 8);
        r.size_stored       = ReadU64LE(tail + 16);
        r.crc32             = ReadU32LE(tail + 24);
        if (is_encrypted) {
            MemCopy(r.cipher_nonce, tail + 28, 12);
            MemCopy(r.cipher_tag,   tail + 40, 16);
        }
        // else: ゼロ初期化のまま (RawEntry r{} で 0 クリア済)

        // sanity: entry が指す data 領域が file_size に収まっているか。
        // offset / size_stored は untrusted な u64 のため、`offset + size_stored`
        // を直接比較すると加算が u64 で wrap し、巨大値の組み合わせ
        // (例: offset=0xFFFF...FF00, size_stored=0x200) が境界チェックをすり抜け、
        // ReadFile で任意オフセットへ seek して OOB read する。減算側で評価して
        // overflow を回避する: size_stored > file_size || offset > file_size - size_stored。
        if (r.size_stored > m_FileSize ||
            r.offset > m_FileSize - r.size_stored) {
            return ACS_ERR(IO, kAcpakSubBadSize,
                           "FAcpakReader::Open: entry data range out of file");
        }
        // stored != uncompressed は AcpakFlagCompressed のときだけ許容。
        // Compressed 立ってないのに stored != uncompressed なら破損アーカイブ。
        if (!is_compressed && r.size_stored != r.size_uncompressed) {
            return ACS_ERR(Asset, kAcpakSubBadSize,
                           "FAcpakReader::Open: stored != uncompressed but flag clear");
        }

        raws.PushBack(r);
    }

    // ---- Pass 2: m_StringPool を最終サイズで Resize し、paths_temp を
    //              ムーブ相当でコピー。entry を組み立てて m_Entries に格納 ----
    m_StringPool.Resize(paths_temp.Size());
    if (!paths_temp.IsEmpty()) {
        MemCopy(m_StringPool.Data(),
                paths_temp.Data(),
                paths_temp.Size() * sizeof(wchar_t));
    }
    // ここから m_StringPool は再 grow させない (entry.path は pool ベースに
    // 依存するため)。

    m_Entries.Reserve(raws.Size());
    const wchar_t* pool_base = m_StringPool.Data();
    for (usize i = 0; i < raws.Size(); ++i) {
        const RawEntry& r = raws[i];
        FAcpakFileEntry e{};
        e.path              = pool_base + r.path_pool_offset;
        e.offset            = r.offset;
        e.size_uncompressed = r.size_uncompressed;
        e.size_stored       = r.size_stored;
        e.crc32             = r.crc32;
        // 暗号化フィールドは encrypted pak のみ意味あり、それ以外は 0。
        MemCopy(e.cipher_nonce, r.cipher_nonce, 12);
        MemCopy(e.cipher_tag,   r.cipher_tag,   16);
        m_Entries.PushBack(e);
    }

    return Ok();
}

/**
 * src の wchar_t 列を m_StringPool に NUL 付きで追加し、その先頭を返す。
 *
 * @details
 * 現在の LoadHeaderAndTable は 2 パス構造でこの helper を使わない (= 未使用)。
 * header に宣言があるため空実装を残している。
 * @param src 追加する文字列。
 * @param len src の wchar_t 数。
 * @return pool 内に確保された文字列の先頭ポインタ。
 */
const wchar_t* FAcpakReader::InternPath(const wchar_t* src, u32 len) noexcept {
    const usize prev = m_StringPool.Size();
    for (u32 i = 0; i < len; ++i) {
        m_StringPool.PushBack(src[i]);
    }
    m_StringPool.PushBack(L'\0');
    return m_StringPool.Data() + prev;
}

/** index 番目の entry を返す (範囲外 / 未 Open なら nullptr)。 */
const FAcpakFileEntry* FAcpakReader::GetEntry(u32 index) const noexcept {
    if (!IsOpen()) return nullptr;
    if (static_cast<usize>(index) >= m_Entries.Size()) return nullptr;
    return &m_Entries[static_cast<usize>(index)];
}

/** 仮想パスから entry を線形探索で探す (無い / 未 Open なら nullptr)。 */
const FAcpakFileEntry* FAcpakReader::FindEntry(const wchar_t* path) const noexcept {
    if (!IsOpen() || path == nullptr) return nullptr;
    for (usize i = 0; i < m_Entries.Size(); ++i) {
        if (CompareW(m_Entries[i].path, path) == 0) {
            return &m_Entries[i];
        }
    }
    return nullptr;
}

/** 仮想パスの復号 + 解凍後のバイト数を返す (未存在は kAcpakSubNotFound)。 */
TResult<u64> FAcpakReader::GetUncompressedSize(const wchar_t* path) const noexcept {
    if (!IsOpen()) {
        return ACS_ERR(IO, kAcpakSubNotOpen,
                       "FAcpakReader::GetUncompressedSize: pak not open");
    }
    const FAcpakFileEntry* e = FindEntry(path);
    if (e == nullptr) {
        return ACS_ERR(IO, kAcpakSubNotFound,
                       "FAcpakReader::GetUncompressedSize: path not found");
    }
    return TResult<u64>(OkInit, e->size_uncompressed);
}

/** 仮想パスのファイルを out_buffer に読み出す (復号 → 解凍 → CRC32 検証)。 */
TResult<u64> FAcpakReader::ReadFile(const wchar_t* path,
                                  void*          out_buffer,
                                  u64            buffer_size) noexcept {
    if (!IsOpen()) {
        return ACS_ERR(IO, kAcpakSubNotOpen,
                       "FAcpakReader::ReadFile: pak not open");
    }
    if (out_buffer == nullptr) {
        return ACS_ERR(IO, kAcpakSubIOFailure,
                       "FAcpakReader::ReadFile: out_buffer is null");
    }
    const FAcpakFileEntry* e = FindEntry(path);
    if (e == nullptr) {
        return ACS_ERR(IO, kAcpakSubNotFound,
                       "FAcpakReader::ReadFile: path not found");
    }
    if (buffer_size < e->size_uncompressed) {
        return ACS_ERR(IO, kAcpakSubBufferTooSmall,
                       "FAcpakReader::ReadFile: buffer too small");
    }

    const bool is_encrypted  = (m_Flags & static_cast<u32>(AcpakFlagEncrypted))  != 0u;
    const bool is_compressed = (m_Flags & static_cast<u32>(AcpakFlagCompressed)) != 0u;

    const HANDLE h = static_cast<HANDLE>(m_FileHandle);
    DWORD  err = 0;

    // 暗号化 pak で鍵未設定なら早期に弾く (Decrypt がいずれにせよ失敗するが、
    // 専用 subcode で返した方がトラブルシュートに親切)。
    if (is_encrypted && !m_HasKey) {
        return ACS_ERR(Asset, kAcpakSubCryptoKey,
                       "FAcpakReader::ReadFile: encrypted pak but no key set");
    }

    // ---- pipeline 設計 (decrypt-then-decompress) -------------------------
    //
    //   ディスク → [size_stored bytes (ciphertext or stored)]
    //     ↓ (encrypted ? AES-GCM decrypt : noop)
    //     [size_stored bytes (compressed or raw)]
    //     ↓ (compressed ? LZ4 decompress : noop)
    //     [size_uncompressed bytes (final plaintext)]
    //     ↓ CRC32 検証
    //     out_buffer
    //
    // 中間段が必要かは flags の組み合わせ次第:
    //   ・flags == 0           : ディスク直 → out_buffer (中間バッファ不要)
    //   ・compressed only      : ディスク → tmp(size_stored) → LZ4 → out_buffer
    //   ・encrypted only       : ディスク → out_buffer に load → in-place decrypt
    //                            (size_stored == size_uncompressed なので buffer OK)
    //   ・encrypted+compressed : ディスク → tmp(size_stored) → in-place decrypt
    //                            → LZ4 → out_buffer
    //
    // tmp バッファは size_stored バイト確保する。

    TArray<u8> tmp;   // 圧縮中間バッファ (圧縮 flag のときのみ使う)
    void* stored_dst = nullptr;   // ディスク から最初に読み込む先

    if (is_compressed) {
        // 中間バッファ確保
        if (e->size_stored > 0xFFFFFFFFu) {
            return ACS_ERR(Asset, kAcpakSubBadSize,
                           "FAcpakReader::ReadFile: stored size > 4GiB (LZ4 limit)");
        }
        tmp.Resize(static_cast<usize>(e->size_stored));
        stored_dst = tmp.IsEmpty() ? nullptr : tmp.Data();
    } else {
        // 非圧縮なら out_buffer 直接 (size_stored == size_uncompressed)
        stored_dst = out_buffer;
    }

    // ---- ディスク read -----------------------------------------------------
    if (e->size_stored > 0) {
        if (!ReadAt(h, e->offset, stored_dst, e->size_stored, err)) {
            return ACS_ERR_OS(IO, kAcpakSubIOFailure,
                              "FAcpakReader::ReadFile: ReadFile (data) failed", err);
        }
    }

    // ---- AES-GCM 復号 (in-place) -------------------------------------------
    // size_stored == 0 (= 元から空ファイル) でも GMAC 動作で tag 検証を行う。
    // ここを skip すると空ファイルの cipher_tag が攻撃者に書き換え自由になる。
    if (is_encrypted) {
        u8* const p = static_cast<u8*>(stored_dst);
        const auto dr = FAcpakCrypto::Decrypt(m_Key,
                                       e->cipher_nonce,
                                       e->cipher_tag,
                                       p, e->size_stored,
                                       p);
        if (dr.IsErr()) {
            // tag mismatch は改竄、その他は CNG エラー — 呼び出し側にそのまま
            // 伝搬する (subcode が原因を示す)。
            return dr.Error();
        }
    }

    // ---- LZ4 解凍 ---------------------------------------------------------
    if (is_compressed && e->size_uncompressed > 0) {
        if (e->size_uncompressed > 0xFFFFFFFFu) {
            return ACS_ERR(Asset, kAcpakSubBadSize,
                           "FAcpakReader::ReadFile: uncompressed size > 4GiB (LZ4 limit)");
        }
        const auto dr = FAcpakLz4::Decompress(tmp.Data(),
                                       static_cast<u32>(e->size_stored),
                                       static_cast<u8*>(out_buffer),
                                       static_cast<u32>(buffer_size));
        if (dr.IsErr()) {
            return dr.Error();
        }
        if (static_cast<u64>(dr.Value()) != e->size_uncompressed) {
            // 解凍結果サイズが TOC と一致しない = 破損 or バグ
            return ACS_ERR(Asset, kAcpakSubBadSize,
                           "FAcpakReader::ReadFile: LZ4 decompressed size mismatch");
        }
    }

    // ---- CRC32 検証 (元の生バイトに対して) --------------------------------
    const u32 actual = ComputeCrc32(out_buffer, e->size_uncompressed);
    if (actual != e->crc32) {
        ACS_LOG_WARN("FAcpakReader::ReadFile: CRC mismatch (expected=0x%08x, actual=0x%08x)",
                     e->crc32, actual);
        return ACS_ERR(Asset, kAcpakSubBadCrc,
                       "FAcpakReader::ReadFile: CRC32 mismatch");
    }

    return TResult<u64>(OkInit, e->size_uncompressed);
}

} // namespace acs::assetpack
