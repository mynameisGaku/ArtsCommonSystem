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
//   ・Phase 1 では flags = 0 のみ実装。Encrypted / Compressed bit を立てて
//     Open すると ACS_ERR(Asset, kAcpakSubNotImplemented) を返す。
//
// 非コピー・非ムーブ。
// =============================================================================
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"
#include "container/Array.h"

#include "assetpack/AcpakFormat.h"
#include "assetpack/AcpakCrypto.h"  // AcpakKey (Phase 2: 暗号化 pak の鍵注入)

namespace acs::assetpack {

class FAcpakWriter {
public:
    FAcpakWriter() noexcept = default;
    ~FAcpakWriter() noexcept;

    FAcpakWriter(const FAcpakWriter&)            = delete;
    FAcpakWriter& operator=(const FAcpakWriter&) = delete;
    FAcpakWriter(FAcpakWriter&&)                 = delete;
    FAcpakWriter& operator=(FAcpakWriter&&)      = delete;

    // ---- ライフサイクル -----------------------------------------------------

    // 出力ファイルを開く。既存ファイルは上書き (CREATE_ALWAYS)。
    //   ・flags は AcpakFlagNone / AcpakFlagCompressed / AcpakFlagEncrypted の
    //     任意組み合わせ (Phase 2 で全 bit 実装済)。
    //   ・Encrypted を立てる場合は **Open より前に** SetKey() を呼んで鍵を
    //     設定すること。鍵未設定で Encrypted Open すると Finalize 時に
    //     ACS_ERR(Asset, kAcpakSubCryptoKey) を返す (Open 自体は成功する)。
    //   ・既に Open 状態なら ACS_ERR(IO, kAcpakSubAlreadyOpen)。
    //   ・成功すると以降 AddFile / Finalize が呼べる。
    TResult<void> Open(const wchar_t* output_path, EAcpakFlags flags) noexcept;

    // 暗号化用の鍵を設定する。Open 前後どちらでも呼べる。
    // flags に AcpakFlagEncrypted が含まれているときに Finalize で使われる。
    void SetKey(const FAcpakKey& key) noexcept;

    // ハンドルを閉じる。Finalize 前に呼ぶと書きかけアーカイブが残るため、
    // ベストエフォートでファイルを削除する (実装は単純に CloseHandle だけ呼ぶ
    // — DeleteFileW は呼ばない。テスト挙動が分かりにくくなるため)。
    // 多重 Close は安全 (no-op)。
    void Close() noexcept;

    // ---- エントリ追加 -------------------------------------------------------

    // 1 ファイルを pak に追加する。
    //   ・virtual_path は pak 内仮想パス (UTF-16、wcscmp で検索される)。
    //     Open〜Finalize の間ポインタ寿命を保つこと (Writer はコピーしない)。
    //   ・data / size はそのファイルの生バイト列。同様にポインタ寿命を保つ。
    //   ・Open 前に呼ぶと ACS_ERR(IO, kAcpakSubNotOpen)。
    //   ・size 0 のファイルも追加可能 (offset は header の直後を指す)。
    TResult<void> AddFile(const wchar_t* virtual_path,
                         const void*    data,
                         u64            size) noexcept;

    // pak を確定する。
    //   ・header → 全ファイルデータ → file table の順で書き出す。
    //   ・各ファイルの CRC32 を計算し、file table の crc32 フィールドに格納する。
    //   ・成功時はハンドルが flush 済 (Close を呼んでよい)。
    //   ・Open 前 / 2 回目の呼び出しは ACS_ERR(IO, kAcpakSubNotOpen)。
    TResult<void> Finalize() noexcept;

private:
    // pending entry の生表現。AddFile が積み、Finalize が消費する。
    // ポインタはすべて呼び出し側所有 — Writer はコピーを取らない。
    struct PendingEntry {
        const wchar_t* path;   // 仮想パス (UTF-16、wcscmp 比較)
        const void*    data;   // 生バイト
        u64            size;   // バイト数
    };

    // Finalize 後 (or Open 失敗時) に状態をクリアする。
    void ResetState() noexcept;

    void*               m_FileHandle = nullptr;   // Win32 HANDLE 相当
    u32                 m_Flags       = 0;         // header.flags
    bool                m_Finalized   = false;     // Finalize 済か
    TArray<PendingEntry> m_Pending;                 // AddFile が積んだ entry 群

    // Phase 2: 暗号化鍵 (AcpakFlagEncrypted のときに Finalize で使う)。
    FAcpakKey            m_Key{};
    bool                m_HasKey     = false;
};

} // namespace acs::assetpack
