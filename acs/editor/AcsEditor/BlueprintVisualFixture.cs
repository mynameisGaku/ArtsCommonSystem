// SPDX-License-Identifier: Apache-2.0

using System;
using System.Globalization;
using System.IO;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace AcsEditor;

/// <summary>Blueprint 診断 CLI の入力、明示グラフ、ウィンドウ寿命、出力と終了コードを管理する。</summary>
internal static class BlueprintVisualFixture
{
    // ファイルを読まず、診断専用グラフを明示的に選ぶ入力値。
    private const string ExplicitFixtureArgument = "--fixture";

    // グラフ表示と基本操作に必要なノードだけを持つ診断データ。
    private const string ScreenshotGraph =
        "ACSBP 1\n" +
        "N 1 60 90 B03A46 Event  On BeginPlay\n" +
        "O E ▶\n" +
        "N 2 330 70 2E5C8A Spawn Prefab\n" +
        "I E ▶\n" +
        "I D:String path\n" +
        "I D:Vector pos\n" +
        "O E ▶\n" +
        "O D:Object spawned\n" +
        "N 3 620 110 2E5C8A Set Position\n" +
        "I E ▶\n" +
        "I D:Object target\n" +
        "I D:Float x\n" +
        "I D:Float y\n" +
        "O E ▶\n" +
        "N 4 620 270 357A55 Publish  \"Spawned\"\n" +
        "I E ▶\n" +
        "O E ▶\n" +
        "C 1 0 2 0\n" +
        "C 2 0 3 0\n" +
        "C 2 1 3 1\n" +
        "C 3 0 4 0\n";

    // カーブエディターを開くための Timeline を持つ診断データ。
    private const string CurveGraph =
        "ACSBP 1\n" +
        "N 1 60 90 B03A46 Event  On BeginPlay\n" +
        "O E ▶\n" +
        "N 2 330 90 356E7A Timeline\n" +
        "I E Play\n" +
        "I E Stop\n" +
        "I D:Float duration\n" +
        "O E Update\n" +
        "O E Finished\n" +
        "O D:Float value\n" +
        "V 2 1.0\n" +
        "VR 0:0,1:1\n" +
        "C 1 0 2 0\n";

    /// <summary>
    /// Blueprint 診断 CLI を識別して開始する。未対応の command なら false、
    /// 必須入力が無い場合はウィンドウを作らず終了コード 2 を返す。
    /// </summary>
    internal static bool TryStart(
        string[] arguments,
        TextWriter output,
        TextWriter error,
        Action<int> shutdown)
    {
        ArgumentNullException.ThrowIfNull(arguments);
        ArgumentNullException.ThrowIfNull(output);
        ArgumentNullException.ThrowIfNull(error);
        ArgumentNullException.ThrowIfNull(shutdown);
        if (arguments.Length == 0) return false;

        switch (arguments[0])
        {
            case "--bpshot":
                RunScreenshot(arguments, output, error, shutdown);
                return true;
            case "--bpcurve":
                RunCurve(arguments, output, error, shutdown);
                return true;
            case "--bpsrcgen":
                RunSourceGeneration(arguments, output, error, shutdown);
                return true;
            default:
                return false;
        }
    }

