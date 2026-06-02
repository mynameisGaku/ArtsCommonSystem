// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar G — FAssetBundle (シーンスコープのアセット集合非同期ロード)
//
// 役割:
//   シーンが「このシーンで使う N 個のアセット」をまとめて宣言し、BeginLoad() で
//   一括非同期ロード → 集約進捗 (Progress / IsLoaded / HasFailed) を確認する。
//   個別 path の FAssetFuture を散在管理する代わりに 1 つの bundle で扱える。
//
// 使い方 (典型例):
//   class GameplayScene : public Scene {
//       acs::game::FAssetBundle m_Bundle;
//       void OnEnter() noexcept override {
//           m_Bundle.Add("textures/hero.png");
//           m_Bundle.Add("audio/bgm.ogg");
//           m_Bundle.Add("meshes/level.glb");
//           m_Bundle.BeginLoad();
//       }
//       void OnUpdate(f32) noexcept override {
//           if (!m_Bundle.IsLoaded()) {
//               // ローディング画面: m_Bundle.Progress() を表示
//               return;
//           }
//           // 通常ゲームループ
//       }
//       void OnExit() noexcept override { m_Bundle.Unload(); }
//   };
//
// 設計方針:
//   ・シーン死亡で bundle 廃棄 → 内部 TSharedPtr<Asset> が drop → FAssetRegistry の refcount
//     が下がり、他参照がなければ実体メモリも解放される (GC 不要 / 決定的解放)。
//   ・path 文字列は呼び出し側が寿命を保証する (string literal / 永続バッファ前提)。
//     ACS 規約により <string> は使わない (const char* を TArray に保持)。
//   ・実 FAssetRegistry 接続は bridge スケルトンとして TODO 化。Phase G-2 で
//     FAssetFuture を統合し、Tick() で counter.Finished() を polling する想定。
//   ・全 API noexcept (例外は使わない / TResult も bundle 表層では露出しない)。
#pragma once

#include "foundation/Types.h"
#include "foundation/Log.h"
#include "container/Array.h"
#include "memory/SharedPtr.h"
#include "asset/Asset.h"

namespace acs { class FAssetRegistry; }

namespace acs::game {

// FAssetBundle: シーンスコープのアセット集合非同期ロード + 集約進捗。
//
// ライフサイクル: Add* → BeginLoad → (poll Progress/IsLoaded) → ... → Unload。
// BeginLoad 後の Add は無視する (ロード開始後の集合変更は未定義になるのを避けるため)。
class FAssetBundle {
public:
    // bundle 内の各 asset の進捗状態。
    enum class LoadStatus : u8 {
        Pending = 0,  // BeginLoad 前、または BeginLoad 失敗で開始されなかった
        Loading = 1,  // LoadAsync 発行済 / 完了待ち
        Loaded  = 2,  // 完了 (成功)
        Failed  = 3,  // 完了 (失敗)
    };

    FAssetBundle() noexcept = default;
    ~FAssetBundle() noexcept = default;

    // bundle は Scene にメンバとして埋め込む想定。コピー禁止 (内部 TArray 規約)。
    FAssetBundle(const FAssetBundle&)            = delete;
    FAssetBundle& operator=(const FAssetBundle&) = delete;

    // bundle に asset パスを追加する。実体ロードはまだ走らない (BeginLoad で開始)。
    // path 文字列は呼び出し側が FAssetBundle 寿命中ずっと有効である必要がある
    // (string literal を想定。動的文字列はキャストして渡す側で保持する)。
    // BeginLoad 後の Add は no-op (警告ログのみ)。
    void Add(const char* asset_path) noexcept;

    // 全 path を registry 経由で実ロードし、結果 (TSharedPtr<Asset>) を各 entry に保持する。
    // registry は app が所有するもの (FApplication::GetAssets() / FGame 経由) を渡す。
    // RegisterDefaultLoaders() 済みであること。多重呼び出しは no-op (警告のみ)。
    // 同期ロード (FAssetRegistry::Load): 戻った時点で各 entry は Loaded / Failed 確定。
    void BeginLoad(FAssetRegistry& registry) noexcept;

    // ロード済み asset を取得する (未ロード / 範囲外 / 失敗は空 TSharedPtr)。index は Add 順。
    TSharedPtr<Asset> GetAsset(u32 index) const noexcept;
    // path 一致の asset を取得 (見つからない / 失敗は空 TSharedPtr)。
    TSharedPtr<Asset> FindAsset(const char* asset_path) const noexcept;

    // 完了割合 [0, 1]。BeginLoad 未呼び出しは 0、全 asset 完了 (成功/失敗問わず) で 1。
    // 「ローディング画面のプログレスバー」用途を想定。
    f32 Progress() const noexcept;

    // 全 asset が完了 (Loaded または Failed) しているか。空 bundle は true。
    bool IsLoaded() const noexcept;

    // 1 個でも Failed が含まれるか。シーン側でエラーフォールバック判断用。
    bool HasFailed() const noexcept;

    // bundle に登録された asset 総数。
    u32 AssetCount() const noexcept;

    // 完了 (Loaded のみ、Failed は含まない) した asset 数。
    u32 LoadedCount() const noexcept;

    // 全 asset を解放する。FAssetRegistry に release 通知し、内部 TArray を Pending に戻す。
    // シーン破棄前にこれを呼ぶと決定的タイミングで refcount を落とせる
    // (デストラクタに任せても良いが、Scene::OnExit で明示的に呼ぶのが推奨)。
    void Unload() noexcept;

private:
    // 1 個の asset エントリ。
    // path は呼び出し側所有の文字列を借用するだけ (<string> 禁止規約)。
    struct Entry {
        const char* path   = nullptr;
        LoadStatus  status = LoadStatus::Pending;
        TSharedPtr<Asset>  asset;   // BeginLoad で registry からロードした実体 (失敗時は空)。
                             // bundle が参照を保持することで、Unload まで生存を保証する。
    };

    // bundle が BeginLoad を 1 度でも実行したか (Add の閉鎖判定用)。
    bool          m_bBegun = false;
    TArray<Entry>  m_Entries;
};

} // namespace acs::game
