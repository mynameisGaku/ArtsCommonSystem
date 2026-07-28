using System;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace AcsEditor;

public partial class App : Application
{
    private const int GwlExStyle = -20;
    private const long WsExNoActivate = 0x08000000L;
    private const int SmRemoteSession = 0x1000;

    internal const int WmMouseActivate = 0x0021;
    internal const int MaActivate = 1;
    private const int WmLButtonDown = 0x0201;
    private const int WmRButtonDown = 0x0204;
    private const int WmMButtonDown = 0x0207;
    private const int WmXButtonDown = 0x020B;
    private const int WmNcLButtonDown = 0x00A1;
    private const int WmNcRButtonDown = 0x00A4;
    private const int WmNcMButtonDown = 0x00A7;
    private const int WmNcXButtonDown = 0x00AB;
    private const int WmPointerDown = 0x0246;

    /// <summary>
    /// True for unattended visual-validation launches.  In this mode the
    /// editor remains visible and renders normally, but neither its WPF
    /// shortcut layer nor the native viewport child may consume user input.
    /// </summary>
    public static bool IsNonInteractiveLaunch { get; private set; }

    /// <summary>
    /// True while an interactive <c>--no-activate</c> launch is waiting for
    /// the editor window's first activation.  Startup work may continue while
    /// this is set, but owned modal prompts must wait so they cannot take focus
    /// on behalf of the deliberately inactive editor.
    /// </summary>
    public static bool IsInitialActivationSuppressed { get; private set; }

    /// <summary>
    /// Reliability-only escape hatch: the normal-start soak remains
    /// non-activating, but may present its owned modeless recovery fixture so
    /// that the original startup-freeze path is actually exercised.
    /// </summary>
    internal static bool IsInactiveRecoveryPromptAllowed { get; private set; }

    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")]
    private static extern nint GetWindowLongPtr(nint window, int index);

