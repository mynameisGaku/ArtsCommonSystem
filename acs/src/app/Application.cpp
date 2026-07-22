// SPDX-License-Identifier: Apache-2.0
// FApplication 実装
#include "app/Application.h"
#include "foundation/Log.h"
#include "foundation/Move.h"
#include "foundation/StackTrace.h"
#include "memory/MemorySystem.h"
#include "platform/Input.h"
#include "threading/ThreadPool.h"

namespace acs {

FApplication::FRuntimeFoundationLifetime::~FRuntimeFoundationLifetime() noexcept
{
    Release();
}

void FApplication::FRuntimeFoundationLifetime::InitializeLogger(const FLogConfig& config) noexcept
{
    if (!lifetime_active) {
        // Run 前から存在する DbgHelp 状態は外部所有として扱い、終了時にも残す。
        symbol_resolver_preexisting = FStackTrace::IsSymbolResolverInitialized();
        lifetime_active = true;
    }
    if (FLogger::IsInitialized()) {
        InitializeMemoryDiagnostics();
        return;
    }
    FLogger::Init(config);
    logger_owned = FLogger::IsInitialized();
    InitializeMemoryDiagnostics();
}

void FApplication::FRuntimeFoundationLifetime::InitializeMemoryDiagnostics() noexcept
{
    if (!FCrtDebugHeapDiagnostics::IsSupported() || CrtHeapScope.IsActive()) {
        return;
    }

    // レンダーバックエンドのプロセス寿命シングルトン (Diligent EngineFactory 等) を
    // 計測スコープの外で確定させる。スコープ内で遅延構築されると、static デストラクタ
    // でしか解放されないブロックが終了ダンプに残留し、実リーク無しでも
    // leak_detected=true になる (IRhiDevice.h の PrewarmRhiProcessSingletons 参照)。
    PrewarmRhiProcessSingletons();

    FCrtDebugHeapProcessConfiguration ProcessConfiguration{};
    ProcessConfiguration.bEnableAllocationTracking = true;
    ProcessConfiguration.bEnableProcessExitLeakCheck = false;
    ProcessConfiguration.bRetainFreedBlocks = false;
    ProcessConfiguration.bIncludeCrtBlocks = false;
    ProcessConfiguration.CheckFrequency = ECrtDebugHeapCheckFrequency::Default;
    ProcessConfiguration.bReportToDebugger = true;
    ProcessConfiguration.bReportToStandardError = true;
    if (!CrtProcessConfiguration.Begin(ProcessConfiguration)) {
        return;
    }

    FCrtDebugHeapScopeConfiguration ScopeConfiguration{};
    ScopeConfiguration.ScopeName = "FApplication";
    ScopeConfiguration.bCheckHeapIntegrity = true;
    ScopeConfiguration.bDumpObjectsOnLeak = true;
    ScopeConfiguration.bIncludeCrtBlocksInLeakResult = false;
    ScopeConfiguration.bWriteMachineReadableLog = true;
    if (!CrtHeapScope.Begin(ScopeConfiguration)) {
        CrtProcessConfiguration.End();
    }
}

TResult<void> FApplication::FRuntimeFoundationLifetime::InitializeMemorySystem(const FMemorySystemConfig& config) noexcept
{
    if (FMemorySystem::Get(ESegment::Default) != nullptr) return Ok();
    const auto result = FMemorySystem::Init(config);
    if (result.IsOk()) memory_system_owned = true;
    return result;
}

TResult<void> FApplication::FRuntimeFoundationLifetime::InitializeThreadPool(u32 worker_count) noexcept
{
    if (FThreadPool::WorkerCount() != 0) return Ok();
    const auto result = FThreadPool::Init(worker_count);
    if (result.IsOk()) thread_pool_owned = true;
    return result;
}

void FApplication::FRuntimeFoundationLifetime::Release() noexcept
{
    if (thread_pool_owned) {
        FThreadPool::Shutdown();
        thread_pool_owned = false;
    }
    if (lifetime_active && !symbol_resolver_preexisting && FStackTrace::IsSymbolResolverInitialized()) {
        FStackTrace::ShutdownSymbolResolver();
    }
    if (memory_system_owned) {
        FMemorySystem::Shutdown();
        memory_system_owned = false;
    }
    if (logger_owned) {
        FLogger::Flush();
        FLogger::Shutdown();
        logger_owned = false;
    }
    (void)CrtHeapScope.End();
    CrtProcessConfiguration.End();
    lifetime_active = false;
    symbol_resolver_preexisting = false;
}

// FWindow から Event を受け取るブリッジ
// 1) Input サブシステムに流して状態を更新
// 2) アプリ派生クラスの OnEvent も呼ぶ
// 3) リサイズ時は FRenderer にも通知
void FApplication::EventBridge(void* user_data, const FEvent& event) noexcept
{
    FApplication* const app = static_cast<FApplication*>(user_data);
    FInput::OnEvent(event);
    if (event.type == EEventType::WindowResize) {
        app->m_Renderer.OnResize(event.resize.width, event.resize.height);
    }
    app->OnEvent(event);
}

int FApplication::Run(const FAppConfig& configuration) noexcept
{
    if (m_RunActive) return 5;
    m_RunActive = true;

    // 2 回目の Run は、前回 OnShutdown 後に残していた GPU/Window を安全な順序で閉じる。
    // 派生側は OnShutdown で前 lifecycle の GPU 所有物を解放済みであることが前提となる。
    if (m_RunCompleted) {
        m_Renderer.Shutdown();
        m_Window = FWindow{};
        m_RunCompleted = false;
    }

    m_Cfg = configuration;
    m_ClearColor = FClearColor{configuration.clear_r, configuration.clear_g, configuration.clear_b,
                              configuration.clear_a};
    m_FrameTimer = FFrameTimer{};
    m_Dt = 0.0f;
    m_bRunning = true;

    // 既に起動済みのプロセス基盤は借用し、この Run が起動したものだけを終了する。
    FLogConfig lc{};
    lc.console = true;
    lc.debug_output = true;
    lc.min_severity = configuration.log_severity;
    lc.file_path = configuration.log_file;
    m_RuntimeFoundationLifetime.InitializeLogger(lc);

    ACS_LOG_INFO("ACS FApplication starting up...");

    // メモリシステム初期化
    const auto memory_result = m_RuntimeFoundationLifetime.InitializeMemorySystem(
        configuration.memory ? *configuration.memory : FMemorySystem::DefaultConfig());
    if (memory_result.IsErr()) {
        ACS_LOG_ERROR("FMemorySystem::Init failed: %s", memory_result.Error().message);
        m_RuntimeFoundationLifetime.Release();
        m_RunActive = false;
        return 1;
    }

    // FThreadPool 起動
    const auto thread_pool_result = m_RuntimeFoundationLifetime.InitializeThreadPool(configuration.worker_count);
    if (thread_pool_result.IsErr()) {
        ACS_LOG_ERROR("FThreadPool::Init failed: %s", thread_pool_result.Error().message);
        m_RuntimeFoundationLifetime.Release();
        m_RunActive = false;
        return 2;
    }

    // ウィンドウ作成
    FWindowConfig wc{};
    wc.title = configuration.title;
    wc.width = configuration.width;
    wc.height = configuration.height;
    wc.resizable = configuration.resizable;
    wc.vsync_hint = configuration.vsync;
    auto wr = FWindow::Create(wc);
    if (wr.IsErr()) {
        ACS_LOG_ERROR("FWindow::Create failed: %s", wr.Error().message);
        m_RuntimeFoundationLifetime.Release();
        m_RunActive = false;
        return 3;
    }
    m_Window = Move(wr.Value());
    m_Window.SetEventCallback(&EventBridge, this);

    // レンダラ初期化
    const auto rr = m_Renderer.Init(m_Window, configuration.enable_gpu_debug);
    if (rr.IsErr()) {
        ACS_LOG_ERROR("FRenderer::Init failed: %s", rr.Error().message);
        m_Renderer.Shutdown();
        m_Window = FWindow{};
        m_RuntimeFoundationLifetime.Release();
        m_RunActive = false;
        return 4;
    }

    ACS_LOG_INFO("Backend: %s, GPU: %s", m_Renderer.Device()->BackendName(), m_Renderer.Device()->AdapterName());

    // 標準アセットローダを登録（画像/音声/メッシュ/テキスト/バイナリ）
    m_Assets.Restart();
    m_Assets.RegisterDefaultLoaders();

    // 派生クラスの初期化フック
    OnStart();

    // メインループ
    while (m_bRunning && !m_Window.ShouldClose()) {
        // フレーム先頭処理
        FInput::Update();            // 押下状態を 1 フレーム進める
        m_Window.PollEvents();      // OS メッセージ処理
        FMemorySystem::ResetTemp(); // Temp セグメントを毎フレーム巻き戻し
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

    // 実行中に既定アロケータから生成された所有物を、MemorySystem より先に解放する。
    // 特に World の SparseSet、MessageBroker の Channel、Asset の共有参照は
    // FApplication 構築時ではなく OnStart 中に生成されるため、この順序が必要になる。
    m_Timers.Clear();
    m_Events.Clear();
    m_World.Clear();
    m_Assets.Shutdown();

    // 派生デストラクタが GPU/既定アロケータ由来のメンバを安全に解放できるよう、
    // Renderer/Window/ThreadPool/MemorySystem/Logger はオブジェクト破棄まで生存させる。
    ACS_LOG_INFO("ACS FApplication run completed cleanly.");
    m_RunCompleted = true;
    m_RunActive = false;
    return 0;
}

} // namespace acs
