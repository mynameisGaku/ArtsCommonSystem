// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS FAssetPack — `.acpak` v1 アーカイブ読み出し器
// -----------------------------------------------------------------------------
// 1 つの `.acpak` ファイルを開き、含まれる仮想ファイルを名前 (wchar_t*) で
// 取り出すための実装クラス。GameFramework の `IAssetPackReader` とは
// 独立して動作する。
//
// 使い方の典型例:
//   acs::assetpack::FAcpakReader reader;
//   auto r = reader.Open(L"game.acpak");
//   if (r.IsErr()) { /* マウント失敗 */ }
//
//   auto sz = reader.GetUncompressedSize(L"textures/hero.png");
//   if (sz.IsOk()) {
//       acs::byte* buf = MyAllocator().Allocate(sz.Value());
//       auto rd = reader.ReadFile(L"textures/hero.png", buf, sz.Value());
//       if (rd.IsOk()) { /* buf に PNG 生バイト */ }
//   }
//
//   reader.Close();
//
// 非コピー・非ムーブ:
//   ファイルハンドル + 文字列 pool + entry 配列を所有しているため、所有権移転は
//   サポートしない。Reader インスタンスは固定アドレスでライフタイム管理する。
// =============================================================================
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"
#include "container/Array.h"

#include "assetpack/AcpakFormat.h"
#include "assetpack/AcpakCrypto.h"  // AcpakKey (暗号化 pak の鍵注入)

namespace acs::assetpack {

/**
 * 1 つの `.acpak` を開き、含まれる仮想ファイルを名前で取り出す Reader。
 *
 * @details
 * header と file table を読み込み、各 path を内部の文字列 pool に保持して
 * 名前 (wchar_t*) で検索・読み出しする。ファイルハンドル + 文字列 pool +
 * entry 配列を所有するため non-copy / non-move で、固定アドレスでライフタイムを
 * 管理する。GameFramework の IAssetPackReader とは独立に動作する。
 */
class FAcpakReader {
public:
    /** 空状態で構築する (ファイルは Open で開く)。 */
    FAcpakReader() noexcept = default;

    /** 破棄する (Open 済なら Close 相当の後始末を行う)。 */
    ~FAcpakReader() noexcept;

    /** コピー禁止 (ハンドル + pool を単独所有するため)。 */
    FAcpakReader(const FAcpakReader&)            = delete;

    /** コピー代入も禁止。 */
    FAcpakReader& operator=(const FAcpakReader&) = delete;

