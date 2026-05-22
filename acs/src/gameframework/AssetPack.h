// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar G — AssetPack (`.acpak` 暗号化アーカイブ I/F stub)
//
// 製品化に向けた「アセットのパッケージング + 暗号化」を担うエンジンモジュールの
// シーム (seam) インターフェース。開発中はバラのファイル、出荷時は 1 つの
// 不透明な `.acpak` にまとめ、ゲームコードを変えずに切り替えられる。
//
// 注: **本来は独立モジュール `ACS::AssetPack` (`src/assetpack/`) を作る** が、
// Phase 1 (= GameFramework Pillar G スケルトン) では GameFramework 内に
// interface stub のみを置く。AES-256-GCM 暗号 + LZ4 圧縮 + 認証タグ検証 等の
// 実体実装は Phase 2 で独立モジュールへ切り出して行う。
// 詳細仕様は `acs/docs/AssetPack.md` を参照。
//
// 使い方:
//   class AssetLoader {
//       acs::game::IAssetPackReader* _pack = nullptr;
//
//       void OnStart() noexcept override {
//           // 出荷ビルドでは GoldenAssetPackReader を DI、開発ビルドでは Stub。
//           _pack = &acs::game::GetReaderStub();
//           (void)_pack->Mount("game.acpak");
//       }
//       void LoadTexture(const char* name) noexcept {
//           if (auto sz = _pack->FileSize(name); sz.IsOk()) {
//               u8* buf = AllocateBuffer(sz.Value());
//               (void)_pack->ReadFile(name, buf, sz.Value());
//               // ... decode ...
//           }
//       }
//   };
//
// 設計選択 (Pillar G Phase 1):
//   ・**シーム (= 純粋仮想 I/F) として抽象化**: AES-GCM / LZ4 / bcrypt (Windows CNG)
//     依存は重く、それらをリンクしないテストビルドでも本 I/F だけは常に提供する。
//     実装は別モジュール (将来の `ACS::AssetPack`) で `IAssetPackReader` /
//     `IAssetPackWriter` を override する形を取る。
//   ・**Reader / Writer を別 I/F に分離**: ランタイムは Reader しか要らず、Writer は
//     ツール (パッキングコマンド) 側のみ使う。実装も別バイナリに分けやすくする。
//   ・**所有しない const char***: ファイル名 / pack パスは呼び出し側 / 実装側の
//     ライフタイムに従う。Bridge はコピーしない (STL <string> 不使用方針)。
//     `FileName(index)` の戻り値は「次の Mount/Unmount を呼ぶまで有効」と扱うこと。
//   ・**Result<T, ErrorCode> で例外なし**: ACS 全体方針に沿う。Stub は全 API を
//     `ACS_ERR(Generic, kSubAssetPackNotImplemented, ...)` で返す。
//   ・**Stub は static singleton で取得**: 依存ゼロのデフォルト実装として
//     `GetReaderStub()` / `GetWriterStub()` を提供。実 AssetPack 未統合の
//     ビルドでもポインタ DI だけでコンパイル可能。
//   ・**実 AssetPack 実装はここでは作らない**: GoldenAssetPackReader 等は AES-GCM
//     CNG / LZ4 への依存を伴うため、本ファイルでは I/F + Stub のみ。
//
// 範囲外 (Phase 2+ で):
//   ・実 `.acpak` フォーマットの読み書き (ヘッダ / TOC / ブロブ領域)。
//   ・AES-256-GCM 復号 + 認証タグ検証、LZ4 解凍。
//   ・パスヒープ暗号化、ハッシュのみモード、追記 patch pak。
//   ・非同期ストリーミング (Mount は同期 mmap 前提、ReadFile も同期コピー)。
//   ・複数 pak のスタック (overlay) — pak A 上書き pak B の優先解決。
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"

namespace acs::game {

// ---- ErrorCode subcode 定義 (ErrCategory::Generic 配下) ------------------
// SteamworksBridge (1001-1099) / WorkshopBridge (1101-1199) と subcode 空間が
// 重ならないよう、AssetPack は 1200 番台を使う。
inline constexpr u16 kSubAssetPackNotImplemented = 1201;  // Stub による未実装
inline constexpr u16 kSubAssetPackNotMounted     = 1202;  // Mount() 前の API 呼び出し

// ---- AssetPackInfo (現在マウント中の pak の最小情報) ---------------------
// Bridge は文字列を所有しない。`file_path` は呼び出し側 (or 実装内 static literal) の
// メモリを参照するだけで、利用側でコピーしないこと。寿命は「次の Unmount/Mount を
// 呼ぶまで」を保証する。
struct AssetPackInfo {
    const char* file_path    = nullptr;  // Mount した pak ファイルの絶対 / 相対パス
    u64         content_hash = 0;        // pak 全体の content hash (改竄検知用)
    bool        encrypted    = false;    // AES-256-GCM 暗号化されているか
    bool        mounted      = false;    // 現在マウント中か
};

// ---- 抽象 I/F: Reader -----------------------------------------------------
// `.acpak` を読み出すためのインターフェース。ランタイムが利用する側。
// 実装は本体外モジュール (or テスト) で Override する。
class IAssetPackReader {
public:
    IAssetPackReader() noexcept = default;
    virtual ~IAssetPackReader() noexcept = default;

