// SPDX-License-Identifier: Apache-2.0
// CGame — CSceneManager を駆動するゲーム基底クラス
//
// CApplication を継承し、CSceneManager を駆動する基底。利用者は派生クラスで
// InitialScene() を override して最初の AScene を返すだけでよい。
//
// 使い方:
//   class CMyGame : public acs::CGame {
//   protected:
//       acs::TUniquePtr<acs::AScene> InitialScene() noexcept override {
//           return acs::MakeUnique<ATitleScene>();
//       }
//   };
//   ACS_GAME_MAIN(CMyGame)
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
#include "gameframework/SceneRenderResources.h"
#include "gameframework/RenderContext.h"
#include "gameframework/AppState.h"
#include "gameframework/FadeTransition.h"
#include "gameframework/FixedStepRuntimeSnapshot.h"
#include "gameframework/SubsystemCollection.h"
#include "timing/FixedStepClock.h"

namespace acs::game {

class IFixedTickInputSource;
class IInputFrameSource;

/**
 * CApplication を継承し CSceneManager を駆動するゲーム基底クラス。
 *
 * @details
 * 利用者は派生クラスで InitialScene() を override し最初の AScene を返すだけでよい。
 * 固定ステップ時計、AppState による型消去の永続状態、フェード付き
 * シーン遷移を提供する。AScene に渡す dt は time_scale 乗算済み。
 */
class CGame : public CApplication {
public:
    /** 固定step runtime snapshot用のprocess内owner tokenを割り当てて構築する。 */
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
    void SetFixedTimestep(f32 fixed_dt, u32 max_steps_per_frame = 8) noexcept;

    /** 固定タイムステップの完全な設定を検証して適用する。 */
    bool TrySetFixedTimestep(const timing::FFixedStepOptions& options) noexcept;

    /** 固定タイムステップ更新を無効化し、時計の累積状態を初期化する。 */
    void DisableFixedTimestep() noexcept;

    /** 固定タイムステップ更新が有効ならtrueを返す。 */
    bool IsFixedTimestepEnabled() const noexcept
    {
        return m_FixedStepEnabled;
    }

    /**
     * 固定タイムステップの長さを返す。
     *
     * @return 固定 step の長さ (秒)。
     */
    f32 FixedTimestep() const noexcept
    {
        return m_FixedStepEnabled ? static_cast<f32>(m_FixedStepClock.Options().step_seconds) : 0.0f;
    }

    /** 次の固定stepまでの描画補間率を返す。 */
    f64 FixedStepInterpolationAlpha() const noexcept
    {
        return m_FixedStepEnabled ? m_FixedStepClock.InterpolationAlpha() : 0.0;
    }

    /** AIやheadless testが所有する描画フレーム入力ソースへ切り替える。 */
    void SetFixedStepInputSource(IInputFrameSource& source) noexcept;

    /** replayやrollbackが所有する固定tick入力ソースへ切り替える。 */
    void SetFixedTickInputSource(IFixedTickInputSource& source) noexcept;

    /** platform入力を使う既定状態へ戻し、未消費入力を破棄する。 */
    void ResetFixedStepInputSource() noexcept;

    /** platform入力を直接取得する既定状態ならtrueを返す。 */
    bool UsesPlatformFixedStepInput() const noexcept
    {
        return m_FixedStepInputSource == nullptr && m_FixedTickInputSource == nullptr;
    }

    /** 固定tickごとの決定論入力ソースを使っている場合はtrueを返す。 */
    bool UsesFixedTickInputSource() const noexcept
    {
        return m_FixedTickInputSource != nullptr;
    }

    /** 固定更新時計の再現可能な状態を取得する。 */
    bool TryCaptureFixedStepSnapshot(timing::FFixedStepClockSnapshot& snapshot) const noexcept;

    /** 固定更新時計を保存状態へ復元し、成功時だけ固定更新を有効化する。 */
    bool TryRestoreFixedStepSnapshot(const timing::FFixedStepClockSnapshot& snapshot) noexcept;