    /// <summary>
    /// 明示グラフに mode を適用して PNG を作る。入力、mode、出力 path が不正なら実行しない。
    /// </summary>
    private static void RunScreenshot(
        string[] arguments,
        TextWriter output,
        TextWriter error,
        Action<int> shutdown)
    {
        if (arguments.Length is < 3 or > 4)
        {
            RejectInput(
                "Usage: --bpshot <out.png> <--fixture|input.acsbp> [mode]",
                error,
                shutdown);
            return;
        }
        if (!TryResolveOutputPath(arguments[1], out string outputPath, out string pathError))
        {
            RejectInput(pathError, error, shutdown);
            return;
        }
        if (!TryResolveGraph(
                arguments[2],
                ScreenshotGraph,
                out GraphInput graph,
                out string graphError))
        {
            RejectInput(graphError, error, shutdown);
            return;
        }

        string mode = arguments.Length == 4 ? arguments[3] : "";
        if (!TryValidateScreenshotMode(mode, out string modeError))
        {
            RejectInput(modeError, error, shutdown);
            return;
        }

        BlueprintWindow? window = null;
        string? temporarySource = null;
        string success = "";
        string failure = "";
        int exitCode = 1;
        Application.Current.ShutdownMode = ShutdownMode.OnExplicitShutdown;
        try
        {
            window = new BlueprintWindow
            {
                WindowStartupLocation = WindowStartupLocation.Manual,
                Left = -30000,
                Top = -30000,
                Width = 1280,
                Height = 760,
                ShowActivated = false,
                ShowInTaskbar = false,
            };
            window.Show();
            LoadGraph(window.Editor, graph);
            if (mode == "gencpp")
            {
                temporarySource = Path.Combine(
                    Path.GetTempPath(),
                    "acs-blueprint-visual-" + Guid.NewGuid().ToString("N"));
                Directory.CreateDirectory(temporarySource);
                window.Editor.SourceDir = temporarySource;
            }
            ApplyScreenshotMode(window.Editor, mode);
            window.UpdateLayout();
            (int width, int height) = RenderWindow(window, outputPath);
            success =
                $"PASS blueprint screenshot: {outputPath} ({width}x{height})";
            exitCode = 0;
        }
        catch (Exception exception)
        {
            failure = "Blueprint screenshot failed: " + OneLine(exception.Message);
        }
        finally
        {
            TryCloseWindow(window, ref exitCode, ref failure);
            TryDeleteDirectory(temporarySource, ref exitCode, ref failure);
        }

        if (exitCode == 0) output.WriteLine(success);
        else error.WriteLine(failure);
        shutdown(exitCode);
    }

    /// <summary>
    /// Timeline を持つ明示グラフを可視ウィンドウで開く。Timeline が無い場合は起動前に失敗する。
    /// </summary>
    private static void RunCurve(
        string[] arguments,
        TextWriter output,
        TextWriter error,
        Action<int> shutdown)
    {
        if (arguments.Length != 2)
        {
            RejectInput(
                "Usage: --bpcurve <--fixture|input.acsbp>",
                error,
                shutdown);
            return;
        }
        if (!TryResolveGraph(
                arguments[1],
                CurveGraph,
                out GraphInput graph,
                out string graphError))
        {
            RejectInput(graphError, error, shutdown);
            return;
        }
        if (!ContainsTimelineNode(graph.Text))
        {
            RejectInput(
                "Blueprint curve input has no Timeline node.",
                error,
                shutdown);
            return;
        }

        BlueprintWindow? window = null;
        bool completed = false;
        int closeExitCode = 1;
        void Complete(int code)
        {
            if (completed) return;
            completed = true;
            shutdown(code);
        }

        Application.Current.ShutdownMode = ShutdownMode.OnExplicitShutdown;
        try
        {
            window = new BlueprintWindow
            {
                WindowStartupLocation = WindowStartupLocation.CenterScreen,
                Width = 1280,
                Height = 760,
            };
            BlueprintWindow activeWindow = window;
            activeWindow.Closed += (_, __) => Complete(closeExitCode);
            Application.Current.MainWindow = activeWindow;
            activeWindow.Show();
            _ = activeWindow.Dispatcher.BeginInvoke(
                DispatcherPriority.Loaded,
                new Action(() =>
                {
                    try
                    {
                        LoadGraph(activeWindow.Editor, graph);
                        if (!ContainsTimelineNode(activeWindow.Editor.Serialize()))
                            throw new InvalidDataException(
                                "Blueprint curve input has no Timeline node.");
                        activeWindow.Editor.OpenCurveForTest();
                        closeExitCode = 0;
                        output.WriteLine("READY blueprint curve: " + graph.DisplayName);
                    }
                    catch (Exception exception)
                    {
                        closeExitCode = 1;
                        error.WriteLine(
                            "Blueprint curve failed: " +
                            OneLine(exception.Message));
                        activeWindow.Close();
                    }
                }));
        }
        catch (Exception exception)
        {
            error.WriteLine(
                "Blueprint curve failed: " + OneLine(exception.Message));
            try
            {
                window?.Close();
            }
            catch
            {
            }
            Complete(1);
        }
    }

