// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS AssetPack — `.acpak` v1 アーカイブ読み出し器
// -----------------------------------------------------------------------------
// 1 つの `.acpak` ファイルを開き、含まれる仮想ファイルを名前 (wchar_t*) で
// 取り出すための実装クラス。GameFramework Pillar G の `IAssetPackReader` とは
// 独立して動作する (将来 `IAssetPackReader` の concrete 実装 = adapter として
// この AcpakReader を内部委譲する形を取る予定)。
//
// 使い方の典型例:
//   acs::assetpack::AcpakReader reader;
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
// Phase 1 制約 (Phase 2 で解除):
//   ・flags = 0 のみ対応。Encrypted / Compressed bit が立っているアーカイブ
//     を Open するとエラー (`kAcpakSubNotImplemented`)。
//   ・全 file は uncompressed / unencrypted 生バイトとして保存されている前提。
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
#include "assetpack/AcpakCrypto.h"  // AcpakKey (Phase 2: 暗号化 pak の鍵注入)

namespace acs::assetpack {

class AcpakReader {
public:
    AcpakReader() noexcept = default;
    ~AcpakReader() noexcept;

    AcpakReader(const AcpakReader&)            = delete;
    AcpakReader& operator=(const AcpakReader&) = delete;
    AcpakReader(AcpakReader&&)                 = delete;
    AcpakReader& operator=(AcpakReader&&)      = delete;

    // ---- ライフサイクル -----------------------------------------------------

    // `.acpak` ファイルを開き、header と file table を読み出す。
    //   ・file_path は UTF-16 (Windows convention)。
    //   ・成功すると以降の API が有効になる。多重 Open は前回を自動 Close する。
    //   ・失敗時は内部状態は Close() 後と同じになる (IsOpen() == false)。
    //   ・**暗号化 pak** (AcpakFlagEncrypted) を Open する前に SetKey() で
    //     鍵を与えておく必要がある。鍵なしで encrypted pak を開いて ReadFile
    //     を呼ぶと ACS_ERR(Asset, kAcpakSubCryptoKey) を返す。Open 自体は
    //     header と file table (= nonce/tag を含む) だけを読むので鍵不要。
    //
    // 主なエラー:
    //   ・ACS_ERR(IO, kAcpakSubIOFailure)     — CreateFileW / ReadFile 失敗
    //   ・ACS_ERR(IO, kAcpakSubBadSize)       — ヘッダより小さい
    //   ・ACS_ERR(Asset, kAcpakSubBadMagic)   — magic mismatch
    //   ・ACS_ERR(Asset, kAcpakSubBadVersion) — version mismatch
    //   ・ACS_ERR(Asset, kAcpakSubBadFlags)   — 未知 flags bit が立っている
    TResult<void> Open(const wchar_t* file_path) noexcept;

    // 暗号化 pak の復号鍵を設定する。Open の前後どちらでも呼べる。
    // ReadFile 内で AES-256-GCM 復号に使われる。flags=0 の pak では無視される。
    // Close すると内部の鍵情報も 0 クリアされる。
    void SetKey(const AcpakKey& key) noexcept;

    // ハンドルを閉じ、文字列 pool + entry 配列を解放する。
    // Open 前 / 既に Close 済でも安全 (no-op)。
    void Close() noexcept;

    // Open 成功後かつ Close 前なら true。
    bool IsOpen() const noexcept { return _file_handle != nullptr; }

    // ---- file table 参照 ----------------------------------------------------

    // 現在開いている pak に含まれる仮想ファイル数。未 Open なら 0。
    u32 FileCount() const noexcept { return static_cast<u32>(_entries.Size()); }

    // index 番目の entry を返す。範囲外 / 未 Open なら nullptr。
    // 返り値の寿命は「次の Close まで」。
    const AcpakFileEntry* GetEntry(u32 index) const noexcept;

    // 仮想パスから entry を探す。線形探索 (Phase 1 = 数百〜数千 entry 想定で
    // 十分高速)。見つからない / 未 Open なら nullptr。
    // path 比較は wcscmp 相当の完全一致。
    const AcpakFileEntry* FindEntry(const wchar_t* path) const noexcept;

    // ---- データ読み出し -----------------------------------------------------

    // 仮想パスのファイルを out_buffer に読み出す。
    //   ・buffer_size は GetUncompressedSize() の返す値以上必要。不足は
    //     ACS_ERR(IO, kAcpakSubBufferTooSmall)。
    //   ・Phase 1 では size_stored == size_uncompressed の生バイトを直接読む。
    //   ・読み出し後、CRC32 を計算し entry.crc32 と照合 → 不一致は
    //     ACS_ERR(Asset, kAcpakSubBadCrc) を返す (buf の中身は破棄せず残るが
    //     呼び出し側はエラー時の中身を使わないこと)。
    //   ・成功時の戻り値は実際に書き込んだバイト数 (= size_uncompressed)。
    TResult<u64> ReadFile(const wchar_t* path,
                         void*          out_buffer,
                         u64            buffer_size) noexcept;

    // 仮想パスの「復号 + 解凍後の」バイト数を返す。ReadFile 用バッファ事前
    // 確保に使う。未存在パスは ACS_ERR(IO, kAcpakSubNotFound)。
    TResult<u64> GetUncompressedSize(const wchar_t* path) const noexcept;

    // ---- 診断 ---------------------------------------------------------------

    // ヘッダから読み取った flags をそのまま返す。Phase 1 では常に 0 だが、
    // Phase 2 で encrypted / compressed が入ったアーカイブを Open 拒否する前に
    // 何が立っているかロガーで確認する用途。
    u32 Flags() const noexcept { return _flags; }

private:
    // ヘッダ + file table をハンドルから読み出して内部状態を構築する。
    // Open() からのみ呼ばれる。失敗時は呼び出し側 (= Open) が Close を呼ぶ。
    TResult<void> LoadHeaderAndTable() noexcept;

    // file_path の wchar_t を _string_pool に追加 (NUL 付き) し、その先頭
    // ポインタを返す。
    const wchar_t* InternPath(const wchar_t* src, u32 len) noexcept;

    // ---- 状態 ---------------------------------------------------------------
    // Win32 HANDLE は void* として保持し、<windows.h> を header から外す。
    // 実装側 (.cpp) で reinterpret_cast<HANDLE>(_file_handle) として使う。
    void*                 _file_handle = nullptr;   // Win32 HANDLE 相当
    u64                   _file_size   = 0;         // CreateFileW 直後の GetFileSizeEx
    u32                   _flags       = 0;         // header.flags
    u64                   _table_offset = 0;        // header.file_table_offset
    TArray<AcpakFileEntry> _entries;                 // file table の in-memory 表現
    TArray<wchar_t>        _string_pool;             // path 文字列の連結 (NUL 区切り)

    // Phase 2: 暗号化 pak の復号鍵。flags=0 のときは未使用。
    // Close で _has_key=false にリセットされ、_key も 0 クリアされる。
    AcpakKey              _key{};
    bool                  _has_key     = false;
};

} // namespace acs::assetpack
