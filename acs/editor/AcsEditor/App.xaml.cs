using System;
using System.IO;
using System.Linq;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace AcsEditor;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        // ヘッドレス / RDP / GPU コンテキスト不在の環境では WPF のハードウェア描画が
        // ブランク (白) になることがある。WPF をソフトウェア描画に固定しておく。
        // ビューポート (エンジン) は独自の DX12 デバイス/スワップチェインで GPU 描画する
        // ため、この設定の影響を受けない。
        RenderOptions.ProcessRenderMode = RenderMode.SoftwareOnly;
        base.OnStartup(e);

        // CLI: --new <name> <parentDir> <template>  → プロジェクトを生成して即終了 (スクリプト/テスト用)。
        if (e.Args.Length >= 4 && e.Args[0] == "--new")
        {
            try { ProjectManager.CreateNew(e.Args[1], e.Args[2], e.Args[3]); }
            catch (Exception ex) { Console.Error.WriteLine(ex.Message); }
            Shutdown();
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
            if (e.Args.Length >= 4 && int.TryParse(e.Args[3], out int hOverride)) win.Height = hOverride;
            win.Show();
            // レイアウト確定後 (Loaded) に VisualTree をビットマップへ。
            win.Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() =>
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
                    Console.Error.WriteLine($"matshot saved: {outPng} ({w}x{h})");
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
            win.Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() =>
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
            win.Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() =>
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
            win.Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() =>
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
            win.Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() =>
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
            win.Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() =>
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

        // CLI / ファイル関連付けで .acsproject を渡されたらランチャーを飛ばして直接開く。
        string? cliProject = e.Args.FirstOrDefault(
            a => a.EndsWith(".acsproject", StringComparison.OrdinalIgnoreCase) && File.Exists(a));
        if (cliProject != null)
        {
            try { chosen = ProjectManager.Open(cliProject); }
            catch (Exception ex) { Console.Error.WriteLine(ex.Message); }
        }

        if (chosen == null)
        {
            var launcher = new ProjectLauncher();
            launcher.ShowDialog();
            chosen = launcher.SelectedProject;
        }

        if (chosen != null)
        {
            var win = new MainWindow(chosen);
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