    /// <summary>
    /// 明示グラフから C++ を指定 directory へ生成する。入力検証に失敗したら directory を作らない。
    /// </summary>
    private static void RunSourceGeneration(
        string[] arguments,
        TextWriter output,
        TextWriter error,
        Action<int> shutdown)
    {
        if (arguments.Length != 3)
        {
            RejectInput(
                "Usage: --bpsrcgen <--fixture|input.acsbp> <source-dir>",
                error,
                shutdown);
            return;
        }
        if (!TryResolveGraph(
                arguments[1],
                ScreenshotGraph,
                out GraphInput graph,
                out string graphError))
        {
            RejectInput(graphError, error, shutdown);
            return;
        }
        if (!TryResolveDirectoryPath(
                arguments[2],
                out string sourceDirectory,
                out string directoryError))
        {
            RejectInput(directoryError, error, shutdown);
            return;
        }

        BlueprintWindow? window = null;
        string success = "";
        string failure = "";
        int exitCode = 1;
        Application.Current.ShutdownMode = ShutdownMode.OnExplicitShutdown;
        try
        {
            Directory.CreateDirectory(sourceDirectory);
            window = new BlueprintWindow
            {
                WindowStartupLocation = WindowStartupLocation.Manual,
                Left = -30000,
                Top = -30000,
                ShowActivated = false,
                ShowInTaskbar = false,
            };
            window.Show();
            LoadGraph(window.Editor, graph);
            window.Editor.SourceDir = sourceDirectory;
            string generatedSource =
                window.Editor.GenerateCppFile(build: false) ??
                throw new IOException("Blueprint C++ generation returned no output path.");
            string generatedHeader = Path.ChangeExtension(generatedSource, ".h");
            if (!File.Exists(generatedSource) || !File.Exists(generatedHeader))
                throw new IOException("Blueprint C++ generation did not create both outputs.");
            success = "PASS blueprint source generation: " + generatedSource;
            exitCode = 0;
        }
        catch (Exception exception)
        {
            failure =
                "Blueprint source generation failed: " +
                OneLine(exception.Message);
        }
        finally
        {
            TryCloseWindow(window, ref exitCode, ref failure);
        }

        if (exitCode == 0) output.WriteLine(success);
        else error.WriteLine(failure);
        shutdown(exitCode);
    }

    /// <summary>ファイルまたは明示 fixture を読み、正規 header でなければ false を返す。</summary>
    private static bool TryResolveGraph(
        string argument,
        string fixtureGraph,
        out GraphInput graph,
        out string error)
    {
        graph = new GraphInput("", null, "");
        error = "";
        if (argument == ExplicitFixtureArgument)
        {
            graph = new GraphInput(
                fixtureGraph,
                null,
                ExplicitFixtureArgument);
            return true;
        }
        if (string.IsNullOrWhiteSpace(argument) ||
            argument.StartsWith("--", StringComparison.Ordinal))
        {
            error = "Unknown Blueprint input selector: " + argument;
            return false;
        }

        try
        {
            string fullPath = Path.GetFullPath(argument);
            if (!File.Exists(fullPath))
            {
                error = "Blueprint input file does not exist: " + fullPath;
                return false;
            }
            string text = File.ReadAllText(fullPath);
            if (!HasCanonicalHeader(text))
            {
                error = "Blueprint input must begin with exactly 'ACSBP 1'.";
                return false;
            }
            graph = new GraphInput(text, fullPath, fullPath);
            return true;
        }
        catch (Exception exception)
        {
            error = "Blueprint input is invalid: " + OneLine(exception.Message);
            return false;
        }
    }

