// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace AcsEditor;

internal static class AssetImportSettingsVisualFixture
{
    internal static int Run(string outputPath, TextWriter log)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(outputPath);
        ArgumentNullException.ThrowIfNull(log);

        AssetImportSettingsWindow? window = null;
        try
        {
            window = new AssetImportSettingsWindow(
                new[]
                {
                    @"C:\Source\Textures\ocean_normal.png",
                    @"C:\Source\Meshes\island.fbx",
                    @"C:\Source\Audio\shoreline.wav",
                    @"C:\Source\Data\weather.csv",
                },
                AssetImporterSettings.Default with
                {
                    TextureCompression = "normal-map",
                    MeshGenerateCollision = true,
                    AudioStreaming = true,
                    AudioSampleRate = 48000,
                })
            {
                Width = 940,
                Height = 720,
                Left = -30000,
                Top = -30000,
                ShowActivated = false,
                ShowInTaskbar = false,
                WindowStartupLocation = WindowStartupLocation.Manual,
            };
            window.Show();
            window.UpdateLayout();

            if (!window.ValidateVisualFixtureLayout(out string error))
            {
                log.WriteLine("FAIL  asset importer visual fixture: " + error);
                return 1;
            }

            string fullOutput = Path.GetFullPath(outputPath);
            RenderWindow(window, fullOutput);

            window.SetVisualFixtureInvalidScale();
            window.UpdateLayout();
            if (!window.ValidateVisualFixtureInvalidState(out error))
            {
                log.WriteLine(
                    "FAIL  asset importer invalid-state fixture: " + error);
                return 1;
            }

            string invalidOutput = Path.Combine(
                Path.GetDirectoryName(fullOutput) ?? "",
                Path.GetFileNameWithoutExtension(fullOutput) +
                "-invalid" +
                Path.GetExtension(fullOutput));
            RenderWindow(window, invalidOutput);

            log.WriteLine(
                "PASS  asset importer visual fixture: " +
                fullOutput +
                " and " +
                invalidOutput);
            return 0;
        }
        catch (Exception error)
        {
            log.WriteLine("FAIL  asset importer visual fixture: " + error);
            return 1;
        }
        finally
        {
            window?.Close();
        }
    }

    private static void RenderWindow(Window window, string outputPath)
    {
        int pixelWidth = Math.Max(1, (int)Math.Ceiling(window.ActualWidth));
        int pixelHeight = Math.Max(1, (int)Math.Ceiling(window.ActualHeight));
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

public partial class AssetImportSettingsWindow
{
    internal bool ValidateVisualFixtureLayout(out string error)
    {
        error = "";
        if (TextureSection.Visibility != Visibility.Visible ||
            MeshSection.Visibility != Visibility.Visible ||
            AudioSection.Visibility != Visibility.Visible ||
            PassthroughSection.Visibility != Visibility.Visible)
        {
            error = "Mixed selection did not expose every applicable section.";
            return false;
        }
        if (SelectedFileList.Items.Count != 4 ||
            SelectedFileList.ActualWidth < 140 ||
            TextureColorSpaceBox.ActualWidth < 180 ||
            MeshScaleBox.ActualWidth < 180 ||
            AudioSampleRateBox.ActualWidth < 180)
        {
            error =
                "Selection or field columns did not receive usable geometry.";
            return false;
        }
        if (!ImportButton.IsEnabled ||
            ValidationText.Visibility == Visibility.Visible ||
            string.IsNullOrWhiteSpace(RecipeSummaryText.Text))
        {
            error = "Valid settings did not produce an actionable recipe.";
            return false;
        }

        Rect importBounds =
            ImportButton
                .TransformToAncestor(this)
                .TransformBounds(new Rect(ImportButton.RenderSize));
        if (importBounds.Width < 96 ||
            importBounds.Height < 28 ||
            importBounds.Left < 0 ||
            importBounds.Top < 0 ||
            importBounds.Right > ActualWidth ||
            importBounds.Bottom > ActualHeight)
        {
            error = "Import action is clipped or too small.";
            return false;
        }
        return true;
    }

    internal void SetVisualFixtureInvalidScale()
    {
        MeshScaleBox.Text = "not-a-number";
    }

    internal bool ValidateVisualFixtureInvalidState(out string error)
    {
        if (ValidationText.Visibility != Visibility.Visible ||
            ImportButton.IsEnabled ||
            string.IsNullOrWhiteSpace(ValidationText.Text))
        {
            error =
                "Invalid numeric input did not disable Import with inline feedback.";
            return false;
        }
        return ValidateInvalidVisualFixtureGeometry(out error);
    }

    private bool ValidateInvalidVisualFixtureGeometry(out string error)
    {
        error = "";
        Rect validationBounds =
            ValidationText
                .TransformToAncestor(this)
                .TransformBounds(new Rect(ValidationText.RenderSize));
        if (validationBounds.Width < 120 ||
            validationBounds.Left < 0 ||
            validationBounds.Right > ActualWidth ||
            validationBounds.Bottom > ActualHeight)
        {
            error = "Inline validation feedback is clipped.";
            return false;
        }
        return true;
    }
}
