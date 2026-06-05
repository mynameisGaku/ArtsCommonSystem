// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS FAssetPack — `.acpak` v1 アーカイブ書き出し器
// -----------------------------------------------------------------------------
// 複数のバラのファイル (= バイト列) を 1 つの `.acpak` にまとめる Writer。
// ツールビルド (= パッキングコマンド) から使うことを想定し、ランタイムは
// FAcpakReader だけで足りる。
//
// 使い方の典型例:
//   acs::assetpack::FAcpakWriter w;
//   auto r = w.Open(L"out/game.acpak", acs::assetpack::AcpakFlagNone);
//   if (r.IsErr()) { /* 開けない */ }
//
//   for (auto& asset : assets) {
//       w.AddFile(asset.virtual_path, asset.bytes, asset.size);
//   }
//
//   auto fin = w.Finalize();   // header + 全 data + file table を書き出す
//   if (fin.IsErr()) { /* 書き出し失敗 */ }
//   w.Close();                  // ハンドルを閉じる (Finalize 失敗時のロールバックもここ)
//
// 設計:
//   ・AddFile はその場ではファイルに書かず、内部 pending list にポインタ +
//     サイズだけを積む。実書き込みは Finalize 内で一気に行う。これにより
//     呼び出し側のデータ寿命要件は「Open〜Finalize の間」だけで済む。
//   ・data ポインタは呼び出し側所有 — Writer はコピーを取らない。寿命管理
//     ミスを早期検知するため、Finalize 完了後 Close するまで data は触れない
//     とドキュメント上明記する。
// 非コピー・非ムーブ。
// =============================================================================
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"
#include "container/Array.h"

#include "assetpack/AcpakFormat.h"
#include "assetpack/AcpakCrypto.h"  // AcpakKey (暗号化 pak の鍵注入)

namespace acs::assetpack {

/**
 * 複数のバラのファイルを 1 つの `.acpak` にまとめる Writer。
 *
 * @details
 * AddFile は実書き込みせず内部の pending list にポインタ + サイズだけを積み、
 * 実書き込みは Finalize 内で一気に行う。data ポインタは呼び出し側所有で Writer は
 * コピーを取らないため、data の寿命は Open〜Finalize の間保つこと。ツールビルド
 * (パッキングコマンド) から使う想定で、ランタイムは FAcpakReader だけで足りる。
 * ハンドル + pending list を所有するため non-copy / non-move。
 */
class FAcpakWriter {
public:
    /** 空状態で構築する (出力は Open で開く)。 */
    FAcpakWriter() noexcept = default;

    /** 破棄する (Open 済なら Close 相当の後始末を行う)。 */
    ~FAcpakWriter() noexcept;

    /** コピー禁止 (ハンドル + pending list を単独所有するため)。 */
    FAcpakWriter(const FAcpakWriter&)            = delete;

    /** コピー代入も禁止。 */
    FAcpakWriter& operator=(const FAcpakWriter&) = delete;

