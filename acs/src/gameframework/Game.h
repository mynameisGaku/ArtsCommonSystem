// SPDX-License-Identifier: Apache-2.0
// FGame — FSceneManager を駆動するゲーム基底クラス
//
// FApplication を継承し、FSceneManager を駆動する基底。利用者は派生クラスで
// InitialScene() を override して最初の Scene を返すだけでよい。
//
// 使い方:
//   class MyGame : public acs::game::FGame {
//   protected:
//       acs::TUniquePtr<acs::game::Scene> InitialScene() noexcept override {
//           return acs::MakeUnique<TitleScene>();
//       }
//   };
//   ACS_GAME_MAIN(MyGame)
//
// FSceneManager 駆動 + RenderContext 配線。固定タイムステップ accumulator +
// AppState 型消去永続状態 + Scene への dt は time_scale 乗算済を渡す。
// OnPause/OnResume は FSceneManager 側で配線済 (Push/Pop 時)。
#pragma once

#include "app/Application.h"
#include "memory/UniquePtr.h"
#include "foundation/Move.h"
#include "render/Font.h"
#include "render/SpriteBatch.h"
#include "gameframework/SceneManager.h"
#include "gameframework/RenderContext.h"
#include "gameframework/AppState.h"
#include "gameframework/FadeTransition.h"

namespace acs::game {

class Scene;

/**
 * FApplication を継承し FSceneManager を駆動するゲーム基底クラス。
 *
 * @details
 * 利用者は派生クラスで InitialScene() を override し最初の Scene を返すだけでよい。
 * 固定タイムステップ accumulator、AppState による型消去の永続状態、フェード付き
 * シーン遷移を提供する。Scene に渡す dt は time_scale 乗算済み。
 */
class FGame : public FApplication {
public:
    /** 既定状態で構築する。 */
    FGame() noexcept = default;

    /** 破棄する。 */
    ~FGame() noexcept override = default;

    /** コピー禁止。 */
    FGame(const FGame&)            = delete;

    /** コピー代入も禁止。 */
    FGame& operator=(const FGame&) = delete;

    /**
     * シーンマネージャへの参照を返す。
     *
     * @return FSceneManager への参照。
     */
    FSceneManager&  Scenes()        noexcept { return m_Scenes; }

    /**
     * レンダーコンテキストへの参照を返す。
     *
     * @return RenderContext への参照。
     */
    RenderContext& GetRenderCtx()  noexcept { return m_RenderCtx; }

    /**
     * 時間スケールを設定する。
     *
     * @details Scene::OnUpdate / OnFixedUpdate に渡る dt に乗算される。負値は 0 にクランプ。
     * @param s 新しい時間スケール。
     */
    void SetTimeScale(f32 s) noexcept { m_TimeScale = s < 0.0f ? 0.0f : s; }

    /**
     * 現在の時間スケールを返す。
     *
     * @return 設定済みの時間スケール。
     */
    f32  TimeScale() const noexcept { return m_TimeScale; }

    /**
     * 固定タイムステップを設定する。
     *
     * @details
     * 既定は fixed_dt=1/60, max=8 (= 0.133s ぶんまでキャッチアップ、それ以上は遅延吸収)。
     * @param fixed_dt 固定 step の長さ (秒、典型 1/60 = 0.01667)。0 以下で無効化。
     * @param max_steps_per_frame 1 フレームで進める最大 step 数 (暴走防止クランプ)。
     */
    void SetFixedTimestep(f32 fixed_dt, u32 max_steps_per_frame = 8) noexcept {
        m_FixedDt = fixed_dt;
        m_MaxFixedSteps = max_steps_per_frame;
    }

    /**
     * 固定タイムステップの長さを返す。
     *
     * @return 固定 step の長さ (秒)。
     */
    f32 FixedTimestep() const noexcept { return m_FixedDt; }

    /**
     * シーン跨ぎの永続状態 (AppState) を構築する。
     *
     * @details 型消去で 1 個だけ保持する。詳細は AppState.h。
     * @tparam T 構築する AppState 型。
     * @tparam Args T のコンストラクタ引数型。
     * @param args T のコンストラクタへ転送する引数。
     * @return 構築した T への参照。
     */
    template<typename T, typename... Args>
    T& EmplaceAppState(Args&&... args) noexcept {
        return m_AppState.Emplace<T>(Forward<Args>(args)...);
    }

