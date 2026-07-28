// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace AcsEditor;

internal static class AssetPackageReadinessVisualFixture
{
    internal static int Run(string outputPath, TextWriter log)
    {
        AssetPackageReadinessWindow? window = null;
        try
        {
            var request = new AssetPackageReadinessRequest(
                "SkyCombat",
                @"C:\Fixture\SkyCombat",
                @"C:\Fixture\SkyCombat\Assets",
                "11111111111111111111111111111111");
            var diagnostics =
                new List<AssetPackageReadinessDiagnostic>
                {
                    new(
                        AssetCookDiagnosticSeverity.Error,
                        "ASSET_DEPENDENCY_MISSING",
                        "'Aircraft/Fighter.acsbp' references a missing dependency GUID.",
                        "Aircraft/Fighter.acsbp",
                        "22222222222222222222222222222222",
                        "Open Reference Viewer for the owning asset and replace or restore the missing dependency."),
                    new(
                        AssetCookDiagnosticSeverity.Error,
                        "ASSET_TYPE_UNSUPPORTED",
                        "A required asset uses an unsupported Cook input format.",
                        "Environment/weather.rawcloud",
                        "33333333333333333333333333333333",
                        "Import or convert this required file to a supported Cook input format."),
                    new(
                        AssetCookDiagnosticSeverity.Error,
                        "ASSET_PATH_UNSAFE",
                        "Asset reference crosses a reparse point.",
                        "Shared/CloudNoise.ktx2",
                        "44444444444444444444444444444444",
                        "Remove the symlink/junction and import an ordinary file inside Assets."),
                    new(
                        AssetCookDiagnosticSeverity.Warning,
                        "ASSET_INDEX_CACHE_IGNORED",
                        "Asset index cache was stale; authoritative sidecars were used.",
                        "",
                        "",
                        "No action is normally required; refresh Asset View if the warning persists."),
                };
            var assets = new List<AssetPackageReadinessAsset>
            {
                Asset(
                    "11111111111111111111111111111111",
                    "Scenes/Main.acs3d",
                    "scene",
                    2840,
                    'a'),
                Asset(
                    "22222222222222222222222222222222",
                    "Aircraft/Fighter.acsbp",
                    "blueprint",
                    9824,
                    'b'),
                Asset(
                    "33333333333333333333333333333333",
                    "Environment/weather.rawcloud",
                    "file",
                    142800,
                    'c'),
                Asset(
                    "44444444444444444444444444444444",
                    "Shared/CloudNoise.ktx2",
                    "image",
                    512000,
                    'd'),
            };
            var report = new AssetPackageReadinessReport(
                1,
                false,
                "SkyCombat",
                request.CanonicalSceneAssetId,
                "Scenes/Main.acs3d",
                4,
                3,
                1,
                new string('e', 64),
                diagnostics,
                assets);

            window = new AssetPackageReadinessWindow(
                request,
                report)
            {
                Width = 1080,
                Height = 720,
                Left = -30000,
                Top = -30000,
                ShowActivated = false,
                ShowInTaskbar = false,
                WindowStartupLocation =
                    WindowStartupLocation.Manual,
            };
            window.Show();
            window.UpdateLayout();
            if (!window.ValidateVisualFixtureLayout(out string error))
            {
                log.WriteLine(
                    "FAIL  " +
                    error +
                    $" Window={window.ActualWidth:0}x" +
                    $"{window.ActualHeight:0}; diagnostics=" +
                    $"{window.DiagnosticsGrid.ActualWidth:0}x" +
                    $"{window.DiagnosticsGrid.ActualHeight:0}.");
                return 1;
            }
            if (!ValidateVisualContract(
                    window,
                    closureSelected: false,
                    out error))
            {
                log.WriteLine("FAIL  " + error);
                return 1;
            }

            string fullOutput = Path.GetFullPath(outputPath);
            RenderWindow(window, fullOutput);
            window.ShowVisualFixtureClosure();
            var hashCell = new DataGridCellInfo(
                window.AssetsGrid.Items[0],
                window.AssetsGrid.Columns[4]);
            window.AssetsGrid.CurrentCell = hashCell;
            window.AssetsGrid.SelectedCells.Clear();
            window.AssetsGrid.SelectedCells.Add(hashCell);
            window.UpdateLayout();
            if (!ValidateVisualContract(
                    window,
                    closureSelected: true,
                    out error))
            {
                log.WriteLine("FAIL  closure " + error);
                return 1;
            }
            string closureOutput = Path.Combine(
                Path.GetDirectoryName(fullOutput) ?? "",
                Path.GetFileNameWithoutExtension(fullOutput) +
                "-closure" +
                Path.GetExtension(fullOutput));
            RenderWindow(window, closureOutput);
            log.WriteLine(
                "PASS  Package Readiness visual fixture: " +
                fullOutput + " and " + closureOutput);
            return 0;
        }
        catch (Exception error)
        {
            log.WriteLine(
                "FAIL  Package Readiness visual fixture: " +
                error);
            return 1;
        }
        finally
        {
            window?.Close();
        }
    }

    private static AssetPackageReadinessAsset Asset(
        string id,
        string path,
        string kind,
        long size,
        char hash) =>
        new(
            id,
            path,
            kind,
            size,
            new string(hash, 64));

