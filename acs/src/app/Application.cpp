// SPDX-License-Identifier: Apache-2.0
// FApplication 実装
#include "app/Application.h"
#include "foundation/Log.h"
#include "foundation/Move.h"
#include "memory/MemorySystem.h"
#include "platform/Input.h"
#include "threading/ThreadPool.h"

namespace acs {

// FWindow から Event を受け取るブリッジ
// 1) Input サブシステムに流して状態を更新
// 2) アプリ派生クラスの OnEvent も呼ぶ
// 3) リサイズ時は FRenderer にも通知
void FApplication::EventBridge(void* user, const Event& e) noexcept {
    FApplication* app = static_cast<FApplication*>(user);
    Input::OnEvent(e);
    if (e.type == EventType::WindowResize) {
        app->m_Renderer.OnResize(e.resize.width, e.resize.height);
    }
    app->OnEvent(e);
}

int FApplication::Run(const FAppConfig& cfg) noexcept {
    m_Cfg = cfg;
    m_ClearColor = ClearColor{ cfg.clear_r, cfg.clear_g, cfg.clear_b, cfg.clear_a };

    // ロガー初期化（最初に立ち上げて以降のエラーを記録できるように）
    FLogConfig lc{};
    lc.console = true;
    lc.debug_output = true;
    lc.min_severity = cfg.log_severity;
    lc.file_path = cfg.log_file;
    FLogger::Init(lc);

    ACS_LOG_INFO("ACS FApplication starting up...");

    // メモリシステム初期化
    auto mr = FMemorySystem::Init(cfg.memory ? *cfg.memory : FMemorySystem::DefaultConfig());
    if (mr.IsErr()) {
        ACS_LOG_ERROR("FMemorySystem::Init failed: %s", mr.Error().message);
        FLogger::Shutdown();
        return 1;
    }

    // FThreadPool 起動
    auto tr = FThreadPool::Init(cfg.worker_count);
    if (tr.IsErr()) {
        ACS_LOG_ERROR("FThreadPool::Init failed: %s", tr.Error().message);
        FMemorySystem::Shutdown();
        FLogger::Shutdown();
        return 2;
    }

    // ウィンドウ作成
    FWindowConfig wc{};
    wc.title = cfg.title;
    wc.width = cfg.width;
    wc.height = cfg.height;
    wc.resizable = cfg.resizable;
    wc.vsync_hint = cfg.vsync;
    auto wr = FWindow::Create(wc);
    if (wr.IsErr()) {
        ACS_LOG_ERROR("FWindow::Create failed: %s", wr.Error().message);
        FThreadPool::Shutdown();
        FMemorySystem::Shutdown();
        FLogger::Shutdown();
        return 3;
    }
    m_Window = Move(wr.Value());
    m_Window.SetEventCallback(&EventBridge, this);

    // レンダラ初期化
    auto rr = m_Renderer.Init(m_Window, cfg.enable_gpu_debug);
    if (rr.IsErr()) {
        ACS_LOG_ERROR("FRenderer::Init failed: %s", rr.Error().message);
        FThreadPool::Shutdown();
        FMemorySystem::Shutdown();
        FLogger::Shutdown();
        return 4;
    }

    ACS_LOG_INFO("Backend: %s, GPU: %s",
                 m_Renderer.Device()->BackendName(),
                 m_Renderer.Device()->AdapterName());

    // 標準アセットローダを登録（画像/音声/メッシュ/テキスト/バイナリ）
    m_Assets.RegisterDefaultLoaders();

    // 派生クラスの初期化フック
    OnStart();

    // メインループ
    while (m_bRunning && !m_Window.ShouldClose()) {
        // フレーム先頭処理
        Input::Update();         // 押下状態を 1 フレーム進める
        m_Window.PollEvents();    // OS メッセージ処理
        FMemorySystem::ResetTemp();  // Temp セグメントを毎フレーム巻き戻し
        m_Dt = m_FrameTimer.Tick();

        // タイマーをまず進める (派生クラスの OnUpdate より前に発火させて、
        // ゲームロジックがタイマー結果を見られるようにする)
        m_Timers.Tick(m_Dt);

        // 派生クラスの更新フック
        OnUpdate(m_Dt);

        // フレーム描画 — OnCustomFrame() が true を返すなら派生クラスに完全委譲
        if (!OnCustomFrame()) {
            // m_ClearColor は既定で FAppConfig 由来。SetClearColor() で毎フレーム変更可能。
            m_Renderer.BeginFrame(m_ClearColor);
            OnRender();
            m_Renderer.EndFrame();
        }
    }

    // 派生クラスが GPU リソースを保持しているはずなので、OnShutdown より先に
    // GPU 完了を待つ。これを忘れると use-after-free でクラッシュしがち。
    if (m_Renderer.Device()) m_Renderer.Device()->WaitIdle();

    // 派生クラスの終了フック
    OnShutdown();

    // サブシステムの破棄（逆順）
    m_Renderer.Shutdown();
    FThreadPool::Shutdown();
    FMemorySystem::Shutdown();

    ACS_LOG_INFO("ACS FApplication shut down cleanly.");
    FLogger::Flush();
    FLogger::Shutdown();
    return 0;
}

} // namespace acs