    /**
     * シーン跨ぎの永続状態 (AppState) を取り出す。
     *
     * @tparam T 取り出す AppState 型。
     * @return T へのポインタ (未設定 / 型不一致なら nullptr)。
     */
    template<typename T>
    T* AppState() noexcept { return m_AppState.Get<T>(); }

    /**
     * フェード付きシーン遷移を行う。
     *
     * @details
     * fade-out → (暗転中に) Scene 切替 → fade-in を 1 行で行う。フェードは
     * time_scale の影響を受けない実時間で進む (ポーズ中でも遷移は進む)。遷移演出は
     * FGame が描画するので、切替先 Scene 側で重ねてフェードしないこと。
     * @param next 遷移先の Scene (所有権が移る)。
     * @param out_sec fade-out の秒数。
     * @param in_sec fade-in の秒数。
     */
    void TransitionTo(TUniquePtr<Scene> next, f32 out_sec = 0.3f, f32 in_sec = 0.3f) noexcept;

    /**
     * 進行中のフェード状態への参照を返す。
     *
     * @return overlay alpha/color・phase を参照できる FFadeTransition への参照。
     */
    FFadeTransition& Fade() noexcept { return m_Fade; }

protected:
    /**
     * 最初に push される Scene を返す (派生クラスで実装必須)。
     *
     * @return 起動時に push する初期 Scene。
     */
    virtual TUniquePtr<Scene> InitialScene() noexcept = 0;

    /**
     * 起動時フック。InitialScene() を push して即時適用する。
     *
     * @details 派生がさらに override する場合は基底を呼ぶこと。
     */
    void OnStart()    noexcept override;

    /**
     * 毎フレーム update フック。フェード進行と固定 / 可変 update を駆動する。
     *
     * @details 派生がさらに override する場合は基底を呼ぶこと。
     * @param dt 前フレームからの経過秒。
     */
    void OnUpdate(f32 dt) noexcept override;

    /**
     * 描画フック。シーン描画の上にフェード幕を重ねる。
     *
     * @details 派生がさらに override する場合は基底を呼ぶこと。
     */
    void OnRender()   noexcept override;

    /**
     * 終了フック。全シーンとフォント・overlay リソースを解放する。
     *
     * @details 派生がさらに override する場合は基底を呼ぶこと。
     */
    void OnShutdown() noexcept override;

    /**
     * イベントフック。受け取ったイベントを FSceneManager に流す。
     *
     * @details 派生がさらに override する場合は基底を呼ぶこと。
     * @param e ディスパッチするイベント。
     */
    void OnEvent(const Event& e) noexcept override;

private:
    /** 初回 OnRender で default UI フォントを遅延ロードする。 */
    void EnsureUiFont() noexcept;

    /** フェード overlay 用 SpriteBatch を遅延 init する。 */
    void EnsureOverlay() noexcept;

    /** 進行中フェードの fullscreen quad を描く。 */
    void DrawFadeOverlay() noexcept;

    /** シーンマネージャ (Scene の push/pop/切替を管理)。 */
    FSceneManager  m_Scenes;

    /** Scene 描画に渡すレンダーコンテキスト。 */
    RenderContext m_RenderCtx;

    /** シーン跨ぎの型消去永続状態 (1 個固定)。 */
    FAppStateSlot  m_AppState;

    /** 全シーン共有の HUD フォント (game 寿命)。 */
    Font          m_UiFont;

    /** UI フォントのロードに成功したか。 */
    bool          m_UiFontReady = false;

    /** UI フォントのロードを試行済みか (再試行抑止)。 */
    bool          m_UiFontTried = false;

    /** シーン遷移フェードの状態。 */
    FFadeTransition   m_Fade;

    /** 暗転中に差し替える次 Scene。 */
    TUniquePtr<Scene> m_PendingScene;

    /** フェード overlay 描画用の SpriteBatch。 */
    FSpriteBatch      m_Overlay;

    /** overlay SpriteBatch の init に成功したか。 */
    bool              m_OverlayReady = false;

    /** overlay SpriteBatch の init を試行済みか (再試行抑止)。 */
    bool              m_OverlayTried = false;

    /** 時間スケール (Scene の dt に乗算)。 */
    f32           m_TimeScale       = 1.0f;

    /** 固定 step の長さ (秒、0 以下で無効)。 */
    f32           m_FixedDt         = 1.0f / 60.0f;

    /** 固定タイムステップの蓄積秒。 */
    f32           m_FixedAccum      = 0.0f;

    /** 1 フレームで進める最大固定 step 数。 */
    u32           m_MaxFixedSteps  = 8;
};

} // namespace acs::game
