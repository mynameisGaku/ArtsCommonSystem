// SPDX-License-Identifier: Apache-2.0
// FApplication 基底（継承して OnStart / OnUpdate / OnRender / OnShutdown を実装）
//
// 使い方:
//   class MyGame : public FApplication {
//   public:
//       // フックは必ず noexcept override で宣言する
//       // （基底のフックが noexcept のため。noexcept を省くとコンパイルエラー）
//       void OnStart() noexcept override {
//           ACS_LOG_INFO("ゲーム開始");
//       }
//       void OnUpdate(f32 dt) noexcept override {
//           if (Input::IsKeyPressed(EKey::Escape)) Quit();
//       }
//       void OnRender() noexcept override {
//           // 描画コマンド (BeginFrame / EndFrame は基底が呼ぶ)
//       }
//   };
//
//   ACS_DEFINE_MAIN(MyGame)   // エントリポイントを自動生成 (app/EntryPoint.h)
#pragma once

#include "foundation/Types.h"
#include "platform/Window.h"
#include "platform/Time.h"
#include "ecs/World.h"
#include "asset/AssetRegistry.h"
#include "render/Renderer.h"
#include "event/Timer.h"
#include "event/MessageBroker.h"
#include "app/AppConfig.h"

namespace acs {

/**
 * ゲーム/サンプルが継承するアプリケーション基底クラス。
 *
 * @details
 * ウィンドウ・レンダラ・ECS World・アセット・タイマー・イベントブローカーといった
 * エンジンサブシステムを所有し、Run() で初期化からメインループ・後始末までを駆動する。
 * 派生クラスは OnStart/OnUpdate/OnRender/OnShutdown/OnEvent を override してロジックと
 * 描画を書く。non-copy 型で、通常は ACS_DEFINE_MAIN マクロ経由でインスタンス化する。
 */
class FApplication {
public:
    /** 既定状態で構築する (サブシステムは Run で初期化)。 */
    FApplication() noexcept = default;

    /** 派生クラスを正しく破棄するための仮想デストラクタ。 */
    virtual ~FApplication() noexcept = default;

    /** コピー禁止 (エンジンサブシステムを単独所有するため)。 */
    FApplication(const FApplication&) = delete;

    /** コピー代入も禁止。 */
    FApplication& operator=(const FApplication&) = delete;

    /**
     * サブシステムを初期化し、メインループを最後まで回す。
     *
     * @details
     * ロガー→メモリ→スレッドプール→ウィンドウ→レンダラの順に立ち上げ、OnStart を呼び、
     * Quit() か OnClose まで毎フレーム update/描画を回す。終了時は逆順に後始末する。
     * @param cfg ウィンドウ・ロガー・レンダラ等の起動オプション。
     * @return 正常終了で 0、初期化失敗で非 0 のエラーコード。
     */
    int Run(const FAppConfig& cfg) noexcept;

    /** メインループを抜けて終了するよう要求する (OnShutdown が呼ばれる)。 */
    void Quit() noexcept { m_bRunning = false; }

    /**
     * 背景クリア色を動的に変更する (次フレームの描画から反映)。
     *
     * @details 既定値は起動時 FAppConfig の clear_r/g/b/a。
     * @param r 赤成分 (0..1)。
     * @param g 緑成分 (0..1)。
     * @param b 青成分 (0..1)。
     * @param a アルファ成分 (0..1、既定 1.0)。
     */
    void SetClearColor(f32 r, f32 g, f32 b, f32 a = 1.0f) noexcept {
        m_ClearColor = ClearColor{ r, g, b, a };
    }

    /**
     * 現在の背景クリア色を返す。
     *
     * @details マルチパス描画で RT 再バインド時のクリアに使う。
     * @return 現在のクリア色への const 参照。
     */
    const ClearColor& GetClearColor() const noexcept { return m_ClearColor; }

    /**
     * ウィンドウへの参照を返す。
     *
     * @return アプリが所有する FWindow への参照。
     */
    FWindow&         GetWindow()        noexcept { return m_Window; }

    /**
     * レンダラへの参照を返す。
     *
     * @return アプリが所有する FRenderer への参照。
     */
    FRenderer&       GetRenderer()      noexcept { return m_Renderer; }