    /// <summary>PNG 出力 path を絶対 path にし、directory を指す場合は false を返す。</summary>
    private static bool TryResolveOutputPath(
        string argument,
        out string outputPath,
        out string error)
    {
        outputPath = "";
        error = "";
        try
        {
            if (string.IsNullOrWhiteSpace(argument))
            {
                error = "Blueprint screenshot output path is empty.";
                return false;
            }
            outputPath = Path.GetFullPath(argument);
            if (Directory.Exists(outputPath))
            {
                error = "Blueprint screenshot output path is a directory.";
                return false;
            }
            return true;
        }
        catch (Exception exception)
        {
            error =
                "Blueprint screenshot output path is invalid: " +
                OneLine(exception.Message);
            return false;
        }
    }

    /// <summary>生成先 directory を絶対 path にし、同名 file がある場合は false を返す。</summary>
    private static bool TryResolveDirectoryPath(
        string argument,
        out string directoryPath,
        out string error)
    {
        directoryPath = "";
        error = "";
        try
        {
            if (string.IsNullOrWhiteSpace(argument))
            {
                error = "Blueprint source output directory is empty.";
                return false;
            }
            directoryPath = Path.GetFullPath(argument);
            if (File.Exists(directoryPath))
            {
                error = "Blueprint source output directory is an existing file.";
                return false;
            }
            return true;
        }
        catch (Exception exception)
        {
            error =
                "Blueprint source output directory is invalid: " +
                OneLine(exception.Message);
            return false;
        }
    }

    /// <summary>mode 名と数値 suffix を検証し、未対応なら false を返す。</summary>
    private static bool TryValidateScreenshotMode(
        string mode,
        out string error)
    {
        error = "";
        if (mode is "" or "sel" or "validate" or "run" or "gencpp" or
            "func" or "viewport" or "vpangle" or "vptop" or "varcombo" or
            "collapse" or "watch" or "funcio" or "macro" or "override" or
            "split" or "arrange" or "expand" or "vpmove" or "promote" or
            "refs" or "align" or "autoconv" or "wire")
            return true;

        if (mode.StartsWith("vpnode", StringComparison.Ordinal))
            return TryValidateIntegerSuffix(mode, "vpnode", 1, out error);
        if (mode.StartsWith("node", StringComparison.Ordinal))
            return TryValidateIntegerSuffix(mode, "node", 0, out error);
        if (mode.StartsWith("step", StringComparison.Ordinal))
        {
            if (!TryGetIntegerSuffix(mode, "step", 3, out int steps) ||
                steps < 0 ||
                steps > 10000)
            {
                error = "Blueprint screenshot step mode requires 0..10000.";
                return false;
            }
            return true;
        }
        if (mode.StartsWith("zoom", StringComparison.Ordinal))
        {
            if (!TryGetNumberSuffix(mode, "zoom", 2.2, out double zoom) ||
                !double.IsFinite(zoom) ||
                zoom <= 0)
            {
                error = "Blueprint screenshot zoom mode requires a positive number.";
                return false;
            }
            return true;
        }

        error = "Unknown Blueprint screenshot mode: " + mode;
        return false;
    }

