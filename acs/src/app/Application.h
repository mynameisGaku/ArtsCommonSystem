// SPDX-License-Identifier: Apache-2.0
// CApplication 基底（継承して OnStart / OnUpdate / OnRender / OnShutdown を実装）
//
// 使い方:
//   class FMyGame : public CApplication {
//   public:
//       // フックは必ず noexcept override で宣言する
//       // （基底のフックが noexcept のため。noexcept を省くとコンパイルエラー）
//       void OnStart() noexcept override {
//           ACS_LOG_INFO("ゲーム開始");
//       }
//       void OnUpdate(f32 delta_time) noexcept override {
//           if (CInput::IsKeyPressed(EKey::Escape)) Quit();
//       }
//       void OnRender() noexcept override {
//           // 描画コマンド (BeginFrame / EndFrame は基底が呼ぶ)
//       }
//   };
//
//   ACS_DEFINE_MAIN(FMyGame)   // エントリポイントを自動生成 (app/EntryPoint.h)
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
#include "app/Forward.h"
#include "memory/CrtDebugHeapDiagnostics.h"
#include "subsystem/SubsystemCollection.h"

namespace acs {

namespace app_internal {
class FApplicationTestAccess;
}

/**
 * ゲーム/サンプルが継承するアプリケーション基底クラス。
 *
 * @details
 * ウィンドウ・レンダラ・ECS CWorld・アセット・タイマー・イベントブローカーといった
 * エンジンサブシステムを所有し、Run() で初期化からメインループ・後始末までを駆動する。
 * 派生クラスは OnStart/OnUpdate/OnRender/OnShutdown/OnEvent を override してロジックと
 * 描画を書く。non-copy 型で、通常は ACS_DEFINE_MAIN マクロ経由でインスタンス化する。
 */
class CApplication {
public:
    /** 既定状態で構築する (サブシステムは Run で初期化)。 */
    CApplication() noexcept = default;

    /** 派生クラスを正しく破棄するための仮想デストラクタ。 */
    virtual ~CApplication() noexcept = default;

    /** コピー禁止 (エンジンサブシステムを単独所有するため)。 */
    CApplication(const CApplication&) = delete;

    /** コピー代入も禁止。 */
    CApplication& operator=(const CApplication&) = delete;

    /**
     * サブシステムを初期化し、メインループを最後まで回す。
     *
     * @details
     * ロガー→メモリ→スレッドプール→ウィンドウ→レンダラの順に立ち上げ、OnStart を呼び、
     * Quit() か OnClose まで毎フレーム update/描画を回す。正常終了後も派生メンバの安全な
     * 破棄に必要なレンダラと、この Run が所有するプロセス基盤はデストラクタまで生存する。
     * 再度 Run する場合は前回のレンダラを先に閉じるため、派生側の前回 GPU 所有物は
     * OnShutdown で解放しておくこと。
     * @param configuration ウィンドウ・ロガー・レンダラ等の起動オプション。
     * @return 正常終了で 0、初期化失敗で 1〜4、同一オブジェクトへの再入で 5、
     *         実行中の resize/submit/present 失敗で 6、Engine サブシステム初期化失敗で 7。
     */
    int Run(const FAppConfig& configuration) noexcept;

    /** メインループを抜けて終了するよう要求する (OnShutdown が呼ばれる)。 */
    void Quit() noexcept
    {
        m_bRunning = false;
    }

    /**
     * 背景クリア色を動的に変更する (次フレームの描画から反映)。
     *
     * @details 既定値は起動時 FAppConfig の clear_r/g/b/a。
     * @param red 赤成分 (0..1)。
     * @param green 緑成分 (0..1)。
     * @param blue 青成分 (0..1)。
     * @param alpha アルファ成分 (0..1、既定 1.0)。
     */
    void SetClearColor(f32 red, f32 green, f32 blue, f32 alpha = 1.0f) noexcept
    {
        m_ClearColor = FClearColor{red, green, blue, alpha};
    }

    /**
     * 現在の背景クリア色を返す。
     *
     * @details マルチパス描画で RT 再バインド時のクリアに使う。
     * @return 現在のクリア色への const 参照。
     */
    const FClearColor& GetClearColor() const noexcept
    {
        return m_ClearColor;
    }

