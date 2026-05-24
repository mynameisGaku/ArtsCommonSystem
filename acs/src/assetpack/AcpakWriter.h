// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS AssetPack — `.acpak` v1 アーカイブ書き出し器
// -----------------------------------------------------------------------------
// 複数のバラのファイル (= バイト列) を 1 つの `.acpak` にまとめる Writer。
// ツールビルド (= パッキングコマンド) から使うことを想定し、ランタイムは
// AcpakReader だけで足りる。
//
// 使い方の典型例:
//   acs::assetpack::AcpakWriter w;
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

namespace acs::assetpack {

class AcpakWriter {
public:
    AcpakWriter() noexcept = default;
    ~AcpakWriter() noexcept;

    AcpakWriter(const AcpakWriter&)            = delete;
    AcpakWriter& operator=(const AcpakWriter&) = delete;
    AcpakWriter(AcpakWriter&&)                 = delete;
    AcpakWriter& operator=(AcpakWriter&&)      = delete;

    // ---- ライフサイクル -----------------------------------------------------

    // 出力ファイルを開く。既存ファイルは上書き (CREATE_ALWAYS)。
    //   ・flags は AcpakFlagNone のみ実装 (= Phase 1)。
    //   ・Encrypted / Compressed bit を含むと ACS_ERR(Asset, kAcpakSubNotImplemented)。
    //   ・既に Open 状態なら ACS_ERR(IO, kAcpakSubAlreadyOpen)。
    //   ・成功すると以降 AddFile / Finalize が呼べる。
    Result<void> Open(const wchar_t* output_path, EAcpakFlags flags) noexcept;

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
    Result<void> AddFile(const wchar_t* virtual_path,
                         const void*    data,
                         u64            size) noexcept;

    // pak を確定する。
    //   ・header → 全ファイルデータ → file table の順で書き出す。
    //   ・各ファイルの CRC32 を計算し、file table の crc32 フィールドに格納する。
    //   ・成功時はハンドルが flush 済 (Close を呼んでよい)。
    //   ・Open 前 / 2 回目の呼び出しは ACS_ERR(IO, kAcpakSubNotOpen)。
    Result<void> Finalize() noexcept;

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

    void*               _file_handle = nullptr;   // Win32 HANDLE 相当
    u32                 _flags       = 0;         // header.flags
    bool                _finalized   = false;     // Finalize 済か
    Array<PendingEntry> _pending;                 // AddFile が積んだ entry 群
};

} // namespace acs::assetpack