    /// <summary>検証済み mode に対応する診断操作を Editor へ 1 回だけ適用する。</summary>
    private static void ApplyScreenshotMode(
        BlueprintEditor editor,
        string mode)
    {
        if (mode == "") return;
        if (mode == "sel") editor.SelectAll();
        else if (mode == "validate") editor.ValidateForTest();
        else if (mode == "run")
        {
            editor.ValidateForTest();
            editor.RunGraph();
        }
        else if (mode == "gencpp")
        {
            _ = editor.GenerateCppFile(build: false) ??
                throw new IOException(
                    "Blueprint C++ generation returned no output path.");
        }
        else if (mode == "func") editor.SwitchToFirstFunctionForTest();
        else if (mode == "viewport") editor.ShowViewportForTest();
        else if (mode == "vpangle") editor.ViewportAngleForTest(2.4, 0.22);
        else if (mode == "vptop") editor.ViewportAngleForTest(0.0, 1.35);
        else if (mode == "varcombo") editor.OpenVarComboForTest();
        else if (mode == "collapse") editor.CollapseForTest();
        else if (mode == "watch") editor.WatchPinForTest();
        else if (mode == "funcio") editor.FuncIOForTest();
        else if (mode == "macro") editor.MacroForTest();
        else if (mode == "override") editor.OverrideForTest();
        else if (mode == "split") editor.SplitForTest();
        else if (mode == "arrange") editor.ArrangeForTest();
        else if (mode == "expand") editor.ExpandMathForTest();
        else if (mode == "vpmove") editor.MoveComponentForTest(1, 70, -30);
        else if (mode == "promote") editor.PromotePinForTest(3, false, 1);
        else if (mode == "refs") editor.SelectVariableReferences("HP");
        else if (mode == "align") editor.AlignGuideForTest(4, 2);
        else if (mode == "autoconv") editor.AutoConvertForTest();
        else if (mode == "wire") editor.DebugStartWireForTest(2, 1);
        else if (mode.StartsWith("vpnode", StringComparison.Ordinal))
            editor.HighlightViewportNode(
                GetIntegerSuffix(mode, "vpnode", 1));
        else if (mode.StartsWith("node", StringComparison.Ordinal))
            editor.SelectOneForTest(GetIntegerSuffix(mode, "node", 0));
        else if (mode.StartsWith("zoom", StringComparison.Ordinal))
            editor.DebugZoomForTest(GetNumberSuffix(mode, "zoom", 2.2));
        else if (mode.StartsWith("step", StringComparison.Ordinal))
            editor.DebugStepForTest(GetIntegerSuffix(mode, "step", 3));
    }

    /// <summary>入力グラフを Editor へ読み、file 読込の失敗は例外にする。</summary>
    private static void LoadGraph(
        BlueprintEditor editor,
        GraphInput graph)
    {
        if (graph.SourcePath is null)
        {
            editor.Deserialize(graph.Text);
            return;
        }

        editor.LoadFromFile(graph.SourcePath);
        if (!string.Equals(
                editor.CurrentPath,
                graph.SourcePath,
                StringComparison.OrdinalIgnoreCase))
        {
            throw new IOException(
                "Blueprint input could not be loaded: " + graph.SourcePath);
        }
    }

    /// <summary>ウィンドウを PNG に書き、出力寸法を返す。書込に失敗したら例外を返す。</summary>
    private static (int Width, int Height) RenderWindow(
        Window window,
        string outputPath)
    {
        int width = Math.Max(1, (int)Math.Ceiling(window.ActualWidth));
        int height = Math.Max(1, (int)Math.Ceiling(window.ActualHeight));
        var bitmap = new RenderTargetBitmap(
            width,
            height,
            96,
            96,
            PixelFormats.Pbgra32);
        bitmap.Render(window);

        string? outputDirectory = Path.GetDirectoryName(outputPath);
        if (!string.IsNullOrEmpty(outputDirectory))
            Directory.CreateDirectory(outputDirectory);
        using var stream = new FileStream(
            outputPath,
            FileMode.Create,
            FileAccess.Write,
            FileShare.None);
        var encoder = new PngBitmapEncoder();
        encoder.Frames.Add(BitmapFrame.Create(bitmap));
        encoder.Save(stream);
        stream.Flush(flushToDisk: true);
        return (width, height);
    }

    /// <summary>先頭行が ACSBP 1 の場合だけ true を返す。</summary>
    private static bool HasCanonicalHeader(string text)
    {
        using var reader = new StringReader(text);
        return string.Equals(
            reader.ReadLine(),
            "ACSBP 1",
            StringComparison.Ordinal);
    }

    /// <summary>Timeline ノード宣言が 1 つ以上ある場合だけ true を返す。</summary>
    private static bool ContainsTimelineNode(string text)
    {
        foreach (string rawLine in text.Replace("\r", "").Split('\n'))
        {
            string[] parts = rawLine.Split(
                ' ',
                6,
                StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length == 6 &&
                parts[0] == "N" &&
                parts[5] == "Timeline")
                return true;
        }
        return false;
    }