    /**
     * ECS World への参照を返す。
     *
     * @return アプリが所有する World への参照。
     */
    World&          GetWorld()         noexcept { return m_World; }

    /**
     * アセットレジストリへの参照を返す。
     *
     * @return アプリが所有する FAssetRegistry への参照。
     */
    FAssetRegistry&  GetAssets()        noexcept { return m_Assets; }

    /**
     * タイマーマネージャへの参照を返す。
     *
     * @return アプリが所有する FTimerManager への参照。
     */
    FTimerManager&   GetTimers()        noexcept { return m_Timers; }

    /**
     * イベントブローカーへの参照を返す。
     *
     * @return アプリが所有する MessageBroker への参照。
     */
    MessageBroker&  GetEvents()        noexcept { return m_Events; }

    /**
     * 直近フレームの経過時間を返す。
     *
     * @return 前フレームからの経過秒。
     */
    f32             DeltaTime()  const noexcept { return m_Dt; }

    /**
     * 起動からの総フレーム数を返す。
     *
     * @return 描画したフレームの累計数。
     */
    u64             FrameCount() const noexcept { return m_FrameTimer.FrameCount(); }

    /**
     * 平滑化された現在の FPS を返す。
     *
     * @return 平滑化済みのフレームレート。
     */
    f32             FPS()        const noexcept { return m_FrameTimer.SmoothedFPS(); }

protected:
    /** 起動時に 1 回呼ばれる初期化フック (派生クラスで override)。 */
    virtual void OnStart() noexcept   {}

    /**
     * 毎フレーム呼ばれる更新フック (派生クラスで override)。
     *
     * @param dt 前フレームからの経過秒。
     */
    virtual void OnUpdate(f32 /*dt*/) noexcept {}

    /** 毎フレーム呼ばれる描画フック (BeginFrame/EndFrame は基底が囲む)。 */
    virtual void OnRender() noexcept   {}

    /** 終了時に 1 回呼ばれる後始末フック (派生クラスで override)。 */
    virtual void OnShutdown() noexcept {}

    /**
     * ウィンドウ/入力イベントを受信したときに呼ばれるフック。
     *
     * @param e 受信したイベント。
     */
    virtual void OnEvent(const Event& /*e*/) noexcept {}

    /**
     * フレーム描画を派生クラスで完全に差し替えるためのフック。
     *
     * @details
     * true を返すと基底は BeginFrame/OnRender/EndFrame を呼ばず、派生クラスが
     * コマンドリストとスワップチェインを直接制御する責任を負う。HDR + ポストプロセスの
     * パイプラインを自前で組む場合 (HelloBloom 等) に使う。
     * @return フルカスタム描画を行ったなら true、標準描画に任せるなら false (既定)。
     */
    virtual bool OnCustomFrame() noexcept { return false; }

private:
    /**
     * FWindow のイベントを Input に流しつつ OnEvent も呼ぶ静的ブリッジ。
     *
     * @param user this を指すユーザポインタ (SetEventCallback で登録)。
     * @param e 受信したイベント。
     */
    static void EventBridge(void* user, const Event& e) noexcept;

    /** OS ウィンドウ。 */
    FWindow         m_Window;

    /** レンダラ。 */
    FRenderer       m_Renderer;

    /** ECS World。 */
    World          m_World;

    /** アセットレジストリ。 */
    FAssetRegistry  m_Assets;

    /** タイマーマネージャ。 */
    FTimerManager   m_Timers;

    /** イベントブローカー。 */
    MessageBroker  m_Events;

    /** フレーム計時 (dt・FPS・フレーム数を提供)。 */
    FrameTimer     m_FrameTimer;

    /** 直近フレームの経過秒。 */
    f32            m_Dt       = 0.0f;

    /** メインループ継続フラグ (Quit で false)。 */
    bool           m_bRunning  = true;

    /** Run に渡された起動オプションの控え。 */
    FAppConfig      m_Cfg;

    /** BeginFrame に渡す現在のクリア色。 */
    ClearColor     m_ClearColor{};
};

} // namespace acs
