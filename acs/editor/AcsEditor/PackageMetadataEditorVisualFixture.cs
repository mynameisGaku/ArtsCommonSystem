// SPDX-License-Identifier: Apache-2.0

using AcsEditor.Packaging;
using System;
using System.IO;
using System.Text;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace AcsEditor;

internal static class PackageMetadataEditorVisualFixture
{
    internal static int Run(string outputPath, TextWriter log)
    {
        string root = Path.Combine(
            Path.GetTempPath(),
            "acs-package-metadata-visual-" + Guid.NewGuid().ToString("N"));
        string config = Path.Combine(root, "Config");
        ProjectSettingsWindow? window = null;
        try
        {
            Directory.CreateDirectory(config);
            PackageProductMetadataContract.SaveOptionalAtomic(
                config,
                new PackageProductMetadata(
                    1,
                    "Northwind Interactive",
                    "A high-fidelity flight experience built with ACS.",
                    "Copyright 2026 Northwind Interactive",
                    "https://support.example.com/northwind"));
            PackageMetadataEditorSession session =
                PackageMetadataEditorSession.Load(root);

            window = new ProjectSettingsWindow(
                root,
                session)
            {
                Width = 1020,
                Height = 700,
                Left = -30000,
                Top = -30000,
                ShowActivated = false,
                ShowInTaskbar = false,
                WindowStartupLocation = WindowStartupLocation.Manual,
            };
            window.Show();
            window.UpdateLayout();

            if (!window.ValidatePackageMetadataFixtureLayout(
                    out string layoutError))
            {
                log.WriteLine("FAIL  " + layoutError);
                return 1;
            }

            string fullOutput = Path.GetFullPath(outputPath);
            RenderWindow(window, fullOutput);

            window.SetPackageMetadataFixtureInvalidSupportUrl();
            window.UpdateLayout();
            if (!window.ValidatePackageMetadataFixtureInvalidState(
                    out layoutError))
            {
                log.WriteLine("FAIL  invalid-state " + layoutError);
                return 1;
            }
            string invalidOutput = Path.Combine(
                Path.GetDirectoryName(fullOutput) ?? "",
                Path.GetFileNameWithoutExtension(fullOutput) +
                "-invalid" +
                Path.GetExtension(fullOutput));
            RenderWindow(window, invalidOutput);

            log.WriteLine(
                "PASS  package metadata visual fixture: " +
                fullOutput +
                " and " +
                invalidOutput);
            return 0;
        }
        catch (Exception error)
        {
            log.WriteLine("FAIL  package metadata visual fixture: " + error);
            return 1;
        }
        finally
        {
            if (window is not null)
            {
                window.AllowPackageMetadataFixtureClose();
                window.Close();
            }
            try
            {
                Directory.Delete(root, recursive: true);
            }
            catch
            {
            }
        }
    }

    private static void RenderWindow(
        Window window,
        string outputPath)
    {
        int pixelWidth = Math.Max(
            1,
            (int)Math.Ceiling(window.ActualWidth));
        int pixelHeight = Math.Max(
            1,
            (int)Math.Ceiling(window.ActualHeight));
        var bitmap = new RenderTargetBitmap(
            pixelWidth,
            pixelHeight,
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
    }
}

public partial class ProjectSettingsWindow
{
    internal ProjectSettingsWindow(
        string projectRoot,
        PackageMetadataEditorSession packageMetadataFixture)
    {
        InitializeComponent();
        _engine = IntPtr.Zero;
        _applyMutation = static (_, _, _) => false;
        _uiReady = true;
        ReloadPackageMetadataFixture(
            projectRoot,
            packageMetadataFixture);
        UpdateSearchChrome();
        UpdateAddButton();
    }

    private void ReloadPackageMetadataFixture(
        string projectRoot,
        PackageMetadataEditorSession session)
    {
        _packageMetadataProjectRoot = Path.GetFullPath(projectRoot);
        ConfigurePackageMetadataFieldLimits();
        _packageMetadataSession = session;
        _packageMetadataBusy = false;
        PackageMetadataFieldsPanel.IsEnabled = true;
        PackageMetadataRetryButton.Visibility = Visibility.Collapsed;
        PopulatePackageMetadataFields(session.Draft);
        RefreshCategoryList(keepSelection: false);
        foreach (object item in CatList.Items)
        {
            if (item is CategoryItem { IsPackageMetadata: true })
            {
                CatList.SelectedItem = item;
                break;
            }
        }
    }

    internal bool ValidatePackageMetadataFixtureLayout(
        out string error)
    {
        error = "";
        if (PackageMetadataScroll.Visibility != Visibility.Visible)
        {
            error = "Distribution metadata surface is not visible.";
            return false;
        }
        bool aggregateCountIsCoherent = false;
        bool distributionCountIsCoherent = false;
        foreach (object item in CatList.Items)
        {
            if (item is CategoryItem { IsAll: true, Count: 4 })
                aggregateCountIsCoherent = true;
            if (item is CategoryItem
                {
                    IsPackageMetadata: true,
                    Count: 4,
                })
            {
                distributionCountIsCoherent = true;
            }
        }
        if (!aggregateCountIsCoherent ||
            !distributionCountIsCoherent)
        {
            error =
                "All settings and Distribution counts do not include the same metadata fields.";
            return false;
        }
        if (PackageMetadataFieldsPanel.ActualWidth < 420 ||
            PackageMetadataFieldsPanel.ActualHeight < 300)
        {
            error =
                "Distribution metadata fields did not receive a usable layout " +
                $"({PackageMetadataFieldsPanel.ActualWidth:0}x" +
                $"{PackageMetadataFieldsPanel.ActualHeight:0}).";
            return false;
        }

        Rect revertBounds =
            PackageMetadataRevertButton
                .TransformToAncestor(this)
                .TransformBounds(
                    new Rect(PackageMetadataRevertButton.RenderSize));
        Rect applyBounds =
            PackageMetadataApplyButton
                .TransformToAncestor(this)
                .TransformBounds(
                    new Rect(PackageMetadataApplyButton.RenderSize));
        if (revertBounds.IntersectsWith(applyBounds) ||
            applyBounds.Right > ActualWidth ||
            applyBounds.Bottom > ActualHeight ||
            applyBounds.Width < 70 ||
            applyBounds.Height < 28)
        {
            error = "Apply/Revert controls overlap or are clipped.";
            return false;
        }
        return true;
    }

    internal void SetPackageMetadataFixtureInvalidSupportUrl()
    {
        SupportUrlBox.Text = "http://support.example.com/not-https";
    }

    internal bool ValidatePackageMetadataFixtureInvalidState(
        out string error)
    {
        if (SupportUrlValidationText.Visibility != Visibility.Visible ||
            PackageMetadataRevertButton.IsEnabled != true ||
            PackageMetadataApplyButton.IsEnabled)
        {
            error =
                "Invalid support URL did not produce inline validation with Revert-only recovery.";
            return false;
        }
        return ValidatePackageMetadataFixtureLayout(out error);
    }

    internal void AllowPackageMetadataFixtureClose() =>
        _allowPackageMetadataClose = true;
}