    /// <summary>整数 suffix が省略値または 0 以上なら true を返す。</summary>
    private static bool TryValidateIntegerSuffix(
        string value,
        string prefix,
        int defaultValue,
        out string error)
    {
        error = "";
        if (TryGetIntegerSuffix(value, prefix, defaultValue, out int parsed) &&
            parsed >= 0)
            return true;
        error =
            "Blueprint screenshot mode requires a non-negative integer: " +
            value;
        return false;
    }

    /// <summary>prefix 後の整数を読み、suffix が無ければ省略値を返す。</summary>
    private static bool TryGetIntegerSuffix(
        string value,
        string prefix,
        int defaultValue,
        out int parsed)
    {
        string suffix = value.Substring(prefix.Length);
        if (suffix.Length == 0)
        {
            parsed = defaultValue;
            return true;
        }
        return int.TryParse(
            suffix,
            NumberStyles.Integer,
            CultureInfo.InvariantCulture,
            out parsed);
    }

    /// <summary>prefix 後の数値を読み、suffix が無ければ省略値を返す。</summary>
    private static bool TryGetNumberSuffix(
        string value,
        string prefix,
        double defaultValue,
        out double parsed)
    {
        string suffix = value.Substring(prefix.Length);
        if (suffix.Length == 0)
        {
            parsed = defaultValue;
            return true;
        }
        return double.TryParse(
            suffix,
            NumberStyles.Float,
            CultureInfo.InvariantCulture,
            out parsed);
    }

    /// <summary>検証済み整数 suffix を返す。</summary>
    private static int GetIntegerSuffix(
        string value,
        string prefix,
        int defaultValue)
    {
        _ = TryGetIntegerSuffix(value, prefix, defaultValue, out int parsed);
        return parsed;
    }

    /// <summary>検証済み数値 suffix を返す。</summary>
    private static double GetNumberSuffix(
        string value,
        string prefix,
        double defaultValue)
    {
        _ = TryGetNumberSuffix(value, prefix, defaultValue, out double parsed);
        return parsed;
    }

    /// <summary>診断 window を閉じ、失敗したら終了コードと診断を更新する。</summary>
    private static void TryCloseWindow(
        Window? window,
        ref int exitCode,
        ref string failure)
    {
        if (window is null) return;
        try
        {
            window.Close();
        }
        catch (Exception exception)
        {
            if (exitCode == 0)
            {
                exitCode = 1;
                failure =
                    "Blueprint diagnostic window close failed: " +
                    OneLine(exception.Message);
            }
        }
    }

    /// <summary>一時 directory を削除し、失敗したら終了コードと診断を更新する。</summary>
    private static void TryDeleteDirectory(
        string? directory,
        ref int exitCode,
        ref string failure)
    {
        if (string.IsNullOrEmpty(directory) || !Directory.Exists(directory))
            return;
        try
        {
            Directory.Delete(directory, recursive: true);
        }
        catch (Exception exception)
        {
            if (exitCode == 0)
            {
                exitCode = 1;
                failure =
                    "Blueprint diagnostic cleanup failed: " +
                    OneLine(exception.Message);
            }
        }
    }

    /// <summary>入力不正を stderr に 1 行出力し、終了コード 2 を返す。</summary>
    private static void RejectInput(
        string message,
        TextWriter error,
        Action<int> shutdown)
    {
        error.WriteLine("Blueprint diagnostic input error: " + OneLine(message));
        shutdown(2);
    }

    /// <summary>診断文を 1 行に正規化する。</summary>
    private static string OneLine(string message) =>
        message.Replace('\r', ' ').Replace('\n', ' ').Trim();

    /// <summary>検証済みグラフ本文、任意の読込 path、診断表示名を保持する。</summary>
    private sealed record GraphInput(
        string Text,
        string? SourcePath,
        string DisplayName);
}