    /**
     * ウィンドウへの参照を返す。
     *
     * @return アプリが所有する FWindow への参照。
     */
    FWindow& GetWindow() noexcept
    {
        return m_Window;
    }

    /**
     * レンダラへの参照を返す。
     *
     * @return アプリが所有する CRenderer への参照。
     */
    CRenderer& GetRenderer() noexcept
    {
        return m_Renderer;
    }

    /**
     * ECS CWorld への参照を返す。
     *
     * @return アプリが所有する CWorld への参照。
     */
    CWorld& GetWorld() noexcept
    {
        return m_World;
    }

    /**
     * アセットレジストリへの参照を返す。
     *
     * @return アプリが所有する CAssetRegistry への参照。
     */
    CAssetRegistry& GetAssets() noexcept
    {
        return m_Assets;
    }

    /**
     * タイマーマネージャへの参照を返す。
     *
     * @return アプリが所有する CTimerManager への参照。
     */
    CTimerManager& GetTimers() noexcept
    {
        return m_Timers;
    }

    /**
     * イベントブローカーへの参照を返す。
     *
     * @return アプリが所有する CMessageBroker への参照。
     */
    CMessageBroker& GetEvents() noexcept
    {
        return m_Events;
    }

    /** Application 寿命の Engine サブシステム群を返す。 */
    CSubsystemCollection& EngineSubsystems() noexcept
    {
        return m_EngineSubsystems;
    }

    /** Application 寿命の Engine サブシステムを型で取得する。 */
    template<typename T>
    T* GetSubsystem() noexcept
    {
        return m_EngineSubsystems.Get<T>();
    }

    /**
     * 直近フレームの経過時間を返す。
     *
     * @return 前フレームからの経過秒。
     */
    f32 DeltaTime() const noexcept
    {
        return m_Dt;
    }

    /**
     * 起動からの総フレーム数を返す。
     *
     * @return 描画したフレームの累計数。
     */
    u64 FrameCount() const noexcept
    {
        return m_FrameTimer.FrameCount();
    }

    /**
     * 平滑化された現在の FPS を返す。
     *
     * @return 平滑化済みのフレームレート。
     */
    f32 FPS() const noexcept
    {
        return m_FrameTimer.SmoothedFPS();
    }

protected:
    /** 起動時に 1 回呼ばれる初期化フック (派生クラスで override)。 */
    virtual void OnStart() noexcept
    {
    }

    /**
     * 毎フレーム呼ばれる更新フック (派生クラスで override)。
     *
     * @param delta_time 前フレームからの経過秒。
     */
    virtual void OnUpdate(f32 /*delta_time*/) noexcept
    {
    }

    /** 毎フレーム呼ばれる描画フック (BeginFrame/EndFrame は基底が囲む)。 */
    virtual void OnRender() noexcept
    {
    }

    /** 終了時に 1 回呼ばれる後始末フック (派生クラスで override)。 */
    virtual void OnShutdown() noexcept
    {
    }

    /**
     * ウィンドウ/入力イベントを受信したときに呼ばれるフック。
     *
     * @param event 受信したイベント。
     */
    virtual void OnEvent(const FEvent& /*event*/) noexcept
    {
    }

    /**
     * フレーム描画を派生クラスで完全に差し替えるためのフック。
     *
     * @details
     * true を返すと基底は BeginFrame/OnRender/EndFrame を呼ばず、派生クラスが
     * コマンドリストとスワップチェインを直接制御する責任を負う。HDR + ポストプロセスの
     * パイプラインを自前で組む場合 (HelloBloom 等) に使う。true は submit/present
     * まで担当したフレームを表す。担当後に失敗した場合は Quit() を呼んでから true を
     * 返すこと。false は標準描画へ委譲する。
     * @return フルカスタム描画を行ったなら true、標準描画に任せるなら false (既定)。
     */
    virtual bool OnCustomFrame() noexcept
    {
        return false;
    }

private:
    /**
     * Run が起動したプロセス基盤だけを保持し、CApplication の全メンバより後に停止する。
     *
     * @details この型のインスタンスを最初のデータメンバに置くことで、派生メンバ、レンダラ、
     * ウィンドウ、基底コンテナの破棄後に CThreadPool、CMemorySystem、CLogger を停止できる。
     */
    struct FRuntimeFoundationLifetime {
        /** 所有中の基盤を逆順で停止する。 */
        ~FRuntimeFoundationLifetime() noexcept;

