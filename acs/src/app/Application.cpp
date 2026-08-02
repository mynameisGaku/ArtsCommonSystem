// SPDX-License-Identifier: Apache-2.0
// CApplication 実装
#include "app/Application.h"
#include "app/ApplicationSubsystemCatalog.h"
#include "app/AssetSubsystem.h"
#include "app/TimerSubsystem.h"
#include "foundation/Log.h"
#include "foundation/Move.h"
#include "foundation/Platform.h"
#include "foundation/StackTrace.h"
#include "memory/MemorySystem.h"
#include "platform/Input.h"
#include "threading/ThreadPool.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace acs {

namespace {

constexpr usize kPackageSmokeTokenLength = 64;
constexpr const char* kPackageSmokeTokenEnvironment =
    "ACS_PACKAGE_SMOKE_TOKEN";

bool ReadAuthenticatedPackageSmokeToken(
    char (&out_token)[kPackageSmokeTokenLength + 1]) noexcept
{
    out_token[0] = '\0';
#if defined(_WIN32)
    const DWORD length = ::GetEnvironmentVariableA(
        kPackageSmokeTokenEnvironment,
        out_token,
        static_cast<DWORD>(sizeof(out_token)));
    if (length != kPackageSmokeTokenLength) {
        out_token[0] = '\0';
        return false;
    }
#else
    const char* token = std::getenv(kPackageSmokeTokenEnvironment);
    if (token == nullptr ||
        std::strlen(token) != kPackageSmokeTokenLength) {
        return false;
    }
    std::memcpy(
        out_token,
        token,
        kPackageSmokeTokenLength + 1);
#endif
    for (usize index = 0; index < kPackageSmokeTokenLength; ++index) {
        const char value = out_token[index];
        if (!((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f'))) {
            out_token[0] = '\0';
            return false;
        }
    }
    return true;
}

void PublishPackageSmokeReady(const char* token) noexcept
{
    if (token == nullptr) return;
    std::fprintf(stdout, "ACS_PACKAGE_SMOKE_V1 READY %s\n", token);
    std::fflush(stdout);
}

#if defined(_WIN32)
class FScopedPackageSmokeErrorMode final {
public:
    explicit FScopedPackageSmokeErrorMode(bool active) noexcept
    {
        if (!active) return;
        // SetErrorMode is process-wide. Preserve the caller's mode so a
        // reusable CApplication instance (and every early-return path below)
        // cannot leak smoke-only dialog suppression into a later normal run.
        m_PreviousMode = ::SetErrorMode(
            SEM_FAILCRITICALERRORS |
            SEM_NOGPFAULTERRORBOX |
            SEM_NOOPENFILEERRORBOX);
        m_Active = true;
    }

    ~FScopedPackageSmokeErrorMode() noexcept
    {
        if (m_Active) {
            ::SetErrorMode(m_PreviousMode);
        }
    }

    FScopedPackageSmokeErrorMode(
        const FScopedPackageSmokeErrorMode&) = delete;
    FScopedPackageSmokeErrorMode& operator=(
        const FScopedPackageSmokeErrorMode&) = delete;

private:
    UINT m_PreviousMode = 0u;
    bool m_Active = false;
};
#endif

} // 無名名前空間

CApplication::FRuntimeFoundationLifetime::~FRuntimeFoundationLifetime() noexcept
{
    Release();
}

void CApplication::FRuntimeFoundationLifetime::InitializeLogger(const FLogConfig& config) noexcept
{
    if (!lifetime_active) {
        // Run 前から存在する DbgHelp 状態は外部所有として扱い、終了時にも残す。
        symbol_resolver_preexisting = FStackTrace::IsSymbolResolverInitialized();
        lifetime_active = true;
    }
    if (CLogger::IsInitialized()) {
        InitializeMemoryDiagnostics();
        return;
    }
    CLogger::Init(config);
    logger_owned = CLogger::IsInitialized();
    InitializeMemoryDiagnostics();
}

void CApplication::FRuntimeFoundationLifetime::InitializeMemoryDiagnostics() noexcept
{
    if (!CCrtDebugHeapDiagnostics::IsSupported() || CrtHeapScope.IsActive()) {
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

TResult<void> CApplication::FRuntimeFoundationLifetime::InitializeMemorySystem(const FMemorySystemConfig& config) noexcept
{
    if (CMemorySystem::Get(ESegment::Default) != nullptr) return Ok();
    const auto result = CMemorySystem::Init(config);
    if (result.IsOk()) memory_system_owned = true;
    return result;
}

TResult<void> CApplication::FRuntimeFoundationLifetime::InitializeThreadPool(u32 worker_count) noexcept
{
    if (CThreadPool::WorkerCount() != 0) return Ok();
    const auto result = CThreadPool::Init(worker_count);
    if (result.IsOk()) thread_pool_owned = true;
    return result;
}

void CApplication::FRuntimeFoundationLifetime::Release() noexcept
{
    if (thread_pool_owned) {
        CThreadPool::Shutdown();
        thread_pool_owned = false;
    }
    if (lifetime_active && !symbol_resolver_preexisting && FStackTrace::IsSymbolResolverInitialized()) {
        FStackTrace::ShutdownSymbolResolver();
    }
    if (memory_system_owned) {
        CMemorySystem::Shutdown();
        memory_system_owned = false;
    }
    if (logger_owned) {
        CLogger::Flush();
        CLogger::Shutdown();
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
// 3) リサイズ時は CRenderer にも通知
void CApplication::EventBridge(void* user_data, const FEvent& event) noexcept
{
    CApplication* const app = static_cast<CApplication*>(user_data);
    CInput::OnEvent(event);
    if (app->m_RendererFailurePending) return;
    if (event.type == EEventType::WindowResize && !app->m_Window.IsMinimized()) {
        if (!app->m_Renderer.OnResize(event.resize.width, event.resize.height)) {
            app->m_RendererFailurePending = true;
            return;
        }
    }
    app->OnEvent(event);
}

int CApplication::Run(const FAppConfig& configuration) noexcept
{
    if (m_RunActive) return 5;
    m_RunActive = true;
    char package_smoke_token[kPackageSmokeTokenLength + 1]{};
    const bool package_smoke_active =
        ReadAuthenticatedPackageSmokeToken(package_smoke_token);
#if defined(_WIN32)
    // A smoke launch is machine-driven. Never let an OS critical-error or
    // unhandled-fault dialog turn its bounded timeout into an interactive
    // modal prompt, but restore the caller's process mode on every exit.
    const FScopedPackageSmokeErrorMode package_smoke_error_mode(
        package_smoke_active);
#endif

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
    m_RendererFailurePending = false;

    // 既に起動済みのプロセス基盤は借用し、この Run が起動したものだけを終了する。
    FLogConfig lc{};
    lc.console = true;
    lc.debug_output = true;
    lc.min_severity = configuration.log_severity;
    lc.file_path = configuration.log_file;
    m_RuntimeFoundationLifetime.InitializeLogger(lc);

    ACS_LOG_INFO("ACS CApplication starting up...");

    // メモリシステム初期化
    const auto memory_result = m_RuntimeFoundationLifetime.InitializeMemorySystem(
        configuration.memory ? *configuration.memory : CMemorySystem::DefaultConfig());
    if (memory_result.IsErr()) {
        ACS_LOG_ERROR("CMemorySystem::Init failed: %s", memory_result.Error().message);
        m_RuntimeFoundationLifetime.Release();
        m_RunActive = false;
        return 1;
    }

    // CThreadPool 起動
    const auto thread_pool_result = m_RuntimeFoundationLifetime.InitializeThreadPool(configuration.worker_count);
    if (thread_pool_result.IsErr()) {
        ACS_LOG_ERROR("CThreadPool::Init failed: %s", thread_pool_result.Error().message);
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
    wc.visible = !package_smoke_active;
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
        ACS_LOG_ERROR("CRenderer::Init failed: %s", rr.Error().message);
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

    // Application 所有の既存サービスを Engine サブシステムとして公開する。
    const bool application_subsystems_registered = AcsRegisterApplicationSubsystems();
    const bool engine_subsystems_initialized = application_subsystems_registered && m_EngineSubsystems.TryInitialize(ESubsystemScope::Engine, nullptr, FSubsystemOwner{this, ESubsystemOwnerKind::Application});
    ATimerSubsystem* const timer_subsystem = GetSubsystem<ATimerSubsystem>();
    AAssetSubsystem* const asset_subsystem = GetSubsystem<AAssetSubsystem>();
    if (!engine_subsystems_initialized || timer_subsystem == nullptr || asset_subsystem == nullptr ||
        timer_subsystem->GetTimers() != &m_Timers || asset_subsystem->GetAssets() != &m_Assets) {
        ACS_LOG_ERROR("CApplication: Engine subsystem initialization failed");
        m_EngineSubsystems.Deinitialize();
        m_Timers.Clear();
        m_Events.Clear();
        m_World.Clear();
        m_Assets.Shutdown();
        m_Renderer.Shutdown();
        m_Window = FWindow{};
        m_RuntimeFoundationLifetime.Release();
        m_RunActive = false;
        return 7;
    }

    // 派生クラスの初期化フック
    OnStart();
    bool renderer_failed = false;

    // メインループ
    while (m_bRunning && !m_Window.ShouldClose()) {
        // フレーム先頭処理
        CInput::Update();            // 押下状態を 1 フレーム進める
        m_Window.PollEvents();      // OS メッセージ処理
        if (m_RendererFailurePending) {
            ACS_LOG_ERROR(
                "CApplication: renderer resize failed; "
                "stopping before frame recording");
            renderer_failed = true;
            break;
        }
        if (!m_bRunning || m_Window.ShouldClose()) break;

        // 最小化中のスワップチェーンには描画すべき画素がない。フレーム時計だけを進めて
        // 復帰直後の dt が数秒分へ跳ねることを防ぎ、ビジー描画せず入力または上限時間まで待つ。
        if (m_Window.IsMinimized()) {
            (void)m_FrameTimer.Tick();
            m_Window.WaitForEvents(configuration.minimized_wait_ms);
            continue;
        }
        CMemorySystem::ResetTemp(); // Temp セグメントを毎フレーム巻き戻し
        m_Dt = m_FrameTimer.Tick();

        // Engine の PreUpdate を生時間で進め、既存タイマーを OnUpdate より前に発火させる。
        const FSubsystemFrameContext PreContext{m_Dt, m_Dt, m_FrameTimer.FrameCount(), ESubsystemTickPhase::PreUpdate};
        m_EngineSubsystems.TickFrame(PreContext);

        // 派生クラスの更新フック
        OnUpdate(m_Dt);

        // 派生更新後に Engine の PostUpdate を同じフレーム情報で進める。
        const FSubsystemFrameContext PostContext{m_Dt, m_Dt, m_FrameTimer.FrameCount(), ESubsystemTickPhase::PostUpdate};
        m_EngineSubsystems.TickFrame(PostContext);

        // フレーム描画 — OnCustomFrame() が true を返すなら派生クラスに完全委譲
        const bool custom_frame_completed = OnCustomFrame();
        bool frame_completed =
            custom_frame_completed &&
            m_bRunning &&
            m_Renderer.Device() &&
            m_Renderer.Device()->IsOperational();
        if (!custom_frame_completed) {
            // m_ClearColor は既定で FAppConfig 由来。SetClearColor() で毎フレーム変更可能。
            m_Renderer.BeginFrame(m_ClearColor);
            OnRender();
            if (!m_Renderer.EndFrame()) {
                ACS_LOG_ERROR(
                    "CApplication: renderer submit/present failed; "
                    "stopping the main loop");
                renderer_failed = true;
                break;
            }
            frame_completed = true;
        }
        if (package_smoke_active && frame_completed) {
            // 標準または独自フレームが一つ完了してから準備完了を公開する。
            // レンダラー初期化とシーン開始だけでは、段階配置した実行環境が
            // 描画できる証拠として不十分なためである。
            PublishPackageSmokeReady(package_smoke_token);
            m_bRunning = false;
        }
    }

    // 派生クラスが GPU リソースを保持しているはずなので、OnShutdown より先に
    // GPU 完了を待つ。これを忘れると use-after-free でクラッシュしがち。
    if (m_Renderer.Device() && m_Renderer.Device()->IsOperational())
        m_Renderer.Device()->WaitIdle();

    // 派生クラスの終了フック
    OnShutdown();

    // 派生側が参照を使い終えてから Engine サブシステムを逆順で終了する。
    m_EngineSubsystems.Deinitialize();

    // 実行中に既定アロケータから生成された所有物を、MemorySystem より先に解放する。
    // 特に World の SparseSet、MessageBroker の Channel、Asset の共有参照は
    // CApplication 構築時ではなく OnStart 中に生成されるため、この順序が必要になる。
    m_Timers.Clear();
    m_Events.Clear();
    m_World.Clear();
    m_Assets.Shutdown();

    // 派生デストラクタが GPU/既定アロケータ由来のメンバを安全に解放できるよう、
    // Renderer/Window/ThreadPool/MemorySystem/Logger はオブジェクト破棄まで生存させる。
    if (renderer_failed)
        ACS_LOG_ERROR("ACS CApplication stopped after a renderer failure.");
    else
        ACS_LOG_INFO("ACS CApplication run completed cleanly.");
    m_RunCompleted = true;
    m_RunActive = false;
    return renderer_failed ? 6 : 0;
}

} // acs 名前空間
