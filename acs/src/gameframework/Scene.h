// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — Scene 基底
//
// 1 つの画面/状態を 1 つの Scene サブクラスで書く。FGame がスタック上で
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
// 本ヘッダは lifecycle hook + FGame/FSceneManager 参照を提供する。
#pragma once

#include "foundation/Types.h"
#include "foundation/Assert.h"
#include "foundation/Move.h"
#include "memory/UniquePtr.h"
#include "gameframework/SceneServices.h"
#include "gameframework/SubsystemCollection.h"

namespace acs {

struct Event;

namespace game {

class FGame;
class FSceneManager;
class RenderContext;

/**
 * 1 つの画面/状態を表すシーンの基底クラス。
 *
 * @details
 * 派生クラスが OnEnter/OnUpdate/OnRender/OnExit 等の lifecycle hook を override して
 * ロジックと描画を書く。FGame がスタック上で切り替え・更新・描画する。コピー禁止で、
 * FGame/FSceneManager が _SetContext / _AttachServices で実行コンテキストを配線する。
 * シーン遷移は OnUpdate 内で Scenes().ChangeScene() を呼ぶと次フレーム頭で適用される。
 */
class Scene {
public:
    /** 空のシーンを構築する (コンテキスト・サービスは未配線)。 */
    Scene() noexcept = default;

    /** 派生クラスを正しく破棄するための仮想デストラクタ。 */
    virtual ~Scene() noexcept = default;

    /** コピー禁止 (シーンは単独所有・参照が前提のため)。 */
    Scene(const Scene&)            = delete;

    /** コピー代入も禁止。 */
    Scene& operator=(const Scene&) = delete;

    /**
     * シーンがスタックの top に来た直後に 1 度だけ呼ばれる初期化フック。
     *
     * @details FGame が新規 push したときと、上のシーンが pop されて復帰したときの
     * 両方で呼ばれる。アセット読み込みなどはここで行う。
     */
    virtual void OnEnter() noexcept {}

    /**
     * シーンが top から退場する直前に呼ばれる後始末フック (Change/Pop 両方)。
     *
     * @details GPU リソース解放はここで明示的に行ってもよいが、デストラクタに任せてもよい。
     */
    virtual void OnExit() noexcept {}

    /**
     * 別シーンが上に Push されて自分が更新も描画も止まるときに呼ばれるフック。
     *
     * @details BGM ダッキングなどに使う。
     */
    virtual void OnPause() noexcept {}

    /** 上のシーンが Pop されて自分が top に戻った直後に呼ばれる復帰フック。 */
    virtual void OnResume() noexcept {}

    /**
     * 毎フレーム 1 回呼ばれる update フック。
     *
     * @param dt スケール後の経過秒 (FGame::SetTimeScale 反映済)。
     */
    virtual void OnUpdate(f32 /*dt*/) noexcept {}

    /**
     * 固定タイムステップ update フック (物理など固定刻みの更新に使う)。
     *
     * @param fixed_dt 固定刻みの秒。
     */
    virtual void OnFixedUpdate(f32 /*fixed_dt*/) noexcept {}

    /**
     * 描画フック。
     *
     * @details RenderContext は Scene 全体で共有される FSpriteBatch / Font /
     * 現フレームの IRhiCommandList* を持つ。
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     */
    virtual void OnRender(RenderContext& /*rc*/) noexcept {}

    /**
     * ウィンドウ/入力イベントの配送フック (最上段シーンにのみ届く)。
     *
     * @param e 配送されたイベント。
     */
    virtual void OnEvent(const Event& /*e*/) noexcept {}

    /**
     * このシーンが使いたいサービスを bit flag で宣言する。
     *
     * @details 派生クラスで override する。既定は None (= サービス無し、scenes が
     * 直接メンバーで service を持つ古いパターン互換)。
     * @return 使用するサービスのビットフラグ (既定 ESvc::None)。
     */
    virtual ESvc WantedServices() const noexcept { return ESvc::None; }

    /**
     * FSceneManager が attach 済みの FSceneServices を返す。
     *
     * @details WantedServices が None で attach されていない場合は ACS_ASSERT で停止する
     * (= 使う気がないなら呼ばない)。
     * @return attach 済みの FSceneServices への参照。
     */
    FSceneServices& Services() const noexcept {
        ACS_ASSERTF(m_Services.Get() != nullptr,
                    "Scene::Services() called but WantedServices() returned None (or attach failed)");
        return *m_Services;
    }