        /** 未起動なら CLogger を起動し、成功時だけ所有権を記録する。 */
        void InitializeLogger(const FLogConfig& config) noexcept;

        /** Debug CRT のプロセス設定と基盤寿命スコープ診断を開始する。 */
        void InitializeMemoryDiagnostics() noexcept;

        /** 未起動なら CMemorySystem を起動し、成功時だけ所有権を記録する。 */
        TResult<void> InitializeMemorySystem(const FMemorySystemConfig& config) noexcept;

        /** 未起動なら CThreadPool を起動し、成功時だけ所有権を記録する。 */
        TResult<void> InitializeThreadPool(u32 worker_count) noexcept;

        /** 起動途中の失敗時にも使える冪等な明示解放。 */
        void Release() noexcept;

        /** このガードが CLogger の終了責任を持つ。 */
        bool logger_owned = false;

        /** このガードが CMemorySystem の終了責任を持つ。 */
        bool memory_system_owned = false;

        /** このガードが CThreadPool の終了責任を持つ。 */
        bool thread_pool_owned = false;

        /** Run の基盤所有権を追跡中である。 */
        bool lifetime_active = false;

        /** 追跡開始前から DbgHelp シンボルリゾルバが外部で初期化済みだった。 */
        bool symbol_resolver_preexisting = false;

        /** 基盤寿命中だけ適用する Debug CRT プロセス設定。 */
        FCrtDebugHeapProcessConfigurationScope CrtProcessConfiguration;

        /** 基盤寿命中の CRT ヒープ正味増加を終了時に検査する。 */
        FCrtDebugHeapScope CrtHeapScope;
    };

    friend class app_internal::FApplicationTestAccess;

    /**
     * FWindow のイベントを CInput に流しつつ OnEvent も呼ぶ静的ブリッジ。
     *
     * @param user_data this を指すユーザポインタ (SetEventCallback で登録)。
     * @param event 受信したイベント。
     */
    static void EventBridge(void* user_data, const FEvent& event) noexcept;

    // 最初に構築して最後に破棄する。以降のメンバを dead allocator/device より先に解放する。
    FRuntimeFoundationLifetime m_RuntimeFoundationLifetime;

    /** OS ウィンドウ。 */
    FWindow m_Window;

    /** レンダラ。 */
    CRenderer m_Renderer;

    /** ECS CWorld。 */
    CWorld m_World;

    /** アセットレジストリ。 */
    CAssetRegistry m_Assets;

    /** タイマーマネージャ。 */
    CTimerManager m_Timers;

    /** イベントブローカー。 */
    CMessageBroker m_Events;

    /** Application が所有する Engine スコープのサブシステム群。 */
    CSubsystemCollection m_EngineSubsystems;

    /** フレーム計時 (dt・FPS・フレーム数を提供)。 */
    FFrameTimer m_FrameTimer;

    /** 直近フレームの経過秒。 */
    f32 m_Dt = 0.0f;

    /** メインループ継続フラグ (Quit で false)。 */
    bool m_bRunning = true;

    /**
     * WindowResize callback で検出した描画系失敗。
     *
     * Resize は失敗時にバックバッファ未取得状態になり得るため、イベント処理から
     * 戻った同じ iteration で BeginFrame へ進まず、安全に終了する。
     */
    bool m_RendererFailurePending = false;

    /** Run の再入を防ぐ。 */
    bool m_RunActive = false;

    /** 正常 Run 後のレンダラとウィンドウが派生デストラクタ待ちで残っている。 */
    bool m_RunCompleted = false;

    /** Run に渡された起動オプションの控え。 */
    FAppConfig m_Cfg;

    /** BeginFrame に渡す現在のクリア色。 */
    FClearColor m_ClearColor{};
};

} // namespace acs