    /** ムーブ禁止 (固定アドレスでライフタイムを管理するため)。 */
    FAcpakWriter(FAcpakWriter&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FAcpakWriter& operator=(FAcpakWriter&&)      = delete;

    /**
     * 出力ファイルを開き、ヘッダのプレースホルダを書く (既存は上書き)。
     *
     * @details
     * CREATE_ALWAYS で開き、file_count / file_table_offset を 0 にしたヘッダを先に
     * 書く (実値は Finalize で seek して上書き)。成功すると以降 AddFile / Finalize が
     * 呼べる。flags は AcpakFlagNone / AcpakFlagCompressed / AcpakFlagEncrypted の
     * 任意組み合わせ。Encrypted を立てる場合は Open より前に SetKey() で鍵を設定する
     * こと (鍵未設定でも Open 自体は成功し、Finalize 時に kAcpakSubCryptoKey を返す)。
     * 既に Open 状態なら kAcpakSubAlreadyOpen、未知 flag bit は kAcpakSubBadFlags。
     * @param output_path 出力する `.acpak` の UTF-16 パス。
     * @param flags 書き出しオプション (圧縮 / 暗号化)。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    TResult<void> Open(const wchar_t* output_path, EAcpakFlags flags) noexcept;

    /**
     * 暗号化用の鍵を設定する。
     *
     * @details
     * Open 前後どちらでも呼べる。flags に AcpakFlagEncrypted が含まれるとき
     * Finalize で AES-256-GCM 暗号化に使われる。Close すると 0 クリアされる。
     * @param key 設定する AES-256 鍵。
     */
    void SetKey(const FAcpakKey& key) noexcept;

    /**
     * ハンドルを閉じて内部状態をクリアする (多重 Close は no-op)。
     *
     * @details
     * Finalize 前に呼ぶと書きかけアーカイブがディスクに残る (DeleteFileW は
     * 呼ばない)。鍵情報も 0 クリアする。
     */
    void Close() noexcept;

    /**
     * 1 ファイルを pak に追加する (実書き込みはせず pending list に積む)。
     *
     * @details
     * virtual_path / data は呼び出し側所有のため Open〜Finalize の間ポインタ寿命を
     * 保つこと (Writer はコピーしない)。size 0 のファイルも追加できる。Open 前 /
     * Finalize 後に呼ぶと kAcpakSubNotOpen、data が null かつ size>0 は
     * kAcpakSubIOFailure。
     * @param virtual_path pak 内の仮想パス (UTF-16、wcscmp で検索される)。
     * @param data 追加するファイルの生バイト列。
     * @param size data のバイト数。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    TResult<void> AddFile(const wchar_t* virtual_path,
                         const void*    data,
                         u64            size) noexcept;

    /**
     * pak を確定し、header → 全ファイルデータ → file table の順で書き出す。
     *
     * @details
     * 各 entry を compress-then-encrypt パイプライン (圧縮 → 暗号化、flags 依存) に
     * 通し、CRC32 (元バイト基準) を file table に格納する。最後にヘッダを seek して
     * file_count / file_table_offset を確定し FlushFileBuffers する。成功時は
     * ハンドルが flush 済 (Close を呼んでよい)。Open 前 / 2 回目の呼び出しは
     * kAcpakSubNotOpen、暗号化 flag 立ちで鍵未設定は kAcpakSubCryptoKey。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    TResult<void> Finalize() noexcept;

private:
    /**
     * AddFile が積み Finalize が消費する pending entry の生表現。
     *
     * @details ポインタはすべて呼び出し側所有 — Writer はコピーを取らない。
     */
    struct PendingEntry {
        /** 仮想パス (UTF-16、wcscmp 比較)。 */
        const wchar_t* path;

        /** 追加するファイルの生バイト列。 */
        const void*    data;

        /** data のバイト数。 */
        u64            size;
    };

    /** Finalize 後 / Open 失敗時に内部状態 (flags / pending / 鍵) をクリアする。 */
    void ResetState() noexcept;

    /** Win32 HANDLE 相当 (<windows.h> を header から外すため void* で保持)。 */
    void*               m_FileHandle = nullptr;

    /** header.flags (encrypted / compressed)。 */
    u32                 m_Flags       = 0;

    /** Finalize 済フラグ (2 回目の AddFile / Finalize を弾く)。 */
    bool                m_Finalized   = false;

    /** AddFile が積んだ entry 群 (Finalize で消費)。 */
    TArray<PendingEntry> m_Pending;

    /** 暗号化鍵 (AcpakFlagEncrypted のときに Finalize で使う、Close で 0 クリア)。 */
    FAcpakKey            m_Key{};

    /** SetKey で鍵が設定されたか (Close で false にリセット)。 */
    bool                m_HasKey     = false;
};

} // namespace acs::assetpack
