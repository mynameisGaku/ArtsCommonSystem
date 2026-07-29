// SPDX-License-Identifier: Apache-2.0
// FApplication 実装
#include "app/Application.h"
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
        // reusable FApplication instance (and every early-return path below)
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

} // namespace

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
    if (app->m_RendererFailurePending) return;
    if (event.type == EEventType::WindowResize) {
        if (!app->m_Renderer.OnResize(
                event.resize.width, event.resize.height)) {
            app->m_RendererFailurePending = true;
            return;
        }
    }
    app->OnEvent(event);
}

int FApplication::Run(const FAppConfig& configuration) noexcept
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
    bool renderer_failed = false;

    // メインループ
    while (m_bRunning && !m_Window.ShouldClose()) {
        // フレーム先頭処理
        FInput::Update();            // 押下状態を 1 フレーム進める
        m_Window.PollEvents();      // OS メッセージ処理
        if (m_RendererFailurePending) {
            ACS_LOG_ERROR(
                "FApplication: renderer resize failed; "
                "stopping before frame recording");
            renderer_failed = true;
            break;
        }
        FMemorySystem::ResetTemp(); // Temp セグメントを毎フレーム巻き戻し
        m_Dt = m_FrameTimer.Tick();

        // タイマーをまず進める (派生クラスの OnUpdate より前に発火させて、
        // ゲームロジックがタイマー結果を見られるようにする)
        m_Timers.Tick(m_Dt);

        // 派生クラスの更新フック
        OnUpdate(m_Dt);

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
                    "FApplication: renderer submit/present failed; "
                    "stopping the main loop");
                renderer_failed = true;
                break;
            }
            frame_completed = true;
        }
        if (package_smoke_active && frame_completed) {
            // Publish readiness only after one completed standard or custom
            // frame. Renderer initialization plus scene OnStart alone is not
            // sufficient proof that the staged runtime can render.
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

    // 実行中に既定アロケータから生成された所有物を、MemorySystem より先に解放する。
    // 特に World の SparseSet、MessageBroker の Channel、Asset の共有参照は
    // FApplication 構築時ではなく OnStart 中に生成されるため、この順序が必要になる。
    m_Timers.Clear();
    m_Events.Clear();
    m_World.Clear();
    m_Assets.Shutdown();

    // 派生デストラクタが GPU/既定アロケータ由来のメンバを安全に解放できるよう、
    // Renderer/Window/ThreadPool/MemorySystem/Logger はオブジェクト破棄まで生存させる。
    if (renderer_failed)
        ACS_LOG_ERROR("ACS FApplication stopped after a renderer failure.");
    else
        ACS_LOG_INFO("ACS FApplication run completed cleanly.");
    m_RunCompleted = true;
    m_RunActive = false;
    return renderer_failed ? 6 : 0;
}

} // namespace acs
