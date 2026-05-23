// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — Scene 基底 (Phase 1 着手)
//
// 1 つの画面/状態を 1 つの Scene サブクラスで書く。Game がスタック上で
// 切り替え・更新・描画する。Scene の override は全て `noexcept`。
//
// 使い方:
//   class TitleScene : public acs::game::Scene {
//   public:
//       void OnEnter()      noexcept override { /* 起動時の初期化 */ }
//       void OnUpdate(f32)  noexcept override { /* ロジック */ }
//       void OnRender(acs::game::RenderContext&) noexcept override { /* 描画 */ }
//       void OnExit()       noexcept override { /* 後片付け */ }
//   };
//
// 遷移: OnUpdate 内で `Scenes().ChangeScene(MakeUnique<NextScene>())` を呼ぶと
// **次フレーム頭**で適用される (走査中の構造変更を避ける、1 フレーム 1 遷移)。
//
// v3 設計書: docs/GameFramework.md §3「Pillar A」。本ヘッダは Phase 1 の
// 最小骨格 = lifecycle hook + Game/SceneManager 参照のみ。SceneServices /
// AppState / GPU 遅延削除 / フェード遷移は次フェーズで肉付け。
#pragma once

#include "foundation/Types.h"
#include "foundation/Assert.h"
#include "foundation/Move.h"
#include "memory/UniquePtr.h"
#include "gameframework/SceneServices.h"

namespace acs {

struct Event;

namespace game {

class Game;
class SceneManager;
class RenderContext;

class Scene {
public:
    Scene() noexcept = default;
    virtual ~Scene() noexcept = default;

    Scene(const Scene&)            = delete;
    Scene& operator=(const Scene&) = delete;

    // ----- Lifecycle hooks (override what you need。全て noexcept 必須) -----

    // シーンがスタックの top に来た直後に 1 度だけ呼ばれる (Game が新規 push
    // または既存 pop で復帰したときの両方)。アセット読み込みなどはここで。
    virtual void OnEnter() noexcept {}

    // シーンが top から退場する直前に呼ばれる (Change/Pop 両方)。GPU リソース
    // 解放はここで明示的にやってもよいが、デストラクタに任せてよい。
    virtual void OnExit() noexcept {}

    // 別シーンが上に Push された (= 自分は更新も描画も止まる) ときに呼ばれる。
    // BGM ダッキングなどに使う。Phase 1 では呼び出し未実装、Phase 2 で。
    virtual void OnPause() noexcept {}

    // 上のシーンが Pop されて自分が top に戻った直後に呼ばれる。Phase 1 未実装。
    virtual void OnResume() noexcept {}

    // 毎フレーム 1 回呼ばれる。dt はスケール後 (Game::SetTimeScale が
    // 反映済)、単位は秒。Phase 1 では fixed update / accumulator 無し。
    virtual void OnUpdate(f32 /*dt*/) noexcept {}

    // 固定タイムステップ更新。Phase 1 では未呼び出し (Game がアキュムレータを
    // 持つ Phase 2 から)。物理など固定刻みの更新を入れたい場合のフック。
    virtual void OnFixedUpdate(f32 /*fixed_dt*/) noexcept {}

    // 描画。RenderContext は Scene 全体で共有される SpriteBatch / Font /
    // 現フレームの IRhiCommandList* を持つ。
    virtual void OnRender(RenderContext& /*rc*/) noexcept {}

    // ウィンドウ/入力イベントの配送。最上段シーンにのみ届く (v3 設計)。
    virtual void OnEvent(const Event& /*e*/) noexcept {}

    // ----- Services (Phase 8、Pillar A v3 §3.1) -----
    // 派生クラスで override し、使うサービスを bit flag で宣言。default は None
    // (= サービス無し、scenes が直接メンバーで service 持つ古いパターン互換)。
    virtual ESvc WantedServices() const noexcept { return ESvc::None; }

    // SceneManager が attach 済の SceneServices。WantedServices が None で
    // attach されていない場合は ACS_ASSERT で停止 (= 使う気がないなら呼ばない)。
    SceneServices& Services() const noexcept {
        ACS_ASSERTF(_services.Get() != nullptr,
                    "Scene::Services() called but WantedServices() returned None (or attach failed)");
        return *_services;
    }
    bool HasServices() const noexcept { return _services.Get() != nullptr; }

    // ----- Context accessors (Game が _SetContext で配線) -----
    Game&         GetGame() const noexcept { return *_game; }
    SceneManager& Scenes()  const noexcept { return *_scenes; }

    // Game/SceneManager が attach する際に呼ぶ。利用者は触らない。
    void _SetContext(Game* g, SceneManager* sm) noexcept {
        _game   = g;
        _scenes = sm;
    }
    void _AttachServices(UniquePtr<SceneServices> svc) noexcept {
        _services = Move(svc);
    }
    SceneServices* _ServicesOrNull() const noexcept { return _services.Get(); }

private:
    Game*                    _game     = nullptr;
    SceneManager*            _scenes   = nullptr;
    UniquePtr<SceneServices> _services;
};

} // namespace game
} // namespace acs
