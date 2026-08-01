// SPDX-License-Identifier: Apache-2.0
// CGame — CSceneManager を駆動するゲーム基底クラス
//
// CApplication を継承し、CSceneManager を駆動する基底。利用者は派生クラスで
// InitialScene() を override して最初の AScene を返すだけでよい。
//
// 使い方:
//   class FMyGame : public acs::CGame {
//   protected:
//       acs::TUniquePtr<acs::AScene> InitialScene() noexcept override {
//           return acs::MakeUnique<FTitleScene>();
//       }
//   };
//   ACS_GAME_MAIN(FMyGame)
//
// CSceneManager 駆動 + FRenderContext 配線。固定タイムステップ accumulator +
// AppState 型消去永続状態 + AScene への dt は time_scale 乗算済を渡す。
// OnPause/OnResume は CSceneManager 側で配線済 (Push/Pop 時)。
#pragma once

#include "app/Application.h"
#include "memory/UniquePtr.h"
#include "foundation/Move.h"
#include "render/Font.h"
#include "render/SpriteBatch.h"
#include "gameframework/Forward.h"
#include "gameframework/SceneManager.h"
#include "gameframework/RenderContext.h"
#include "gameframework/AppState.h"
#include "gameframework/FadeTransition.h"
#include "gameframework/SubsystemCollection.h"

namespace acs::game {

/**
 * CApplication を継承し CSceneManager を駆動するゲーム基底クラス。
 *
 * @details
 * 利用者は派生クラスで InitialScene() を override し最初の AScene を返すだけでよい。
 * 固定タイムステップ accumulator、AppState による型消去の永続状態、フェード付き
 * シーン遷移を提供する。AScene に渡す dt は time_scale 乗算済み。
 */
class CGame : public CApplication {
public:
    /** 既定状態で構築する。 */
    CGame() noexcept;

    /** 破棄する。 */
    ~CGame() noexcept override = default;

    /** コピー禁止。 */
    CGame(const CGame&)            = delete;

    /** コピー代入も禁止。 */
    CGame& operator=(const CGame&) = delete;

    /**
     * シーンマネージャへの参照を返す。
     *
     * @return CSceneManager への参照。
     */
    CSceneManager&  Scenes()        noexcept { return m_Scenes; }

    /**
     * レンダーコンテキストへの参照を返す。
     *
     * @return FRenderContext への参照。
     */
    FRenderContext& GetRenderCtx()  noexcept { return m_RenderCtx; }

    /**
     * 時間スケールを設定する。
     *
     * @details AScene::OnUpdate / OnFixedUpdate に渡る dt に乗算される。負値は 0 にクランプ。
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
     * fade-out → (暗転中に) AScene 切替 → fade-in を 1 行で行う。フェードは
     * time_scale の影響を受けない実時間で進む (ポーズ中でも遷移は進む)。遷移演出は
     * CGame が描画するので、切替先 AScene 側で重ねてフェードしないこと。
     * @param next 遷移先の AScene (所有権が移る)。
     * @param out_sec fade-out の秒数。
     * @param in_sec fade-in の秒数。
     */
    void TransitionTo(TUniquePtr<AScene> next, f32 out_sec = 0.3f, f32 in_sec = 0.3f) noexcept;

    /**
     * 進行中のフェード状態への参照を返す。
     *
     * @return overlay alpha/color・phase を参照できる CFadeTransition への参照。
     */
    CFadeTransition& Fade() noexcept { return m_Fade; }

    /**
     * GameInstance スコープのサブシステム束を返す(Engine スコープへフォールバックする)。
     *
     * @details AScene の World サブシステム束はこれを parent にする。
     * @return GameInstance スコープのコレクション。
     */
    CSubsystemCollection& GameInstanceSubsystems() noexcept { return m_GameInstanceSubsystems; }

    /**
     * Engine スコープ(アプリ全体寿命)のサブシステム束を返す。
     *
     * @return Engine スコープのコレクション。
     */
    CSubsystemCollection& EngineSubsystems() noexcept { return CApplication::EngineSubsystems(); }

    /**
     * 型でサブシステムを取得する(GameInstance → Engine の順に検索)。
     *
     * @tparam T ASubsystem 派生型。
     * @return T*(未登録なら nullptr)。
     */
    template<typename T>
    T* GetSubsystem() noexcept { return m_GameInstanceSubsystems.Get<T>(); }

protected:
    /**
     * 最初に push される AScene を返す (派生クラスで実装必須)。
     *
     * @return 起動時に push する初期 AScene。
     */
    virtual TUniquePtr<AScene> InitialScene() noexcept = 0;

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
     * イベントフック。受け取ったイベントを CSceneManager に流す。
     *
     * @details 派生がさらに override する場合は基底を呼ぶこと。
     * @param e ディスパッチするイベント。
     */
    void OnEvent(const FEvent& e) noexcept override;

private:
    /** 初回 OnRender で default UI フォントを遅延ロードする。 */
    void EnsureUiFont() noexcept;

    /** フェード overlay 用 FSpriteBatch を遅延 init する。 */
    void EnsureOverlay() noexcept;

    /** 進行中フェードの fullscreen quad を描く。 */
    void DrawFadeOverlay() noexcept;

    /** GameInstance スコープ(ゲームセッション寿命、シーン跨ぎ)のサブシステム束。 */
    CSubsystemCollection m_GameInstanceSubsystems;

    /** シーンマネージャ (AScene の push/pop/切替を管理)。 */
    CSceneManager  m_Scenes;

    /** AScene 描画に渡すレンダーコンテキスト。 */
    FRenderContext m_RenderCtx;

    /** シーン跨ぎの型消去永続状態 (1 個固定)。 */
    FAppStateSlot  m_AppState;

    /** GameFramework同梱factoryの登録が全件成功した。 */
    bool m_BuiltinCatalogReady = false;

    /** 全シーン共有の HUD フォント (game 寿命)。 */
    FFont          m_UiFont;

    /** UI フォントのロードに成功したか。 */
    bool          m_UiFontReady = false;

    /** UI フォントのロードを試行済みか (再試行抑止)。 */
    bool          m_UiFontTried = false;

    /** シーン遷移フェードの状態。 */
    CFadeTransition   m_Fade;

    /** 暗転中に差し替える次 AScene。 */
    TUniquePtr<AScene> m_PendingScene;

    /** フェード overlay 描画用の FSpriteBatch。 */
    FSpriteBatch      m_Overlay;

    /** overlay FSpriteBatch の init に成功したか。 */
    bool              m_OverlayReady = false;

    /** overlay FSpriteBatch の init を試行済みか (再試行抑止)。 */
    bool              m_OverlayTried = false;

    /** 時間スケール (AScene の dt に乗算)。 */
    f32           m_TimeScale       = 1.0f;

    /** 固定 step の長さ (秒、0 以下で無効)。 */
    f32           m_FixedDt         = 1.0f / 60.0f;

    /** 固定タイムステップの蓄積秒。 */
    f32           m_FixedAccum      = 0.0f;

    /** 1 フレームで進める最大固定 step 数。 */
    u32           m_MaxFixedSteps  = 8;
};

} // namespace acs::game
