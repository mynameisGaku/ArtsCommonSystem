using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Threading;
using System.Threading.Tasks;

namespace AcsEditor;

internal sealed record BuildInitialSceneSnapshot(
    string ProjectFilePath,
    string InitialScene,
    string CanonicalSceneAssetId,
    string AuthoritativeAssetId);

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
        string? environmentRoot = Environment.GetEnvironmentVariable(
            "ACS_ENGINE_ROOT");
        string?[] starts =
        [
            environmentRoot,
            AppContext.BaseDirectory,
            Environment.CurrentDirectory,
        ];
        var visited = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (string? start in starts)
        {
            if (string.IsNullOrWhiteSpace(start)) continue;
            DirectoryInfo? directory;
            try { directory = new DirectoryInfo(Path.GetFullPath(start)); }
            catch { continue; }

            while (directory != null)
            {
                if (!visited.Add(directory.FullName))
                {
                    directory = directory.Parent;
                    continue;
                }
                if (File.Exists(Path.Combine(
                        directory.FullName,
                        "engine",
                        "CMakeLists.txt")))
                {
                    return directory.FullName;
                }
                if (string.Equals(
                        directory.Name,
                        "engine",
                        StringComparison.OrdinalIgnoreCase) &&
                    File.Exists(Path.Combine(
                        directory.FullName,
                        "CMakeLists.txt")))
                {
                    return directory.Parent?.FullName;
                }
                directory = directory.Parent;
            }
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
    public static Task<string?> BuildAsync(
        Project project,
        Action<string> log,
        bool forceConfigure = false,
        bool standalone = false,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(project);
        ArgumentNullException.ThrowIfNull(log);

        // ビルド準備では、最初にプロセスを await する前に、アセット ID の全件走査、
        // リフレクション用ソースの解析、CMake キャッシュの I/O を行う。可変な UI モデルは
        // O(1) でスナップショット化し、大規模プロジェクトでもディスパッチャーを止めないよう処理全体をワーカーへ移す。
        var snapshot = new Project
        {
            Version = project.Version,
            Name = project.Name,
            EngineVersion = project.EngineVersion,
            Template = project.Template,
            InitialScene = project.InitialScene,
            CanonicalSceneAssetId = project.CanonicalSceneAssetId,
            ProjectFilePath = project.ProjectFilePath,
        };
        return Task.Run(
            () => BuildCoreAsync(
                snapshot,
                log,
                forceConfigure,
                standalone,
                cancellationToken),
            cancellationToken);
    }

    private static async Task<string?> BuildCoreAsync(
        Project project,
        Action<string> log,
        bool forceConfigure,
        bool standalone,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (ProjectManager.HasPendingInitialScenePathFollow(project))
        {
            log(
                "[INITIAL_SCENE_MOVE_PENDING] Build is blocked until the interrupted " +
                "initial-scene move is reconciled.");
            return null;
        }
        BuildInitialSceneSnapshot? initialSceneSnapshot = null;
        if (standalone)
        {
            try
            {
                initialSceneSnapshot = CaptureInitialSceneSnapshot(project);
            }
            catch (Exception error) when (
                error is IOException or UnauthorizedAccessException or
                    InvalidDataException or ArgumentException or
                    System.Text.Json.JsonException or
                    KeyNotFoundException or NotSupportedException)
            {
                log(
                    "[INITIAL_SCENE_STATE_UNSTABLE] Build is blocked because the initial " +
                    "scene snapshot could not be proven: " + error.Message);
                return null;
            }
        }
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
        cancellationToken.ThrowIfCancellationRequested();

        if (forceConfigure || !Directory.Exists(buildDir) || !CacheHasExtDir(buildDir, srcDir))
        {
            log($"Configuring… (-DACS_EXTERNAL_PROJECT_DIR={srcDir})");
            int rc = await RunAsync("cmake",
                $"-S \"{engineCMake}\" -B \"{buildDir}\" -DACS_EXTERNAL_PROJECT_DIR=\"{srcDir.Replace('\\', '/')}\"",
                engineRoot, log, cancellationToken);
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
                engineRoot, log, cancellationToken);
            if (rdll != 0) { log("Build 失敗 (exit " + rdll + ")。"); return null; }
            string dll = ReflectDllPath(project);
            if (File.Exists(dll)) { log($"Build 成功 → {dll}"); return dll; }
            log("Build は完了しましたが reflect DLL が見つかりません: " + dll);
            return null;
        }

        // スタンドアロン (出荷用): exe + reflect を依存込みでフルビルドし、シーンを exe 隣へコピー。
        log($"Building standalone {ident} + {ident}_reflect (Release)…");
        int br = await RunAsync("cmake",
            $"--build \"{buildDir}\" --target {ident} --target {ident}_reflect --config Release",
            engineRoot,
            log,
            cancellationToken);
        if (br != 0) { log("Build 失敗 (exit " + br + ")。"); return null; }

        string exe = ExePath(project);
        if (File.Exists(exe))
        {
            cancellationToken.ThrowIfCancellationRequested();
            // エディタで編集したシーンを exe の隣へコピー (スタンドアロンが起動時 main.acscene を読む)。
            try
            {
                string sceneDst = Path.Combine(Path.GetDirectoryName(exe)!, "main.acscene");
                CopyInitialSceneForBuild(
                    project,
                    initialSceneSnapshot ??
                        throw new InvalidOperationException(
                            "The standalone build has no initial-scene snapshot."),
                    sceneDst);
                log("シーンを exe の隣へ配置しました。");
            }
            catch (Exception ex)
            {
                log("シーン配置に失敗したため Standalone build を中止します: " + ex.Message);
                return null;
            }
            log($"Build 成功 → {exe}");
            return exe;
        }
        log("Build は完了しましたが exe が見つかりません: " + exe);
        return null;
    }

    internal static BuildInitialSceneSnapshot CaptureInitialSceneSnapshot(Project project)
    {
        ArgumentNullException.ThrowIfNull(project);
        using AssetMutationLock mutationLock = AssetMutationLock.AcquireFailFast(
            project.AssetsDir,
            "Capture build initial scene");
        if (ProjectManager.HasPendingInitialScenePathFollow(project))
        {
            throw new InvalidDataException(
                "An interrupted initial-scene move still requires recovery.");
        }

        Project persisted = ProjectManager.ReadManifest(project.ProjectFilePath);
        if (!string.Equals(
                persisted.InitialScene,
                project.InitialScene,
                StringComparison.OrdinalIgnoreCase) ||
            !string.Equals(
                persisted.CanonicalSceneAssetId,
                project.CanonicalSceneAssetId,
                StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                "The in-memory project no longer matches its manifest.");
        }
        string authoritativeAssetId =
            ProjectManager.ValidateInitialSceneAssetIdentity(persisted);
        return new BuildInitialSceneSnapshot(
            persisted.ProjectFilePath,
            persisted.InitialScene,
            persisted.CanonicalSceneAssetId,
            authoritativeAssetId);
    }

    internal static void CopyInitialSceneForBuild(
        Project project,
        BuildInitialSceneSnapshot snapshot,
        string destination)
    {
        ArgumentNullException.ThrowIfNull(project);
        ArgumentNullException.ThrowIfNull(snapshot);
        using AssetMutationLock mutationLock = AssetMutationLock.AcquireFailFast(
            project.AssetsDir,
            "Publish build initial scene");
        if (ProjectManager.HasPendingInitialScenePathFollow(project))
        {
            throw new InvalidDataException(
                "An initial-scene move began while the standalone build was compiling.");
        }

        Project persisted = ProjectManager.ReadManifest(snapshot.ProjectFilePath);
        if (!SceneSourceFile.PathsEqual(
                persisted.ProjectFilePath,
                snapshot.ProjectFilePath) ||
            !string.Equals(
                persisted.InitialScene,
                snapshot.InitialScene,
                StringComparison.OrdinalIgnoreCase) ||
            !string.Equals(
                persisted.CanonicalSceneAssetId,
                snapshot.CanonicalSceneAssetId,
                StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                "The project manifest changed while the standalone build was compiling.");
        }
        string authoritativeAssetId =
            ProjectManager.ValidateInitialSceneAssetIdentity(persisted);
        if (!string.Equals(
                authoritativeAssetId,
                snapshot.AuthoritativeAssetId,
                StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "The initial scene identity changed while the standalone build was compiling.");
        }

        string destinationRoot = Path.GetDirectoryName(Path.GetFullPath(destination))
            ?? throw new InvalidDataException(
                "The standalone scene destination has no parent directory.");
        SceneSourceFile.CopyAtomic(
            persisted.InitialScenePath,
            destination,
            destinationRoot);
    }

    /// <summary>ビルド済み exe を起動する (無ければ null)。</summary>
    public static Process? Run(Project project, Action<string> log)
    {
        if (ProjectManager.HasPendingInitialScenePathFollow(project))
        {
            log(
                "[INITIAL_SCENE_MOVE_PENDING] Run is blocked until the interrupted " +
                "initial-scene move is reconciled.");
            return null;
        }
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

    private static async Task<int> RunAsync(
        string file,
        string args,
        string cwd,
        Action<string> log,
        CancellationToken cancellationToken)
    {
        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = file, Arguments = args, WorkingDirectory = cwd,
                UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true,
                StandardOutputEncoding = OutEncoding, StandardErrorEncoding = OutEncoding,
            };
            PackageProcessResult process = await PackageProcessRunner.RunAsync(
                psi,
                log,
                cancellationToken);
            return process.ExitCode;
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (Exception ex)
        {
            log("プロセス起動失敗: " + ex.Message);
            return -1;
        }
    }

    internal static Task<int> RunProcessForReliabilitySelfTestAsync(
        string file,
        string arguments,
        CancellationToken cancellationToken) =>
        RunAsync(
            file,
            arguments,
            AppContext.BaseDirectory,
            _ => { },
            cancellationToken);

}