    /** 固定時計とactive sceneの未消費入力を同じ保存値へ複製する。 */
    bool TryCaptureFixedStepRuntimeSnapshot(FFixedStepRuntimeSnapshot& snapshot) const noexcept;

    /** 同じgame、scene、入力sourceで取得した固定runtime状態を一括復元する。 */
    bool TryRestoreFixedStepRuntimeSnapshot(const FFixedStepRuntimeSnapshot& snapshot) noexcept;

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
     * travel context を添えてフェード付きのシーン遷移を開始する。
     *
     * @details context は暗転中の実際の切替時に next へ引き渡される
     * (`AScene::TravelContext<T>()` で読む)。
     * @param next 遷移先の AScene (所有権が移る)。
     * @param context 次のシーンへ持っていく任意データ (所有権が移る。nullptr 可)。
     * @param out_sec fade-out の秒数。
     * @param in_sec fade-in の秒数。
     */
    void TransitionTo(TUniquePtr<AScene> next,
                      TUniquePtr<CSceneTravelContext> context,
                      f32 out_sec = 0.3f, f32 in_sec = 0.3f) noexcept;

    /**
     * 進行中のフェード状態への参照を返す。
     *
     * @return overlay alpha/color・phase を参照できる CFadeTransition への参照。
     */
    CFadeTransition& Fade() noexcept { return m_Fade; }

    /**
     * シーン描画に使う CSpriteBatch と オフスクリーン RT の共有束を返す。
     *
     * @details
     * シーンが所有するのは ANode ツリーだけで、描画リソースは game 寿命で 1 組を共有する
     * (docs/SceneUnification.md)。中の CSpriteBatch と RT は各 Ensure* を呼ぶまで
     * 確保しないため、2D 描画を使わない game は GPU リソースを 1 つも作らない。
     * @return 共有する CSceneRenderResources への参照。
     */
    CSceneRenderResources& SceneRenderResources() noexcept { return m_SceneRenderResources; }

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
    /** 入力source結線の世代を進め、使い切った場合はsnapshot取得不能な0へ移す。 */
    void AdvanceFixedInputSourceEpoch_Internal() noexcept;

    /** 初回 OnRender で default UI フォントを遅延ロードする。 */
    void EnsureUiFont() noexcept;

    /** フェード overlay 用 CSpriteBatch を遅延 init する。 */
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

    /** 暗転中の切替で m_PendingScene へ引き渡す travel context。 */
    TUniquePtr<CSceneTravelContext> m_PendingSceneContext;

    /** フェード overlay 描画用の CSpriteBatch。 */
    CSpriteBatch      m_Overlay;

    /** overlay CSpriteBatch の init に成功したか。 */
    bool              m_OverlayReady = false;

    /** overlay CSpriteBatch の init を試行済みか (再試行抑止)。 */
    bool              m_OverlayTried = false;

    /** シーンが借りて使う描画リソース束 (中の GPU リソースは Ensure* まで確保しない)。 */
    CSceneRenderResources m_SceneRenderResources;

    /** 時間スケール (AScene の dt に乗算)。 */
    f32           m_TimeScale       = 1.0f;

    /** 可変deltaを有界な固定更新回数へ変換する時計。 */
    timing::CFixedStepClock m_FixedStepClock;

    /** 固定タイムステップ更新が有効か。 */
    bool m_FixedStepEnabled = true;

    /** AI、testが所有する描画フレーム入力ソース。 */
    IInputFrameSource* m_FixedStepInputSource = nullptr;

    /** replay、rollbackが所有する固定tick入力ソース。 */
    IFixedTickInputSource* m_FixedTickInputSource = nullptr;

    /** このCGameだけへruntime snapshotを復元するためのprocess内識別token。 */
    u64 m_FixedStepRuntimeOwnerToken = 0u;

    /** frame/tick入力sourceの実効的な切替ごとに進む世代。 */
    u64 m_FixedInputSourceEpoch = 1u;
};

} // namespace acs::game