    IAssetPackReader(const IAssetPackReader&)            = delete;
    IAssetPackReader& operator=(const IAssetPackReader&) = delete;
    IAssetPackReader(IAssetPackReader&&)                 = delete;
    IAssetPackReader& operator=(IAssetPackReader&&)      = delete;

    // pak ファイルをマウントする。成功すると以降の API 呼び出しが有効になる。
    // 多重 Mount は実装依存 (大抵は前の pak を自動 Unmount してから新規 Mount)。
    virtual Result<void> Mount(const char* pack_path) noexcept = 0;

    // 現在の pak をアンマウントする。Mount() 前に呼んでも安全 (no-op)。
    virtual void Unmount() noexcept = 0;

    // Mount() 成功後かつ Unmount() 前なら true。
    virtual bool IsMounted() const noexcept = 0;

    // 現在マウント中の pak に含まれるファイル数。
    virtual Result<u32> FileCount() noexcept = 0;

    // index 番目のファイル名 (UTF-8、pak 内仮想パス)。返り値の文字列の寿命は
    // 「次の Unmount/Mount を呼ぶまで」。範囲外 index はエラー。
    virtual Result<const char*> FileName(u32 index) noexcept = 0;

    // 仮想ファイル名から復号 + 解凍後のオリジナルサイズを取得する。
    // ReadFile に渡すバッファサイズ事前確保用。未存在パスはエラー。
    virtual Result<u64> FileSize(const char* name) noexcept = 0;

    // 仮想ファイル名のデータを out_buffer に復号 + 解凍してコピーする。
    // buffer_size は FileSize() 戻り値以上必要。不足はエラー。
    virtual Result<void> ReadFile(const char* name, u8* out_buffer, u64 buffer_size) noexcept = 0;
};

// ---- 抽象 I/F: Writer -----------------------------------------------------
// `.acpak` を新規作成するためのインターフェース。ツール (パッキングコマンド)
// 側のみ利用する。ランタイムは Reader だけで足りる。
class IAssetPackWriter {
public:
    IAssetPackWriter() noexcept = default;
    virtual ~IAssetPackWriter() noexcept = default;

    IAssetPackWriter(const IAssetPackWriter&)            = delete;
    IAssetPackWriter& operator=(const IAssetPackWriter&) = delete;
    IAssetPackWriter(IAssetPackWriter&&)                 = delete;
    IAssetPackWriter& operator=(IAssetPackWriter&&)      = delete;

    // 出力 pak ファイルを開いて書き込みを開始する。output_path 既存ファイルは
    // 実装依存 (大抵は truncate 上書き)。BeginPack / FinishPack 対で使う。
    virtual Result<void> BeginPack(const char* output_path) noexcept = 0;

    // 1 ファイルを pak に追加する。virtual_name は pak 内仮想パス、data はオリジナル
    // (非圧縮 / 非暗号) バイト列、size はそのサイズ。BeginPack 前に呼ぶとエラー。
    // 実装は compress-then-encrypt 順 (AssetPack.md §5) で TOC / ブロブを構築する。
    virtual Result<void> AddFile(const char* virtual_name, const u8* data, u64 size) noexcept = 0;

    // pak を確定して書き込みを終える。TOC を暗号化し、ヘッダの content_hash を
    // 確定する。これ以降の AddFile 呼び出しはエラー。
    virtual Result<void> FinishPack() noexcept = 0;
};

// ---- Stub 実装: Reader ----------------------------------------------------
// AssetPack 未統合ビルド / ユニットテスト用の no-op 実装。
//   ・Mount() / FileCount() / FileName() / FileSize() / ReadFile() は全て
//     ACS_ERR(Generic, kSubAssetPackNotImplemented) を返す。
//   ・Unmount() は副作用なし。IsMounted() は常に false。
class AssetPackReaderStub final : public IAssetPackReader {
public:
    AssetPackReaderStub() noexcept = default;
    ~AssetPackReaderStub() noexcept override = default;

    Result<void>         Mount(const char* pack_path) noexcept override;
    void                 Unmount() noexcept override;
    bool                 IsMounted() const noexcept override { return false; }
    Result<u32>          FileCount() noexcept override;
    Result<const char*>  FileName(u32 index) noexcept override;
    Result<u64>          FileSize(const char* name) noexcept override;
    Result<void>         ReadFile(const char* name, u8* out_buffer, u64 buffer_size) noexcept override;
};

// ---- Stub 実装: Writer ----------------------------------------------------
// 同上の Writer 側 no-op 実装。全 API が NotImplemented を返す。
class AssetPackWriterStub final : public IAssetPackWriter {
public:
    AssetPackWriterStub() noexcept = default;
    ~AssetPackWriterStub() noexcept override = default;

    Result<void>  BeginPack(const char* output_path) noexcept override;
    Result<void>  AddFile(const char* virtual_name, const u8* data, u64 size) noexcept override;
    Result<void>  FinishPack() noexcept override;
};

// ---- static singleton accessors -----------------------------------------
// 実 AssetPack 実装が DI される前のデフォルト stub。Meyer's singleton。
IAssetPackReader& GetReaderStub() noexcept;
IAssetPackWriter& GetWriterStub() noexcept;

} // namespace acs::game
