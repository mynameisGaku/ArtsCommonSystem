// Application 実装
#include "app/Application.h"
#include "foundation/Log.h"
#include "foundation/Move.h"
#include "memory/MemorySystem.h"
#include "platform/Input.h"
#include "threading/ThreadPool.h"

namespace acs {

// Window から Event を受け取るブリッジ
// 1) Input サブシステムに流して状態を更新
// 2) アプリ派生クラスの OnEvent も呼ぶ
// 3) リサイズ時は Renderer にも通知
void Application::EventBridge(void* user, const Event& e) noexcept {
    Application* app = static_cast<Application*>(user);
    Input::OnEvent(e);
    if (e.type == EventType::WindowResize) {
        app->_renderer.OnResize(e.resize.width, e.resize.height);
    }
    app->OnEvent(e);
}

int Application::Run(const AppConfig& cfg) noexcept {
    _cfg = cfg;

    // ロガー初期化（最初に立ち上げて以降のエラーを記録できるように）
    LogConfig lc{};
    lc.console = true;
    lc.debug_output = true;
    lc.min_severity = cfg.log_severity;
    lc.file_path = cfg.log_file;
    Logger::Init(lc);

    ACS_LOG_INFO("ACS Application starting up...");

    // メモリシステム初期化
    auto mr = MemorySystem::Init(cfg.memory ? *cfg.memory : MemorySystem::DefaultConfig());
    if (mr.IsErr()) {
        ACS_LOG_ERROR("MemorySystem::Init failed: %s", mr.Error().message);
        Logger::Shutdown();
        return 1;
    }

    // ThreadPool 起動
    auto tr = ThreadPool::Init(cfg.worker_count);
    if (tr.IsErr()) {
        ACS_LOG_ERROR("ThreadPool::Init failed: %s", tr.Error().message);
        MemorySystem::Shutdown();
        Logger::Shutdown();
        return 2;
    }

    // ウィンドウ作成
    WindowConfig wc{};
    wc.title = cfg.title;
    wc.width = cfg.width;
    wc.height = cfg.height;
    wc.resizable = cfg.resizable;
    wc.vsync_hint = cfg.vsync;
    auto wr = Window::Create(wc);
    if (wr.IsErr()) {
        ACS_LOG_ERROR("Window::Create failed: %s", wr.Error().message);
        ThreadPool::Shutdown();
        MemorySystem::Shutdown();
        Logger::Shutdown();
        return 3;
    }
    _window = Move(wr.Value());
    _window.SetEventCallback(&EventBridge, this);

    // レンダラ初期化
    auto rr = _renderer.Init(_window, cfg.enable_gpu_debug);
    if (rr.IsErr()) {
        ACS_LOG_ERROR("Renderer::Init failed: %s", rr.Error().message);
        ThreadPool::Shutdown();
        MemorySystem::Shutdown();
        Logger::Shutdown();
        return 4;
    }

    ACS_LOG_INFO("Backend: %s, GPU: %s",
                 _renderer.Device()->BackendName(),
                 _renderer.Device()->AdapterName());

    // 派生クラスの初期化フック
    OnStart();

    // メインループ
    while (_running && !_window.ShouldClose()) {
        // フレーム先頭処理
        Input::Update();         // 押下状態を 1 フレーム進める
        _window.PollEvents();    // OS メッセージ処理
        MemorySystem::ResetTemp();  // Temp セグメントを毎フレーム巻き戻し
        _dt = _frame_timer.Tick();

        // 派生クラスの更新フック
        OnUpdate(_dt);

        // フレーム描画
        ClearColor cc{ cfg.clear_r, cfg.clear_g, cfg.clear_b, cfg.clear_a };
        _renderer.BeginFrame(cc);
        OnRender();
        _renderer.EndFrame();
    }

    // 派生クラスの終了フック
    OnShutdown();

    // サブシステムの破棄（逆順）
    _renderer.Shutdown();
    ThreadPool::Shutdown();
    MemorySystem::Shutdown();

    ACS_LOG_INFO("ACS Application shut down cleanly.");
    Logger::Flush();
    Logger::Shutdown();
    return 0;
}

} // namespace acs
