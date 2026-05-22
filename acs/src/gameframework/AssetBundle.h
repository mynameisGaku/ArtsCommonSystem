// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar G — AssetBundle (シーンスコープのアセット集合非同期ロード)
//
// 役割:
//   シーンが「このシーンで使う N 個のアセット」をまとめて宣言し、BeginLoad() で
//   一括非同期ロード → 集約進捗 (Progress / IsLoaded / HasFailed) を確認する。
//   個別 path の AssetFuture を散在管理する代わりに 1 つの bundle で扱える。
//
// 使い方 (典型例):
//   class GameplayScene : public Scene {
//       acs::game::AssetBundle _bundle;
//       void OnEnter() noexcept override {
//           _bundle.Add("textures/hero.png");
//           _bundle.Add("audio/bgm.ogg");
//           _bundle.Add("meshes/level.glb");
//           _bundle.BeginLoad();
//       }
//       void OnUpdate(f32) noexcept override {
//           if (!_bundle.IsLoaded()) {
//               // ローディング画面: _bundle.Progress() を表示
//               return;
//           }
//           // 通常ゲームループ
//       }
//       void OnExit() noexcept override { _bundle.Unload(); }
//   };
//
// 設計方針:
//   ・シーン死亡で bundle 廃棄 → 内部 Rc<Asset> が drop → AssetRegistry の refcount
//     が下がり、他参照がなければ実体メモリも解放される (GC 不要 / 決定的解放)。
//   ・path 文字列は呼び出し側が寿命を保証する (string literal / 永続バッファ前提)。
//     ACS 規約により <string> は使わない (const char* を Array に保持)。
//   ・実 AssetRegistry 接続は bridge スケルトンとして TODO 化。Phase G-2 で
//     AssetFuture を統合し、Tick() で counter.Finished() を polling する想定。
//   ・全 API noexcept (例外は使わない / Result も bundle 表層では露出しない)。
#pragma once

#include "foundation/Types.h"
#include "foundation/Log.h"
#include "container/Array.h"

namespace acs::game {

// AssetBundle: シーンスコープのアセット集合非同期ロード + 集約進捗。
//
// ライフサイクル: Add* → BeginLoad → (poll Progress/IsLoaded) → ... → Unload。
// BeginLoad 後の Add は無視する (ロード開始後の集合変更は未定義になるのを避けるため)。
class AssetBundle {
public:
    // bundle 内の各 asset の進捗状態。
    enum class LoadStatus : u8 {
        Pending = 0,  // BeginLoad 前、または BeginLoad 失敗で開始されなかった
        Loading = 1,  // LoadAsync 発行済 / 完了待ち
        Loaded  = 2,  // 完了 (成功)
        Failed  = 3,  // 完了 (失敗)
    };

    AssetBundle() noexcept = default;
    ~AssetBundle() noexcept = default;

    // bundle は Scene にメンバとして埋め込む想定。コピー禁止 (内部 Array 規約)。
    AssetBundle(const AssetBundle&)            = delete;
    AssetBundle& operator=(const AssetBundle&) = delete;

    // bundle に asset パスを追加する。実体ロードはまだ走らない (BeginLoad で開始)。
    // path 文字列は呼び出し側が AssetBundle 寿命中ずっと有効である必要がある
    // (string literal を想定。動的文字列はキャストして渡す側で保持する)。
    // BeginLoad 後の Add は no-op (警告ログのみ)。
    void Add(const char* asset_path) noexcept;

    // 全 path に対して AssetRegistry::LoadAsync を発行し、bundle を Loading 状態に。
    // 多重呼び出しは no-op (idempotent: 2 回目以降は警告のみ)。
    void BeginLoad() noexcept;

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

    // 全 asset を解放する。AssetRegistry に release 通知し、内部 Array を Pending に戻す。
    // シーン破棄前にこれを呼ぶと決定的タイミングで refcount を落とせる
    // (デストラクタに任せても良いが、Scene::OnExit で明示的に呼ぶのが推奨)。
    void Unload() noexcept;

private:
    // 1 個の asset エントリ。
    // path は呼び出し側所有の文字列を借用するだけ (<string> 禁止規約)。
    struct Entry {
        const char* path   = nullptr;
        LoadStatus  status = LoadStatus::Pending;
        // Phase G-2 でここに AssetFuture を追加し、Tick で polling する。
        // 現スケルトンでは status のみで状態遷移を追跡。
    };

    // bundle が BeginLoad を 1 度でも実行したか (Add の閉鎖判定用)。
    bool          _begun = false;
    Array<Entry>  _entries;
};

} // namespace acs::game