    private static bool ValidateVisualContract(
        AssetPackageReadinessWindow window,
        bool closureSelected,
        out string error)
    {
        error = "";
        if (window.ReadinessTabs.ItemContainerStyle == null ||
            window.ReadinessTabs.Items.Count != 2 ||
            window.ReadinessTabs.Items[0] is not TabItem diagnostics ||
            window.ReadinessTabs.Items[1] is not TabItem closure)
        {
            error =
                "Package Readiness tabs do not share an explicit item-container style.";
            return false;
        }

        diagnostics.ApplyTemplate();
        closure.ApplyTemplate();
        TabItem selected = closureSelected ? closure : diagnostics;
        TabItem idle = closureSelected ? diagnostics : closure;
        if (!selected.IsSelected ||
            idle.IsSelected ||
            selected.Template.FindName(
                "TabChrome",
                selected) is not Border selectedChrome ||
            idle.Template.FindName(
                "TabChrome",
                idle) is not Border idleChrome ||
            selected.Template.FindName(
                "SelectionAccent",
                selected) is not Border selectedAccent ||
            selected.Template.FindName(
                "FocusRing",
                selected) is not Border)
        {
            error =
                "Dark tab chrome, selected accent, or focus affordance is missing.";
            return false;
        }

        Brush accent = (Brush)window.FindResource("Accent");
        if (!BrushMatches(selectedAccent.Background, accent) ||
            IsWhite(selectedChrome.Background) ||
            IsWhite(idleChrome.Background))
        {
            error =
                "Selected tab accent is incoherent or default white tab chrome remains.";
            return false;
        }

        if (window.AssetsGrid.SelectionUnit !=
                DataGridSelectionUnit.Cell ||
            window.AssetsGrid.ClipboardCopyMode !=
                DataGridClipboardCopyMode.ExcludeHeader ||
            window.AssetsGrid.Columns.Count < 5 ||
            window.AssetsGrid.Columns[4] is not
                DataGridTemplateColumn hashColumn ||
            hashColumn.Width.UnitType !=
                DataGridLengthUnitType.Star ||
            hashColumn.MinWidth < 320 ||
            hashColumn.ClipboardContentBinding == null)
        {
            error =
                "SHA-256 column is not a flexible, cell-copyable closure column.";
            return false;
        }

        if (hashColumn.HeaderTemplate?.LoadContent() is not
                DependencyObject header ||
            FindDescendant<TextBlock>(
                header,
                item => item.Text == "CTRL+C") == null ||
            hashColumn.CellTemplate?.LoadContent() is not
                DependencyObject cell ||
            FindDescendant<TextBlock>(
                cell,
                item => item.TextTrimming ==
                            TextTrimming.CharacterEllipsis) is not
                TextBlock hashText ||
            !string.Equals(
                hashText.FontFamily.Source,
                "Consolas",
                StringComparison.OrdinalIgnoreCase) ||
            BindingOperations.GetBindingBase(
                hashText,
                ToolTipService.ToolTipProperty) == null ||
            !AutomationProperties.GetHelpText(hashText)
                .Contains(
                    "Ctrl+C",
                    StringComparison.Ordinal))
        {
            error =
                "SHA-256 cell lacks monospace ellipsis, full-value tooltip, or visible copy guidance.";
            return false;
        }
        return true;
    }

    private static T? FindDescendant<T>(
        DependencyObject root,
        Func<T, bool> predicate)
        where T : DependencyObject
    {
        if (root is T rootMatch && predicate(rootMatch))
            return rootMatch;

        int count = VisualTreeHelper.GetChildrenCount(root);
        for (int index = 0; index < count; index++)
        {
            DependencyObject child =
                VisualTreeHelper.GetChild(root, index);
            if (child is T match && predicate(match))
                return match;
            T? nested = FindDescendant(child, predicate);
            if (nested != null)
                return nested;
        }
        return null;
    }

    private static bool BrushMatches(
        Brush? actual,
        Brush expected) =>
        actual is SolidColorBrush actualSolid &&
        expected is SolidColorBrush expectedSolid &&
        actualSolid.Color == expectedSolid.Color;

    private static bool IsWhite(Brush? brush) =>
        brush is SolidColorBrush solid &&
        solid.Color.R == 255 &&
        solid.Color.G == 255 &&
        solid.Color.B == 255;

    private static void RenderWindow(
        Window window,
        string outputPath)
    {
        int width = Math.Max(
            1,
            (int)Math.Ceiling(window.ActualWidth));
        int height = Math.Max(
            1,
            (int)Math.Ceiling(window.ActualHeight));
        var bitmap = new RenderTargetBitmap(
            width,
            height,
            96,
            96,
            PixelFormats.Pbgra32);
        bitmap.Render(window);

        string? directory = Path.GetDirectoryName(outputPath);
        if (!string.IsNullOrEmpty(directory))
            Directory.CreateDirectory(directory);
        using var stream = new FileStream(
            outputPath,
            FileMode.Create,
            FileAccess.Write,
            FileShare.None);
        var encoder = new PngBitmapEncoder();
        encoder.Frames.Add(BitmapFrame.Create(bitmap));
        encoder.Save(stream);
        stream.Flush(flushToDisk: true);
    }
}
