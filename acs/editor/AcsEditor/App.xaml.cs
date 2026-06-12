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