    /**
     * FSceneServices が attach されているかを返す。
     *
     * @return attach 済みなら true。
     */
    bool HasServices() const noexcept { return m_Services.Get() != nullptr; }

    /**
     * 所属する FGame を返す。
     *
     * @return FGame への参照。
     */
    FGame&         GetGame() const noexcept { return *m_Game; }

    /**
     * シーンスタックを管理する FSceneManager を返す。
     *
     * @return FSceneManager への参照 (ChangeScene 等の遷移要求に使う)。
     */
    FSceneManager& Scenes()  const noexcept { return *m_Scenes; }

    /**
     * FGame/FSceneManager の参照を配線する (内部用。利用者は触らない)。
     *
     * @param g 所属する FGame。
     * @param sm シーンスタックを管理する FSceneManager。
     */
    void _SetContext(FGame* g, FSceneManager* sm) noexcept {
        m_Game   = g;
        m_Scenes = sm;
    }

    /**
     * 生成済みの FSceneServices を attach する (内部用)。
     *
     * @param svc attach する FSceneServices (所有権が移る)。
     */
    void _AttachServices(TUniquePtr<FSceneServices> svc) noexcept {
        m_Services = Move(svc);
    }

    /**
     * attach 済みの FSceneServices をポインタで返す (内部用)。
     *
     * @return FSceneServices へのポインタ (未 attach なら nullptr)。
     */
    FSceneServices* _ServicesOrNull() const noexcept { return m_Services.Get(); }

    // ===== サブシステム (World スコープ) =====

    /**
     * このシーン(World スコープ)のサブシステム束を返す。
     *
     * @details World に無いサブシステムは GameInstance → Engine へフォールバックする
     * (FSceneManager が parent を配線する)。
     * @return World スコープのコレクション。
     */
    FSubsystemCollection& Subsystems() noexcept { return m_WorldSubsystems; }

    /**
     * 型でサブシステムを取得する (World → GameInstance → Engine の順に検索)。
     *
     * @tparam T FSubsystem 派生型。
     * @return T*(未登録なら nullptr)。
     */
    template<typename T>
    T* GetSubsystem() noexcept { return m_WorldSubsystems.Get<T>(); }

    /**
     * World サブシステムを初期化する (内部用。FSceneManager が push 時に呼ぶ)。
     *
     * @param parent GameInstance スコープのコレクション(フォールバック先)。
     */
    void _InitWorldSubsystems(FSubsystemCollection* parent) noexcept {
        m_WorldSubsystems.Initialize(ESubsystemScope::World, parent);
        _OnWorldSubsystemsReady();
    }

    /** World サブシステムを 1 フレーム進める (内部用)。 */
    void _TickWorldSubsystems(f32 dt) noexcept { m_WorldSubsystems.Tick(dt); }

    /** World サブシステムを解体する (内部用。FSceneManager が pop 時に呼ぶ)。 */
    void _DeinitWorldSubsystems() noexcept { m_WorldSubsystems.Deinitialize(); }

    /** World サブシステムへのポインタ (内部用。派生がノードへ配線するのに使う)。 */
    FSubsystemCollection* _WorldSubsystemsPtr() noexcept { return &m_WorldSubsystems; }

protected:
    /**
     * World サブシステムの初期化直後に呼ばれる内部フック(OnEnter より前)。
     *
     * @details FScene2D が override してルートノードへサブシステム束を配線し、
     * 配下のノード/コンポーネントから GetSubsystem<T>() を使えるようにする。
     */
    virtual void _OnWorldSubsystemsReady() noexcept {}

private:
    /** 所属する FGame (default = nullptr、_SetContext で配線)。 */
    FGame*                    m_Game     = nullptr;

    /** シーンスタックを管理する FSceneManager (default = nullptr、_SetContext で配線)。 */
    FSceneManager*            m_Scenes   = nullptr;

    /** attach されたサービス束 (WantedServices に応じて FSceneManager が確保、所有権を持つ)。 */
    TUniquePtr<FSceneServices> m_Services;

    /** World スコープのサブシステム束 (push 時に FSceneManager が Initialize)。 */
    FSubsystemCollection m_WorldSubsystems;
};

} // namespace game
} // namespace acs