    [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW")]
    private static extern nint SetWindowLongPtr(
        nint window, int index, nint newValue);

    [DllImport("user32.dll")]
    private static extern int GetSystemMetrics(int index);

    internal static bool ShouldForceSoftwareUi(
        string[] arguments,
        bool isRemoteSession,
        int renderingTier) =>
        isRemoteSession ||
        arguments.Contains("--software-ui", StringComparer.OrdinalIgnoreCase);

    internal static bool ShouldRunUnattended(string[] arguments) =>
        arguments.Contains("--unattended", StringComparer.OrdinalIgnoreCase);

    internal static bool ShouldAvoidInitialActivation(string[] arguments) =>
        ShouldRunUnattended(arguments) ||
        arguments.Contains("--no-activate", StringComparer.OrdinalIgnoreCase);

    internal static bool ShouldDeferInteractivePromptsUntilActivation(
        string[] arguments) =>
        !ShouldRunUnattended(arguments) &&
        arguments.Contains("--no-activate", StringComparer.OrdinalIgnoreCase);

    internal static bool ShouldReleaseInitialActivationGuard(
        bool initialActivationSuppressed,
        bool nonInteractiveLaunch,
        int message,
        int activationInputMessage) =>
        initialActivationSuppressed &&
        !nonInteractiveLaunch &&
        message == WmMouseActivate &&
        IsExplicitActivationInput(activationInputMessage);

    private static bool IsExplicitActivationInput(int message) =>
        message == WmLButtonDown ||
        message == WmRButtonDown ||
        message == WmMButtonDown ||
        message == WmXButtonDown ||
        message == WmNcLButtonDown ||
        message == WmNcRButtonDown ||
        message == WmNcMButtonDown ||
        message == WmNcXButtonDown ||
        message == WmPointerDown;

    internal static bool ReleaseInitialEditorActivation(Window window)
    {
        if (!IsInitialActivationSuppressed || IsNonInteractiveLaunch)
        {
            return false;
        }

        SetNoActivateStyle(window, enabled: false);
        IsInitialActivationSuppressed = false;
        return true;
    }

    private static void SetNoActivateStyle(Window window, bool enabled)
    {
        nint handle = new WindowInteropHelper(window).Handle;
        if (handle == 0) return;

        nint style = GetWindowLongPtr(handle, GwlExStyle);
        long current = style.ToInt64();
        long updated = enabled
            ? current | WsExNoActivate
            : current & ~WsExNoActivate;
        if (updated == current) return;

        SetWindowLongPtr(
            handle,
            GwlExStyle,
            (nint)updated);
    }

    protected override async void OnStartup(StartupEventArgs e)
    {
        // SoftwareOnly makes the entire editor UI more expensive. Keep the
        // compatibility fallback for remote sessions (and the explicit
        // diagnostic switch), but let normal local editors use WPF hardware
        // composition. WPF performs its own Tier fallback if the adapter truly
        // cannot compose in hardware; forcing SoftwareOnly from the pre-startup
        // Tier probe can otherwise pin a capable desktop.
        int renderingTier = RenderCapability.Tier >> 16;
        if (ShouldForceSoftwareUi(
                e.Args,
                GetSystemMetrics(SmRemoteSession) != 0,
                renderingTier))
        {
            RenderOptions.ProcessRenderMode = RenderMode.SoftwareOnly;
        }
        base.OnStartup(e);

        // 内部用のウィンドウを持たない WIC 分離プロセス。デコーダー子プロセスが
        // エディターウィンドウを作成したり通常のプロジェクト起動へ入ったりしないよう、
        // すべてのエディター／自己テスト分岐より先に処理する。
        if (AssetImageDecodeWorker.TryRun(
                e.Args,
                out int imageWorkerExitCode))
        {
            Shutdown(imageWorkerExitCode);
            return;
        }

        // Headless package worker modes are used by the responsiveness harness
        // to exercise bounded output capture and process-tree cancellation.
        if (e.Args.Length >= 1 &&
            e.Args[0] == "--package-process-output-worker")
        {
            Shutdown(PackageResponsivenessSelfTest.RunOutputWorker());
            return;
        }
        if (e.Args.Length >= 1 &&
            e.Args[0] == "--package-process-wait-worker")
        {
            Shutdown(PackageResponsivenessSelfTest.RunWaitWorker());
            return;
        }
        if (e.Args.Length >= 1 &&
            e.Args[0] == "--package-responsiveness-selftest")
        {
            int failures = PackageResponsivenessSelfTest.Run(Console.Error);
            Shutdown(failures);
            return;
        }
        if (e.Args.Length >= 1 &&
            e.Args[0] == "--package-metadata-editor-selftest")
        {
            Shutdown(PackageMetadataEditorSelfTest.Run(Console.Error));
            return;
        }
        if (e.Args.Length >= 1 &&
            e.Args[0] == "--asset-package-readiness-selftest")
        {
            Shutdown(AssetPackageReadinessSelfTest.Run(Console.Error));
            return;
        }
        if (e.Args.Length >= 2 &&
            e.Args[0] == "--asset-package-readiness-visual-fixture")
        {
            Shutdown(AssetPackageReadinessVisualFixture.Run(
                e.Args[1],
                Console.Error));
            return;
        }
        if (e.Args.Length >= 2 &&
            e.Args[0] == "--package-metadata-editor-visual-fixture")
        {
            Shutdown(PackageMetadataEditorVisualFixture.Run(
                e.Args[1],
                Console.Error));
            return;
        }

        // CLI: --project-launcher-responsiveness-selftest -> dispatcher-free
        // recent-project probes, serialized open/create, and stale publication
        // rejection after launcher lifetime changes.
        if (e.Args.Length >= 1 &&
            e.Args[0] == "--project-launcher-responsiveness-selftest")
        {
            int failures =
                ProjectLauncherResponsivenessSelfTest.Run(Console.Error);
            Shutdown(failures);
            return;
        }

        // CLI: --operation-diagnostics-selftest -> managed Build/Package
        // operation identity, typed diagnostics, aggregation and cancellation.
        if (e.Args.Length >= 1 &&
            e.Args[0] == "--operation-diagnostics-selftest")
        {
            int failures =
                EditorOperationDiagnosticsSelfTest.Run(Console.Error);
            Shutdown(failures);
            return;
        }

        // CLI: --abi-contract-selftest -> version/capability negotiation and
        // fail-closed diagnostics without loading the native DLL.
        if (e.Args.Length >= 1 && e.Args[0] == "--abi-contract-selftest")
        {
            Shutdown(EditorAbiContractSelfTest.Run(Console.Error));
            return;
        }

        // CLI: --autosave-selftest  → atomic recovery store / checksum / retention / safety harness.
        // This path creates no WPF window and is safe in build/CI environments.
        if (e.Args.Length >= 1 && e.Args[0] == "--autosave-selftest")
        {
            int failures = SceneAutosaveSelfTest.Run(Console.Error);
            Shutdown(failures);
            return;
        }

        // CLI: --scene-save-selftest → Save All planning + atomic source write safety.
        if (e.Args.Length >= 1 && e.Args[0] == "--scene-save-selftest")
        {
            int failures = SceneSaveSelfTest.Run(Console.Error);
            Shutdown(failures);
            return;
        }

        // CLI: --document-host-selftest -> common identity/dirty/save/transaction contract.
        if (e.Args.Length >= 1 && e.Args[0] == "--document-host-selftest")
        {
            int failures = EditorDocumentHostSelfTest.Run(Console.Error);
            failures += MaterialDocumentHostSelfTest.Run(Console.Error);
            failures += ProjectSettingsDocumentSelfTest.Run(Console.Error);
            Shutdown(failures);
            return;
        }

        // CLI: --project-settings-selftest -> bounded/project-contained load + hosted durability.
        if (e.Args.Length >= 1 &&
            e.Args[0] == "--project-settings-selftest")
        {
            Shutdown(ProjectSettingsDocumentSelfTest.Run(Console.Error));
            return;
        }

        // CLI: --scene-editor-migration-selftest -> stale labels + source/view isolation audit.
        if (e.Args.Length >= 1 &&
            e.Args[0] == "--scene-editor-migration-selftest")
        {
            int failures = SceneEditorMigrationSelfTest.Run(Console.Error);
            Shutdown(failures);
            return;
        }

        // CLI: --material-preview-selftest -> async/cancellation/cache/stale-result contract.
        if (e.Args.Length >= 1 && e.Args[0] == "--material-preview-selftest")
        {
            int failures = MaterialPreviewSelfTest.Run(Console.Error);
            Shutdown(failures);
            return;
        }

        // CLI: --material-workflow-selftest -> deterministic material catalogue / assignment
        // retention / collision-free asset naming plus the material preview scheduler contract.
        if (e.Args.Length >= 1 && e.Args[0] == "--material-workflow-selftest")
        {
            int failures = MaterialWorkflowSelfTest.Run(Console.Error);
            failures += MaterialPreviewSelfTest.Run(Console.Error);
            Shutdown(failures);
            return;
        }

        // CLI: --asset-creation-selftest -> collision-safe/reparse-safe Content Browser New
        // workflow, canonical template delegation, and immediate asset-index metadata.
        if (e.Args.Length >= 1 && e.Args[0] == "--asset-creation-selftest")
        {
            int failures = AssetCreationSelfTest.Run(Console.Error);
            Shutdown(failures);
            return;
        }

        // CLI: --asset-import-selftest -> クラッシュから復旧可能な Import/Reimport
        // ジャーナル、プロセス間ロック、キャンセル時ロールバック、パス安全性の契約を検証する。
        if (e.Args.Length >= 1 && e.Args[0] == "--asset-import-selftest")
        {
            int failures = AssetImportWorkflowSelfTest.Run(Console.Error);
            Shutdown(failures);
            return;
        }

        // CLI: --asset-browser-selftest -> UE-style query/history plus transactional
        // rename, duplicate, and delete behavior for files and folders.
        if (e.Args.Length >= 1 && e.Args[0] == "--asset-browser-selftest")
        {
            int failures = AssetBrowserViewStateSelfTest.Run(Console.Error);
            failures += AssetBrowserSourcesSelfTest.Run(Console.Error);
            failures += AssetCreationSelfTest.Run(Console.Error);
            failures += AssetManagementSelfTest.Run(Console.Error);
            failures += AssetImportWorkflowSelfTest.Run(Console.Error);
            failures += AssetTrashWorkflowSelfTest.Run(Console.Error);
            failures += AssetPathChangeSelfTest.Run(Console.Error);
            failures += AssetViewPresentationSelfTest.Run(Console.Error);
            failures += AssetImageWorkerSelfTest.Run(Console.Error);
            failures += ThumbnailDerivedDataCacheSelfTest.Run(Console.Error);
            failures += AssetBrowserUiSelfTest.Run(Console.Error);
            failures += AssetPackageReadinessSelfTest.Run(Console.Error);
            Shutdown(failures);
            return;
        }

        // CLI: --thumbnail-ddc-selftest -> persistent thumbnail hit/miss,
        // invalidation, corruption recovery, budgets, cancellation, and path safety.
        if (e.Args.Length >= 1 &&
            e.Args[0] == "--thumbnail-ddc-selftest")
        {
            Shutdown(ThumbnailDerivedDataCacheSelfTest.Run(Console.Error));
            return;
        }

        // CLI: --workspace-selftest -> named layout persistence / validation / atomicity.
        if (e.Args.Length >= 1 && e.Args[0] == "--workspace-selftest")
        {
            int failures = EditorWorkspaceSelfTest.Run(Console.Error);
            failures += ToolPanelDockingSelfTest.Run(Console.Error);
            Shutdown(failures);
            return;
        }

        // CLI: --camera-authoring-selftest -> authored camera identity,
        // projection bounds, deterministic designation, and exclusive activation.
        if (e.Args.Length >= 1 &&
            e.Args[0] == "--camera-authoring-selftest")
        {
            int failures = CameraAuthoringSelfTest.Run(Console.Error);
            Shutdown(failures);
            return;
        }

        // CLI: --profiler-selftest -> snapshot/history/labels and unattended input guards.
        if (e.Args.Length >= 1 && e.Args[0] == "--profiler-selftest")
        {
            int failures = EditorProfilerSelfTest.Run(Console.Error);
            Shutdown(failures);
            return;
        }

        // CLI: --editor-reliability-selftest -> pure interaction-health policy,
        // real off-screen modeless windows, and build-cancellation integration.
        if (e.Args.Length >= 1 &&
            e.Args[0] == "--editor-reliability-selftest")
        {
            ShutdownMode = ShutdownMode.OnExplicitShutdown;
            _ = Dispatcher.BeginInvoke(
                DispatcherPriority.Loaded,
                new Action(async () =>
                {
                    int failures;
                    try
                    {
                        failures = await EditorReliabilitySelfTest.RunAsync(
                            Console.Error);
                    }
                    catch (Exception error)
                    {
                        Console.Error.WriteLine(error);
                        failures = 1;
                    }
                    Shutdown(failures);
                }));
            return;
        }

        // CLI: --interaction-soak-runner <project> [seconds] [report]
        //      --interaction-soak-runner-normal <project> [seconds] [report]
        // Spawns a real, non-activating editor child and enforces an external
        // wall-clock deadline, so a blocked child Dispatcher cannot report a
        // false pass merely because its own timer stopped.
        bool interactionSoakRunner = e.Args.Length >= 2 &&
            (e.Args[0] == "--interaction-soak-runner" ||
             e.Args[0] == "--interaction-soak-runner-normal");
        if (interactionSoakRunner)
        {
            bool unattendedRunner =
                e.Args[0] == "--interaction-soak-runner";
            ShutdownMode = ShutdownMode.OnExplicitShutdown;
            _ = Dispatcher.BeginInvoke(
                DispatcherPriority.Loaded,
                new Action(async () =>
                {
                    double seconds = 10;
                    if (e.Args.Length >= 3 &&
                        (!double.TryParse(
                            e.Args[2],
                            NumberStyles.Float,
                            CultureInfo.InvariantCulture,
                            out seconds) ||
                         !double.IsFinite(seconds)))
                    {
                        Console.Error.WriteLine(
                            "interaction soak runner seconds are invalid.");
                        Shutdown(2);
                        return;
                    }
                    string report = e.Args.Length >= 4
                        ? e.Args[3]
                        : Path.Combine(
                            Path.GetTempPath(),
                            $"acs-editor-interaction-soak-runner-{Environment.ProcessId}.json");
                    int exitCode;
                    try
                    {
                        string? executable = Environment.ProcessPath;
                        exitCode = executable == null
                            ? 2
                            : await EditorReliabilitySoakRunner.RunAsync(
                                executable,
                                e.Args[1],
                                seconds,
                                report,
                                unattendedRunner,
                                Console.Error);
                        if (executable == null)
                        {
                            Console.Error.WriteLine(
                                "Current editor executable path is unavailable.");
                        }
                    }
                    catch (Exception error)
                    {
                        Console.Error.WriteLine(
                            "Interaction soak runner failed: " + error);
                        exitCode = 1;
                    }
                    Shutdown(exitCode);
                }));
            return;
        }

        // CLI: --profilershot <out.png> -> render populated profiler state for visual QA.
        if (e.Args.Length >= 2 && e.Args[0] == "--profilershot")
        {
            ProfilerVisualFixture.Capture(
                e.Args[1],
                exitCode => Shutdown(exitCode));
            return;
        }

        // CLI: --workspaceshot <out.png> -> render named-workspace management for visual QA.
        if (e.Args.Length >= 2 && e.Args[0] == "--workspaceshot")
        {
            string outPng = e.Args[1];
            string testRoot = Path.Combine(
                Path.GetTempPath(),
                "acs-workspaceshot-" + Guid.NewGuid().ToString("N"));
            var store = new EditorWorkspaceStore(Path.Combine(testRoot, "workspaces.json"));
            store.SaveUserProfile(
                "Cinematics",
                new EditorWorkspaceLayout
                {
                    HierarchyWidth = 290,
                    InspectorWidth = 390,
                    BottomDockHeight = 280,
                    BottomTab = "assets",
                },
                overwrite: false);
            var win = new WorkspaceManagerWindow(store, new EditorWorkspaceLayout())
            {
                WindowStartupLocation = WindowStartupLocation.Manual,
                Left = -4000,
                Top = -4000,
            };
            win.Show();
            _ = win.Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() =>
            {
                int exitCode = 0;
                try
                {
                    win.UpdateLayout();
                    int width = (int)Math.Ceiling(win.ActualWidth);
                    int height = (int)Math.Ceiling(win.ActualHeight);
                    var bitmap = new RenderTargetBitmap(
                        width,
                        height,
                        96,
                        96,
                        PixelFormats.Pbgra32);
                    bitmap.Render(win);
                    var encoder = new PngBitmapEncoder();
                    encoder.Frames.Add(BitmapFrame.Create(bitmap));
                    using var stream = File.Create(outPng);
                    encoder.Save(stream);
                    Console.Error.WriteLine(
                        $"workspaceshot saved: {outPng} ({width}x{height})");
                }
                catch (Exception ex)
                {
                    Console.Error.WriteLine(ex.Message);
                    exitCode = 1;
                }
                finally
                {
                    try
                    {
                        if (Directory.Exists(testRoot))
                            Directory.Delete(testRoot, recursive: true);
                    }
                    catch
                    {
                    }
                }
                Shutdown(exitCode);
            }));
            return;
        }