    /** ムーブ禁止 (entry.path が内部 pool を指すため)。 */
    FAcpakReader(FAcpakReader&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FAcpakReader& operator=(FAcpakReader&&)      = delete;

    /**
     * `.acpak` ファイルを開き、header と file table を読み出す。
     *
     * @details
     * file_path は UTF-16 (Windows convention)。成功すると以降の API が有効に
     * なる。多重 Open は前回を自動 Close する。失敗時は内部状態が Close() 後と
     * 同じになる (IsOpen() == false)。暗号化 pak (AcpakFlagEncrypted) は Open の
     * 前に SetKey() で鍵を与えておく必要がある (Open 自体は header と file table
     * = nonce/tag を含む までしか読まないので鍵不要だが、鍵なしで encrypted pak
     * の ReadFile を呼ぶと kAcpakSubCryptoKey を返す)。失敗時は
     * kAcpakSubIOFailure (CreateFileW/ReadFile 失敗) / kAcpakSubBadSize
     * (ヘッダより小さい) / kAcpakSubBadMagic / kAcpakSubBadVersion /
     * kAcpakSubBadFlags (未知 flags bit)。
     * @param file_path 開く `.acpak` の UTF-16 パス。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    TResult<void> Open(const wchar_t* file_path) noexcept;

    /**
     * 暗号化 pak の復号鍵を設定する。
     *
     * @details
     * Open の前後どちらでも呼べる。ReadFile 内で AES-256-GCM 復号に使われ、
     * flags=0 の pak では無視される。Close すると内部の鍵情報も 0 クリアされる。
     * @param key 設定する AES-256 鍵。
     */
    void SetKey(const FAcpakKey& key) noexcept;

    /**
     * ハンドルを閉じ、文字列 pool + entry 配列を解放する。
     *
     * @details Open 前 / 既に Close 済でも安全 (no-op)。鍵情報も 0 クリアする。
     */
    void Close() noexcept;

    /**
     * pak が開いているかを返す。
     *
     * @return Open 成功後かつ Close 前なら true。
     */
    bool IsOpen() const noexcept { return m_FileHandle != nullptr; }

    /**
     * 現在開いている pak に含まれる仮想ファイル数を返す。
     *
     * @return 仮想ファイル数 (未 Open なら 0)。
     */
    u32 FileCount() const noexcept { return static_cast<u32>(m_Entries.Size()); }

    /**
     * index 番目の entry を返す。
     *
     * @details 返り値の寿命は次の Close まで。
     * @param index 取得する entry のインデックス。
     * @return entry へのポインタ (範囲外 / 未 Open なら nullptr)。
     */
    const FAcpakFileEntry* GetEntry(u32 index) const noexcept;

    /**
     * 仮想パスから entry を探す。
     *
     * @details
     * 線形探索 (数百〜数千 entry 想定で十分高速)。比較は wcscmp 相当の完全一致。
     * @param path 探す仮想パス (UTF-16)。
     * @return 見つかった entry (無い / 未 Open なら nullptr)。
     */
    const FAcpakFileEntry* FindEntry(const wchar_t* path) const noexcept;

    /**
     * 仮想パスのファイルを out_buffer に読み出す (復号 + 解凍 + CRC 検証)。
     *
     * @details
     * buffer_size は GetUncompressedSize() の返す値以上必要 (不足は
     * kAcpakSubBufferTooSmall)。読み出し後 CRC32 を entry.crc32 と照合し、不一致は
     * kAcpakSubBadCrc を返す (エラー時の buffer 内容は使わないこと)。
     * @param path 読み出す仮想パス (UTF-16)。
     * @param out_buffer 読み出し先バッファ。
     * @param buffer_size out_buffer の容量バイト数。
     * @return 実際に書き込んだバイト数 (= size_uncompressed)、失敗ならエラー。
     */
    TResult<u64> ReadFile(const wchar_t* path,
                         void*          out_buffer,
                         u64            buffer_size) noexcept;

    /**
     * 仮想パスの復号 + 解凍後のバイト数を返す。
     *
     * @details ReadFile 用バッファの事前確保に使う。
     * @param path サイズを問い合わせる仮想パス (UTF-16)。
     * @return size_uncompressed バイト数 (未存在パスは kAcpakSubNotFound)。
     */
    TResult<u64> GetUncompressedSize(const wchar_t* path) const noexcept;

    /**
     * ヘッダから読み取った flags をそのまま返す。
     *
     * @details encrypted / compressed のどのビットが立っているかを診断する用途。
     * @return header.flags の値。
     */
    u32 Flags() const noexcept { return m_Flags; }

private:
    /**
     * ヘッダ + file table をハンドルから読み出して内部状態を構築する。
     *
     * @details Open() からのみ呼ばれる。失敗時は呼び出し側 (Open) が Close する。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    TResult<void> LoadHeaderAndTable() noexcept;

    /**
     * src の wchar_t 列を m_StringPool に NUL 付きで追加し、その先頭を返す。
     *
     * @param src 追加する文字列。
     * @param len src の wchar_t 数。
     * @return pool 内に確保された文字列の先頭ポインタ。
     */
    const wchar_t* InternPath(const wchar_t* src, u32 len) noexcept;

    /** Win32 HANDLE 相当 (<windows.h> を header から外すため void* で保持)。 */
    void*                 m_FileHandle = nullptr;

    /** CreateFileW 直後に GetFileSizeEx で得たファイル長。 */
    u64                   m_FileSize   = 0;

    /** header.flags (encrypted / compressed)。 */
    u32                   m_Flags       = 0;

    /** header.file_table_offset。 */
    u64                   m_TableOffset = 0;

    /** file table の in-memory 表現 (entry.path は m_StringPool を指す)。 */
    TArray<FAcpakFileEntry> m_Entries;

    /** path 文字列の連結 pool (NUL 区切り)。 */
    TArray<wchar_t>        m_StringPool;

    /** 暗号化 pak の復号鍵 (flags=0 のときは未使用、Close で 0 クリア)。 */
    FAcpakKey              m_Key{};

    /** SetKey で鍵が設定されたか (Close で false にリセット)。 */
    bool                  m_HasKey     = false;
};

} // namespace acs::assetpack
