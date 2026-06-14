using System;
using System.Diagnostics;
using System.IO;
using System.Threading.Tasks;

namespace AcsEditor;

/// <summary>
/// プロジェクトの Source/ を ACS エンジンビルドに取り込んでスタンドアロンの .exe にビルドし、実行する。
/// エンジン CMake の ACS_EXTERNAL_PROJECT_DIR フックを使い、既存の静的 lib を再利用してインクリメンタルに作る。
/// </summary>
public static class BuildService
{
    // cmake/cl(VS2022+) はパイプへ UTF-8 で出力する。GUI アプリの既定 (OEM コードページ) で
    // 読むと UTF-8 バイトが cp932 として化けるので、明示的に UTF-8 でデコードする。
    private static readonly System.Text.Encoding OutEncoding =
        new System.Text.UTF8Encoding(encoderShouldEmitUTF8Identifier: false);

    /// <summary>editor 実行ファイルから上方向に engine/CMakeLists.txt を探してリポジトリルートを返す。</summary>
    public static string? FindEngineRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir != null)
        {
            if (File.Exists(Path.Combine(dir.FullName, "engine", "CMakeLists.txt"))) return dir.FullName;
            dir = dir.Parent;
        }
        return null;
    }

    /// <summary>ビルド済み exe の想定パス (RootDir/Binaries/Release/&lt;ident&gt;.exe)。</summary>
    public static string ExePath(Project p) =>
        Path.Combine(p.RootDir, "Binaries", "Release", ProjectManager.SanitizeIdent(p.Name) + ".exe");

    /// <summary>ユーザー型公開リフレクション DLL のパス (RootDir/Binaries/Release/&lt;ident&gt;_reflect.dll)。</summary>
    public static string ReflectDllPath(Project p) =>
        Path.Combine(p.RootDir, "Binaries", "Release", ProjectManager.SanitizeIdent(p.Name) + "_reflect.dll");

    /// <summary>プロジェクトをビルドする。成功で exe パス、失敗で null。ログは log に流す。
    /// forceConfigure=true でソース追加/削除時に必ず再 configure する (glob 反映のため)。</summary>
    public static async Task<string?> BuildAsync(Project project, Action<string> log, bool forceConfigure = false, bool standalone = false)
    {
        string? engineRoot = FindEngineRoot();
        if (engineRoot == null) { log("エンジンルートが見つかりません (engine/CMakeLists.txt)。"); return null; }

        string engineCMake = Path.Combine(engineRoot, "engine");
        string buildDir = Path.Combine(engineRoot, "Intermediate", "vs");
        string srcDir = project.SourceDir;
        string ident = ProjectManager.SanitizeIdent(project.Name);

        // ビルド定義の自己修復: 旧版エディタ生成の陳腐化した CMakeLists (ACS_ENGINE_DIR 要求の
        // 古いスタブ等) を現在のテンプレートへ更新する。書き換えたら再 configure を強制 (glob/配線反映)。
        if (ProjectManager.EnsureBuildFiles(project)) { forceConfigure = true; log("ビルド定義 (Source/CMakeLists.txt) を更新しました。"); }

        // リフレクション・コードジェネレータ: ACS_CLASS / ACS_PROPERTY マーカー → 登録コード (.gen.cpp)。
        // 生成ファイルが新規ならソース集合が変わるので再 configure を強制する。
        try { if (ReflectionCodegen.Generate(project, log)) forceConfigure = true; }
        catch (Exception ex) { log("codegen 警告: " + ex.Message); }

        if (forceConfigure || !Directory.Exists(buildDir) || !CacheHasExtDir(buildDir, srcDir))
        {
            log($"Configuring… (-DACS_EXTERNAL_PROJECT_DIR={srcDir})");
            int rc = await RunAsync("cmake",
                $"-S \"{engineCMake}\" -B \"{buildDir}\" -DACS_EXTERNAL_PROJECT_DIR=\"{srcDir.Replace('\\', '/')}\"",
                engineRoot, log);
            if (rc != 0) { log("Configure 失敗 (exit " + rc + ")。"); return null; }
        }

        if (!standalone)
        {
            // 既定 (Game View / イテレーション): reflect DLL «だけ» を差分ビルド。Game View はこの DLL を
            // 使うので exe は不要。BuildProjectReferences=false で «エンジン依存の up-to-date 走査» を
            // スキップ → 変更したプロジェクトソースとその依存だけを再コンパイル (≈0.9s、UE 風の差分)。
            log($"Building {ident}_reflect (差分)…");
            int rdll = await RunAsync("cmake",
                $"--build \"{buildDir}\" --target {ident}_reflect --config Release -- /p:BuildProjectReferences=false",
                engineRoot, log);
            if (rdll != 0) { log("Build 失敗 (exit " + rdll + ")。"); return null; }
            string dll = ReflectDllPath(project);
            if (File.Exists(dll)) { log($"Build 成功 → {dll}"); return dll; }
            log("Build は完了しましたが reflect DLL が見つかりません: " + dll);
            return null;
        }

        // スタンドアロン (出荷用): exe + reflect を依存込みでフルビルドし、シーンを exe 隣へコピー。
        log($"Building standalone {ident} + {ident}_reflect (Release)…");
        int br = await RunAsync("cmake",
            $"--build \"{buildDir}\" --target {ident} --target {ident}_reflect --config Release", engineRoot, log);
        if (br != 0) { log("Build 失敗 (exit " + br + ")。"); return null; }

        string exe = ExePath(project);
        if (File.Exists(exe))
        {
            // エディタで編集したシーンを exe の隣へコピー (スタンドアロンが起動時 main.acscene を読む)。
            try
            {
                string sceneSrc = Path.Combine(project.RootDir, project.InitialScene);
                string sceneDst = Path.Combine(Path.GetDirectoryName(exe)!, "main.acscene");
                if (File.Exists(sceneSrc)) { File.Copy(sceneSrc, sceneDst, true); log("シーンを exe の隣へ配置しました。"); }
                else log("注意: シーンファイルが見つかりません (先に保存してください): " + sceneSrc);
            }
            catch (Exception ex) { log("シーンのコピー警告: " + ex.Message); }
            log($"Build 成功 → {exe}");
            return exe;
        }
        log("Build は完了しましたが exe が見つかりません: " + exe);
        return null;
    }

    /// <summary>ビルド済み exe を起動する (無ければ null)。</summary>
    public static Process? Run(Project project, Action<string> log)
    {
        string exe = ExePath(project);
        if (!File.Exists(exe)) { log("実行ファイルがありません。先に Build してください: " + exe); return null; }
        try
        {
            var p = Process.Start(new ProcessStartInfo
            {
                FileName = exe,
                WorkingDirectory = Path.GetDirectoryName(exe)!,
                UseShellExecute = true,
            });
            log("実行中: " + Path.GetFileName(exe));
            return p;
        }
        catch (Exception ex) { log("起動失敗: " + ex.Message); return null; }
    }

    // CMakeCache に同じ ACS_EXTERNAL_PROJECT_DIR が既に入っていれば configure を省ける。
    private static bool CacheHasExtDir(string buildDir, string srcDir)
    {
        try
        {
            string cache = Path.Combine(buildDir, "CMakeCache.txt");
            if (!File.Exists(cache)) return false;
            string want = srcDir.Replace('\\', '/');
            foreach (string line in File.ReadLines(cache))
            {
                if (!line.StartsWith("ACS_EXTERNAL_PROJECT_DIR")) continue;
                int eq = line.IndexOf('=');
                if (eq < 0) continue;
                string have = line.Substring(eq + 1).Trim().Replace('\\', '/');
                return string.Equals(have, want, StringComparison.OrdinalIgnoreCase);
            }
        }
        catch { /* キャッシュ読めなければ configure する */ }
        return false;
    }

    private static Task<int> RunAsync(string file, string args, string cwd, Action<string> log)
    {
        var tcs = new TaskCompletionSource<int>();
        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = file, Arguments = args, WorkingDirectory = cwd,
                UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true,
                StandardOutputEncoding = OutEncoding, StandardErrorEncoding = OutEncoding,
            };
            var p = new Process { StartInfo = psi, EnableRaisingEvents = true };
            p.OutputDataReceived += (_, e) => { if (e.Data != null) log(e.Data); };
            p.ErrorDataReceived  += (_, e) => { if (e.Data != null) log(e.Data); };
            p.Exited += (_, _) => { int code = p.ExitCode; p.Dispose(); tcs.TrySetResult(code); };
            p.Start();
            p.BeginOutputReadLine();
            p.BeginErrorReadLine();
        }
        catch (Exception ex) { log("プロセス起動失敗: " + ex.Message); tcs.TrySetResult(-1); }
        return tcs.Task;
    }
}