        // CLI: --new <name> <parentDir> <template>  → プロジェクトを生成して即終了 (スクリプト/テスト用)。
        if (e.Args.Length >= 4 && e.Args[0] == "--new")
        {
            try { ProjectManager.CreateNew(e.Args[1], e.Args[2], e.Args[3]); }
            catch (Exception ex) { Console.Error.WriteLine(ex.Message); }
            Shutdown();
            return;
        }

        // CLI: --paletteshot <out.png> [query] -> render the command palette offscreen.
        if (e.Args.Length >= 2 && e.Args[0] == "--paletteshot")
        {
            string outPng = e.Args[1];
            string query = e.Args.Length >= 3 ? e.Args[2] : "package";
            var win = new EditorCommandPaletteWindow(EditorCommandPaletteWindow.CreateVisualTestCommands())
            {
                WindowStartupLocation = WindowStartupLocation.Manual,
                Left = -4000, Top = -4000,
            };
            win.Show();
            _ = win.Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() =>
            {
                int exitCode = 0;
                try
                {
                    win.SetQueryForTest(query);
                    win.UpdateLayout();
                    int w = (int)Math.Ceiling(win.ActualWidth);
                    int h = (int)Math.Ceiling(win.ActualHeight);
                    var rtb = new RenderTargetBitmap(w, h, 96, 96, PixelFormats.Pbgra32);
                    rtb.Render(win);
                    var enc = new PngBitmapEncoder();
                    enc.Frames.Add(BitmapFrame.Create(rtb));
                    using var fs = File.Create(outPng);
                    enc.Save(fs);
                    Console.Error.WriteLine($"paletteshot saved: {outPng} ({w}x{h})");
                    bool searchOk = EditorCommandPaletteWindow.RunSearchSelfTest();
                    Console.Error.WriteLine(searchOk
                        ? "palette search self-test: PASS"
                        : "palette search self-test: FAIL");
                    if (!searchOk) exitCode = 1;
                }
                catch (Exception ex)
                {
                    Console.Error.WriteLine(ex.Message);
                    exitCode = 1;
                }
                Shutdown(exitCode);
            }));
            return;
        }

        // CLI: --matshot <acsmat> <out.png>  → マテリアルエディタのパネルを画面外で描画し PNG 保存して終了。
        // GPU 不要 (engine=0、プレビューは CPU フォールバック)。UI レイアウト/パネル検証用。
        if (e.Args.Length >= 3 && e.Args[0] == "--matshot")
        {
            string acsmat = e.Args[1], outPng = e.Args[2];
            var win = new MaterialEditorWindow(IntPtr.Zero, acsmat)
            {
                WindowStartupLocation = WindowStartupLocation.Manual,
                Left = -4000, Top = -4000,   // 画面外で描く (チラつき回避)
                Height = 960,                // 全項目を ScrollViewer 内に収めて撮る
            };
            win.SuppressClosePromptForAutomation();
            if (e.Args.Length >= 4 && int.TryParse(e.Args[3], out int hOverride)) win.Height = hOverride;
            win.Show();
            // レイアウト確定後 (Loaded) に VisualTree をビットマップへ。
            _ = win.Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() =>
            {
                try
                {
                    string mode = e.Args.Length >= 5 ? e.Args[4] : "";
                    if (mode is "expr" or "exprsave" or "exprtype" or "exprcycle" or
                        "exproperator" or "exprdelete" or "exprdeleteroot" or "exprtexture")
                        win.BuildExpressionGraphForTest(mode == "exproperator");
                    if (mode == "exprsave")
                        win.SaveExpressionGraphForTest();
                    else if (mode == "exprtype")
                        win.TriggerExpressionConnectionErrorForTest(cycle: false);
                    else if (mode == "exprcycle")
                        win.TriggerExpressionConnectionErrorForTest(cycle: true);
                    else if (mode == "exprdelete")
                        win.DeleteExpressionForTest(3);
                    else if (mode == "exprdeleteroot")
                        win.DeleteExpressionForTest(8);
                    else if (mode == "exprtexture")
                        win.ConfigureSharedTextureForTest();
                    // RefreshPreview is intentionally debounced for interactive edits.
                    // A one-shot visual regression capture must render synchronously
                    // before this dispatcher callback snapshots and shuts down.
                    win.RenderPreviewImmediatelyForTest();
                    win.UpdateLayout();
                    int w = (int)Math.Ceiling(win.ActualWidth);
                    int h = (int)Math.Ceiling(win.ActualHeight);
                    var rtb = new RenderTargetBitmap(w, h, 96, 96, PixelFormats.Pbgra32);
                    rtb.Render(win);
                    var enc = new PngBitmapEncoder();
                    enc.Frames.Add(BitmapFrame.Create(rtb));
                    using var fs = File.Create(outPng);
                    enc.Save(fs);
                    Console.Error.WriteLine($"matshot saved: {outPng} ({w}x{h})");
                }
                catch (Exception ex) { Console.Error.WriteLine(ex.Message); }
                Shutdown();
            }));
            return;
        }

        // CLI: --colorshot <out.png>  -> render the reusable color picker for visual regression QA.
        if (e.Args.Length >= 2 && e.Args[0] == "--colorshot")
        {
            string outPng = e.Args[1];
            var win = new ColorPickerDialog(Color.FromArgb(196, 52, 132, 230))
            {
                WindowStartupLocation = WindowStartupLocation.Manual,
                Left = -4000, Top = -4000,
            };
            win.Show();
            _ = win.Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() =>
            {
                try
                {
                    win.UpdateLayout();
                    int w = (int)Math.Ceiling(win.ActualWidth);
                    int h = (int)Math.Ceiling(win.ActualHeight);
                    var rtb = new RenderTargetBitmap(w, h, 96, 96, PixelFormats.Pbgra32);
                    rtb.Render(win);
                    var enc = new PngBitmapEncoder();
                    enc.Frames.Add(BitmapFrame.Create(rtb));
                    using var fs = File.Create(outPng);
                    enc.Save(fs);
                    Console.Error.WriteLine($"colorshot saved: {outPng} ({w}x{h})");
                }
                catch (Exception ex) { Console.Error.WriteLine(ex.Message); }
                Shutdown();
            }));
            return;
        }

        // CLI: --bpshot <out.png> [acsbp]  → BlueprintWindow を画面外で描画し PNG 保存して終了。
        // GPU 不要 (WPF ソフト描画)。ノードグラフ/グリッド/型付きピン/コメント枠の UI 検証用。
        if (e.Args.Length >= 2 && e.Args[0] == "--bpshot")
        {
            string outPng = e.Args[1];
            string? bpPath = e.Args.Length >= 3 ? e.Args[2] : null;
            var win = new BlueprintWindow
            {
                WindowStartupLocation = WindowStartupLocation.Manual,
                Left = -4000, Top = -4000, Width = 1280, Height = 760,
            };
            win.Show();
            _ = win.Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() =>
            {
                try
                {
                    if (bpPath != null && File.Exists(bpPath)) win.Editor.LoadFromFile(bpPath);
                    string mode = e.Args.Length >= 4 ? e.Args[3] : "";
                    if (mode == "sel") win.Editor.SelectAll();                                  // 選択枠の描画検証
                    else if (mode == "validate") win.Editor.ValidateForTest();                  // ⚠ バッジ検証
                    else if (mode == "run") { win.Editor.ValidateForTest(); win.Editor.RunGraph(); }   // 実行=ウォッチ値+発火配線+バッジ
                    else if (mode == "gencpp") win.Editor.GenerateCppForTest();                 // C++ 生成
                    else if (mode == "func") win.Editor.SwitchToFirstFunctionForTest();         // 関数サブグラフへ切替
                    else if (mode == "viewport") win.Editor.ShowViewportForTest();               // ビューポート (見た目+当たり判定)
                    else if (mode == "vpangle") win.Editor.ViewportAngleForTest(2.4, 0.22);      // 別アングル (オービット確認)
                    else if (mode == "vptop") win.Editor.ViewportAngleForTest(0.0, 1.35);        // ほぼ真上
                    else if (mode == "varcombo") win.Editor.OpenVarComboForTest();                // 変数名コンボのドロップダウン
                    else if (mode == "collapse") win.Editor.CollapseForTest();                    // 関数に折りたたむ
                    else if (mode == "watch") win.Editor.WatchPinForTest();                        // ピン留めウォッチ
                    else if (mode == "funcio") win.Editor.FuncIOForTest();                          // 関数の入出力ピン編集
                    else if (mode == "macro") win.Editor.MacroForTest();                            // マクログラフへ切替
                    else if (mode == "override") win.Editor.OverrideForTest();                      // 親関数をオーバーライド
                    else if (mode == "split") win.Editor.SplitForTest();                            // 構造体ピン分割
                    else if (mode == "arrange") win.Editor.ArrangeForTest();                        // 自動整列
                    else if (mode == "expand") win.Editor.ExpandMathForTest();                      // 式をノード展開
                    else if (mode.StartsWith("vpnode")) win.Editor.HighlightViewportNode(int.TryParse(mode.Substring(6), out var vid) ? vid : 1);   // ビューポートでノード強調
                    else if (mode == "vpmove") win.Editor.MoveComponentForTest(1, 70, -30);   // ビューポートのドラッグ配置を検証 (Player を移動)
                    else if (mode == "promote") win.Editor.PromotePinForTest(3, false, 1);   // データピンを変数に昇格 (Compare.a)
                    else if (mode == "refs") win.Editor.SelectVariableReferences("HP");   // 変数の参照を全選択 (Find References)
                    else if (mode == "align") win.Editor.AlignGuideForTest(4, 2);   // ドラッグ整列ガイド
                    else if (mode == "autoconv") win.Editor.AutoConvertForTest();   // 型変換ノードの自動挿入
                    else if (mode.StartsWith("node")) win.Editor.SelectOneForTest(int.TryParse(mode.Substring(4), out var nid) ? nid : 0);   // 単一ノード選択 (Details 確認)
                    else if (mode == "wire") win.Editor.DebugStartWireForTest(2, 1);             // 互換ピン強調 (Object 出力から)
                    else if (mode.StartsWith("zoom")) win.Editor.DebugZoomForTest(double.TryParse(mode.Substring(4), out var zz) ? zz : 2.2);   // 拡大監査
                    else if (mode.StartsWith("step")) win.Editor.DebugStepForTest(int.TryParse(mode.Substring(4), out var st) ? st : 3);   // ステップ実行
                    win.UpdateLayout();
                    int w = (int)Math.Ceiling(win.ActualWidth);
                    int h = (int)Math.Ceiling(win.ActualHeight);
                    var rtb = new RenderTargetBitmap(w, h, 96, 96, PixelFormats.Pbgra32);
                    rtb.Render(win);
                    var enc = new PngBitmapEncoder();
                    enc.Frames.Add(BitmapFrame.Create(rtb));
                    using var fs = File.Create(outPng);
                    enc.Save(fs);
                    Console.Error.WriteLine($"bpshot saved: {outPng} ({w}x{h})");
                }
                catch (Exception ex) { Console.Error.WriteLine(ex.Message); }
                Shutdown();
            }));
            return;
        }

        // CLI: --bpcurve <acsbp>  → Timeline のカーブエディタを開いた BlueprintWindow を実画面に表示する (ポップアップ検証)。
        if (e.Args.Length >= 2 && e.Args[0] == "--bpcurve")
        {
            string path = e.Args[1];
            var win = new BlueprintWindow { WindowStartupLocation = WindowStartupLocation.CenterScreen, Width = 1280, Height = 760 };
            MainWindow = win; win.Show();
            _ = win.Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() =>
            {
                try { win.Editor.LoadFromFile(path); } catch { }
                win.Editor.OpenCurveForTest();
            }));
            return;
        }

        // CLI: --bpsearch  → 検索式ノードパレットを開いた状態の BlueprintWindow を実画面に表示する
        // (ポップアップは別 HWND のため画面外 RTB では写らない。PowerShell でスクリーンキャプチャして検証)。
        if (e.Args.Length >= 1 && e.Args[0] == "--bpsearch")
        {
            var win = new BlueprintWindow
            {
                WindowStartupLocation = WindowStartupLocation.CenterScreen, Width = 1280, Height = 760,
            };
            MainWindow = win;
            win.Show();
            _ = win.Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() =>
            {
                Color C(byte r, byte g, byte b) => Color.FromRgb(r, g, b);
                BlueprintEditor.BpPinSpec Ex(string n) => new(n, true);
                BlueprintEditor.BpPinSpec Da(string n, string ty = "") => new(n, false, ty);
                var none = Array.Empty<BlueprintEditor.BpPinSpec>();
                var pal = new System.Collections.Generic.List<BlueprintEditor.BpNodeTemplate>
                {
                    new("イベント", "On BeginPlay", C(0xB0,0x3A,0x46), none, new[]{ Ex("▶") }),
                    new("イベント", "On Tick",      C(0xB0,0x3A,0x46), none, new[]{ Ex("▶"), Da("dt","Float") }),
                    new("フロー", "Branch",   C(0x5A,0x64,0x72), new[]{ Ex("▶"), Da("cond","Bool") }, new[]{ Ex("True"), Ex("False") }),
                    new("フロー", "Sequence", C(0x5A,0x64,0x72), new[]{ Ex("▶") }, new[]{ Ex("0"), Ex("1") }),
                    new("シーン操作", "Set Position", C(0x8A,0x5C,0x2E), new[]{ Ex("▶"), Da("target","Object"), Da("x","Float"), Da("y","Float") }, new[]{ Ex("▶") }),
                    new("シーン操作", "Set Color",    C(0x8A,0x5C,0x2E), new[]{ Ex("▶"), Da("target","Object") }, new[]{ Ex("▶") }),
                    new("変数", "Get Variable", C(0x6A,0x4C,0x8C), new[]{ Da("name","String") }, new[]{ Da("value") }),
                    new("変数", "Set Variable", C(0x6A,0x4C,0x8C), new[]{ Ex("▶"), Da("name","String"), Da("value") }, new[]{ Ex("▶") }),
                };
                win.Editor.SetPalette(pal);
                win.Editor.DebugOpenSearch();
            }));
            return;
        }

        // CLI: --bpsrcgen <acsbp> <srcdir>  → SourceDir を設定して C++ を生成 (エンジン組み込みルーティングの検証)。
        if (e.Args.Length >= 3 && e.Args[0] == "--bpsrcgen")
        {
            string acsbp = e.Args[1], srcdir = e.Args[2];
            var win = new BlueprintWindow { WindowStartupLocation = WindowStartupLocation.Manual, Left = -4000, Top = -4000 };
            win.Show();
            _ = win.Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() =>
            {
                try
                {
                    System.IO.Directory.CreateDirectory(srcdir);
                    if (File.Exists(acsbp)) win.Editor.LoadFromFile(acsbp);
                    win.Editor.SourceDir = srcdir;
                    var cp = win.Editor.GenerateCppFile(build: false);
                    Console.Error.WriteLine("generated to: " + cp);
                }
                catch (Exception ex) { Console.Error.WriteLine(ex.Message); }
                Shutdown();
            }));
            return;
        }

        // CLI: --bptest [out.txt]  → Blueprint インタプリタ/直列化の自己テストを実行してログ出力 + 終了コード=失敗数。
        if (e.Args.Length >= 1 && e.Args[0] == "--bptest")
        {
            string? outPath = e.Args.Length >= 2 ? e.Args[1] : null;
            var win = new BlueprintWindow { WindowStartupLocation = WindowStartupLocation.Manual, Left = -4000, Top = -4000 };
            win.Show();
            _ = win.Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() =>
            {
                int fail = 1;
                try
                {
                    var (p, f, logText) = win.Editor.SelfTest();
                    fail = f;
                    Console.Error.WriteLine(logText);
                    if (outPath != null) File.WriteAllText(outPath, logText);
                }
                catch (Exception ex) { Console.Error.WriteLine("SelfTest 例外: " + ex); }
                Shutdown(fail);
            }));
            return;
        }

        // CLI: --codegen <acsproject>  → ACS_CLASS/ACS_PROPERTY からリフレクション登録を生成して終了。
        if (e.Args.Length >= 2 && e.Args[0] == "--codegen")
        {
            try { ReflectionCodegen.Generate(ProjectManager.Open(e.Args[1]), s => Console.Error.WriteLine(s)); }
            catch (Exception ex) { Console.Error.WriteLine(ex.Message); }
            Shutdown();
            return;
        }

        // 起動フロー: [1] (CLI に .acsproject があれば直接開く / 無ければランチャー表示) →
        // [2] 新規/既存を選択 → [3] MainWindow(project) を生成し初期シーンをロード。
        // ランチャーを閉じてから MainWindow を開くまでウィンドウが 0 個になるため、
        // その間は OnExplicitShutdown にして勝手に終了しないようにする。
        ShutdownMode = ShutdownMode.OnExplicitShutdown;

        Project? chosen = null;
        bool unattended = ShouldRunUnattended(e.Args);
        bool avoidInitialActivation = ShouldAvoidInitialActivation(e.Args);
        if (!EditorStartupMonitorPlacement.TryParse(
                e.Args,
                out EditorStartupMonitorSelector? startupMonitor,
                out string? startupMonitorError))
        {
            Console.Error.WriteLine(startupMonitorError);
            Shutdown(2);
            return;
        }
        bool showProfiler = e.Args.Contains(
            "--show-profiler", StringComparer.OrdinalIgnoreCase);
        int interactionSoakArgument = Array.FindIndex(
            e.Args,
            argument => string.Equals(
                argument,
                "--interaction-soak",
                StringComparison.OrdinalIgnoreCase));
        TimeSpan? interactionSoakDuration = null;
        string? interactionSoakReport = null;
        string? profilerCapturePath = null;
        bool interactionSoakRequiresRecovery = false;
        if (interactionSoakArgument >= 0)
        {
            double seconds = 10;
            if (interactionSoakArgument + 1 < e.Args.Length &&
                !e.Args[interactionSoakArgument + 1].StartsWith(
                    "--",
                    StringComparison.Ordinal))
            {
                if (!double.TryParse(
                        e.Args[interactionSoakArgument + 1],
                        NumberStyles.Float,
                        CultureInfo.InvariantCulture,
                        out seconds) ||
                    !double.IsFinite(seconds) ||
                    seconds < 2 ||
                    seconds > 600)
                {
                    Console.Error.WriteLine(
                        "--interaction-soak seconds must be between 2 and 600.");
                    Shutdown(2);
                    return;
                }
            }
            interactionSoakDuration = TimeSpan.FromSeconds(seconds);
            showProfiler = true;
            IsInactiveRecoveryPromptAllowed =
                !unattended && e.Args.Contains(
                    "--interaction-soak-allow-recovery",
                    StringComparer.OrdinalIgnoreCase);
            interactionSoakRequiresRecovery =
                !unattended && e.Args.Contains(
                    "--interaction-soak-require-recovery",
                    StringComparer.OrdinalIgnoreCase);
            if (interactionSoakRequiresRecovery &&
                !IsInactiveRecoveryPromptAllowed)
            {
                Console.Error.WriteLine(
                    "--interaction-soak-require-recovery also requires " +
                    "--interaction-soak-allow-recovery.");
                Shutdown(2);
                return;
            }

            int reportArgument = Array.FindIndex(
                e.Args,
                argument => string.Equals(
                    argument,
                    "--interaction-soak-report",
                    StringComparison.OrdinalIgnoreCase));
            if (reportArgument >= 0)
            {
                if (reportArgument + 1 >= e.Args.Length ||
                    e.Args[reportArgument + 1].StartsWith(
                        "--",
                        StringComparison.Ordinal))
                {
                    Console.Error.WriteLine(
                        "--interaction-soak-report requires an output path.");
                    Shutdown(2);
                    return;
                }
                interactionSoakReport = e.Args[reportArgument + 1];
            }
        }
        int profilerCaptureArgument = Array.FindIndex(
            e.Args,
            argument => string.Equals(
                argument,
                "--profiler-capture",
                StringComparison.OrdinalIgnoreCase));
        if (profilerCaptureArgument >= 0)
        {
            if (interactionSoakDuration == null)
            {
                Console.Error.WriteLine(
                    "--profiler-capture requires --interaction-soak.");
                Shutdown(2);
                return;
            }
            if (e.Args.Count(argument => string.Equals(
                    argument,
                    "--profiler-capture",
                    StringComparison.OrdinalIgnoreCase)) != 1)
            {
                Console.Error.WriteLine(
                    "--profiler-capture may be specified only once.");
                Shutdown(2);
                return;
            }
            if (profilerCaptureArgument + 1 >= e.Args.Length ||
                e.Args[profilerCaptureArgument + 1].StartsWith(
                    "--",
                    StringComparison.Ordinal))
            {
                Console.Error.WriteLine(
                    "--profiler-capture requires an explicit TEMP CSV path.");
                Shutdown(2);
                return;
            }
            if (!EditorProfilerCaptureFile.TryNormalizeAutomationDestination(
                    e.Args[profilerCaptureArgument + 1],
                    out profilerCapturePath,
                    out string? profilerCaptureError))
            {
                Console.Error.WriteLine(profilerCaptureError);
                Shutdown(2);
                return;
            }
            showProfiler = true;
        }
        IsNonInteractiveLaunch = unattended;
        IsInitialActivationSuppressed =
            ShouldDeferInteractivePromptsUntilActivation(e.Args);

        // CLI / ファイル関連付けで .acsproject を渡されたらランチャーを飛ばして直接開く。
        string? cliProject = e.Args.FirstOrDefault(
            a => a.EndsWith(".acsproject", StringComparison.OrdinalIgnoreCase));
        if (cliProject != null)
        {
            // Project.Open performs transaction reconciliation and asset-database I/O. Yield the
            // startup dispatcher before that work so Windows can continue pumping activation and
            // paint messages instead of reporting a hung editor.
            try
            {
                chosen = await System.Threading.Tasks.Task.Run(
                    () => ProjectManager.Open(cliProject));
            }
            catch (Exception ex) { Console.Error.WriteLine(ex.Message); }
        }

        if (chosen == null)
        {
            var launcher = new ProjectLauncher();
            launcher.ShowActivated = !avoidInitialActivation;
            launcher.IsHitTestVisible = !unattended;
            if (startupMonitor is { } launcherMonitor)
            {
                launcher.WindowStartupLocation = WindowStartupLocation.Manual;
                launcher.SourceInitialized += (_, _) =>
                {
                    if (!EditorStartupMonitorPlacement.TryApply(
                            launcher,
                            launcherMonitor,
                            out string? placementError))
                    {
                        Console.Error.WriteLine(placementError);
                    }
                };
            }
            if (unattended)
                launcher.SourceInitialized += (_, _) =>
                    SetNoActivateStyle(launcher, enabled: true);
            launcher.ShowDialog();
            chosen = launcher.SelectedProject;
        }

        if (chosen != null)
        {
            var win = new MainWindow(chosen);
            if (startupMonitor is { } editorMonitor)
            {
                win.SuppressSavedWindowPlacementForStartupMonitor();
                win.SourceInitialized += (_, _) =>
                {
                    if (!EditorStartupMonitorPlacement.TryApply(
                            win,
                            editorMonitor,
                            out string? placementError))
                    {
                        Console.Error.WriteLine(placementError);
                    }
                };
            }
            if (showProfiler)
                win.ShowProfilerAtStartup();
            if (e.Args.Contains("--hide-grid", StringComparer.OrdinalIgnoreCase))
                win.SetStartupGridVisible(false);
            if (interactionSoakDuration is { } soakDuration)
            {
                win.ConfigureInteractionSoak(
                    soakDuration,
                    interactionSoakReport,
                    exitCode => Shutdown(exitCode),
                    interactionSoakRequiresRecovery,
                    profilerCapturePath);
            }
            // Visual validation and secondary-monitor launches may need the
            // editor to become visible without interrupting the foreground
            // application.  WPF's Show() otherwise overrides the process
            // STARTUPINFO show state and activates the new top-level window.
            win.ShowActivated = !avoidInitialActivation;
            // Do not disable hit testing on the Window itself: a native
            // HwndHost/flip-model child must remain on WPF's normal visual
            // path or the swapchain can present only its clear surface.
            // Unattended input is rejected by the top-level and child HWND
            // message gates below instead.
            // ShowActivated=false only controls the Show() call. Renderer and
            // HwndHost startup continues for several seconds and may otherwise
            // activate the parent later. Keep the native no-activate style
            // until MainWindow observes the user's first real mouse click.
            if (unattended || IsInitialActivationSuppressed)
                win.SourceInitialized += (_, _) =>
                    SetNoActivateStyle(win, enabled: true);
            int cameraArg = Array.IndexOf(e.Args, "--camera3d");
            if (cameraArg >= 0)
            {
                if (cameraArg + 6 >= e.Args.Length)
                {
                    Console.Error.WriteLine(
                        "--camera3d requires: yaw pitch distance targetX targetY targetZ");
                }
                else
                {
                    var values = new float[6];
                    bool valid = true;
                    for (int i = 0; i < values.Length; ++i)
                    {
                        bool parsed = float.TryParse(
                            e.Args[cameraArg + 1 + i],
                            NumberStyles.Float,
                            CultureInfo.InvariantCulture,
                            out values[i]);
                        valid &= parsed && float.IsFinite(values[i]);
                    }
                    if (valid)
                    {
                        win.SetStartupCamera3D(
                            values[0], values[1], values[2],
                            values[3], values[4], values[5]);
                        win.RequireView3DForProfilerCapture();
                    }
                    else
                    {
                        Console.Error.WriteLine(
                            "--camera3d values must be finite invariant-culture numbers");
                    }
                }
            }
            MainWindow = win;
            ShutdownMode = ShutdownMode.OnMainWindowClose;
            win.Show();
        }
        else
        {
            Shutdown();
        }
    }
}
