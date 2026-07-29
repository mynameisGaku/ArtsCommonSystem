// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;

namespace AcsEditor;

public partial class AssetImportSettingsWindow : Window
{
    private readonly string[] _assetKinds;
    private bool _initialized;

    internal AssetImporterSettings Settings { get; private set; }

    internal AssetImportSettingsWindow(
        IEnumerable<string> sourcePaths,
        AssetImporterSettings settings)
    {
        ArgumentNullException.ThrowIfNull(sourcePaths);
        ArgumentNullException.ThrowIfNull(settings);
        InitializeComponent();

        string[] paths = sourcePaths
            .Where(static path => !string.IsNullOrWhiteSpace(path))
            .Take(4096)
            .ToArray();
        _assetKinds = paths
            .Select(static path =>
                AssetDatabase.ClassifyExtension(Path.GetExtension(path)))
            .Distinct(StringComparer.Ordinal)
            .OrderBy(static kind => kind, StringComparer.Ordinal)
            .ToArray();
        Settings = settings.Normalize();

        SelectionCountText.Text =
            paths.Length.ToString(CultureInfo.InvariantCulture) +
            (paths.Length == 1 ? " FILE" : " FILES");
        SelectionSummaryText.Text = BuildSelectionSummary(paths, _assetKinds);
        foreach (string path in paths.Take(128))
            SelectedFileList.Items.Add(Path.GetFileName(path));
        if (paths.Length > 128)
        {
            SelectedFileList.Items.Add(
                $"+ {paths.Length - 128} additional file(s)");
        }

        TextureSection.Visibility =
            VisibilityForKind("image");
        MeshSection.Visibility =
            VisibilityForKind("mesh");
        AudioSection.Visibility =
            VisibilityForKind("audio");
        PassthroughSection.Visibility =
            _assetKinds.Any(static kind =>
                kind is not ("image" or "mesh" or "audio"))
                ? Visibility.Visible
                : Visibility.Collapsed;

        SelectTag(TextureColorSpaceBox, Settings.TextureColorSpace);
        SelectTag(TextureCompressionBox, Settings.TextureCompression);
        TextureMipmapsBox.IsChecked = Settings.TextureGenerateMipmaps;
        TextureNormalDetectionBox.IsChecked = Settings.TextureDetectNormalMap;
        MeshScaleBox.Text =
            Settings.MeshUniformScale.ToString("R", CultureInfo.InvariantCulture);
        MeshTangentsBox.IsChecked = Settings.MeshImportTangents;
        MeshCollisionBox.IsChecked = Settings.MeshGenerateCollision;
        AudioStreamingBox.IsChecked = Settings.AudioStreaming;
        AudioNormalizeBox.IsChecked = Settings.AudioNormalize;
        SelectTag(
            AudioSampleRateBox,
            Settings.AudioSampleRate.ToString(CultureInfo.InvariantCulture));

        AttachSummaryHandlers();
        _initialized = true;
        UpdateRecipeSummary();
    }

    private Visibility VisibilityForKind(string kind) =>
        _assetKinds.Contains(kind, StringComparer.Ordinal)
            ? Visibility.Visible
            : Visibility.Collapsed;

    private void AttachSummaryHandlers()
    {
        TextureColorSpaceBox.SelectionChanged += OnSettingChanged;
        TextureCompressionBox.SelectionChanged += OnSettingChanged;
        AudioSampleRateBox.SelectionChanged += OnSettingChanged;
        MeshScaleBox.TextChanged += OnSettingChanged;
        TextureMipmapsBox.Checked += OnSettingChanged;
        TextureMipmapsBox.Unchecked += OnSettingChanged;
        TextureNormalDetectionBox.Checked += OnSettingChanged;
        TextureNormalDetectionBox.Unchecked += OnSettingChanged;
        MeshTangentsBox.Checked += OnSettingChanged;
        MeshTangentsBox.Unchecked += OnSettingChanged;
        MeshCollisionBox.Checked += OnSettingChanged;
        MeshCollisionBox.Unchecked += OnSettingChanged;
        AudioStreamingBox.Checked += OnSettingChanged;
        AudioStreamingBox.Unchecked += OnSettingChanged;
        AudioNormalizeBox.Checked += OnSettingChanged;
        AudioNormalizeBox.Unchecked += OnSettingChanged;
    }

    private void OnSettingChanged(object sender, RoutedEventArgs e)
    {
        if (_initialized)
            UpdateRecipeSummary();
    }

    private void UpdateRecipeSummary()
    {
        if (!TryReadSettings(out AssetImporterSettings? settings, out string? error))
        {
            ValidationText.Text = error;
            ValidationText.Visibility = Visibility.Visible;
            RecipeSummaryText.Text = "Recipe validation failed.";
            ImportButton.IsEnabled = false;
            return;
        }

        ValidationText.Visibility = Visibility.Collapsed;
        ImportButton.IsEnabled = true;
        string[] identities = _assetKinds
            .Select(kind => AssetImporterRecipeContract.Create(kind, settings))
            .Select(static recipe =>
                $"{recipe.Importer} v{recipe.ImporterVersion} " +
                recipe.RecipeHash[..8])
            .Distinct(StringComparer.Ordinal)
            .OrderBy(static value => value, StringComparer.Ordinal)
            .ToArray();
        RecipeSummaryText.Text = identities.Length == 0
            ? "No importable files are selected."
            : "Recipes: " + string.Join("  •  ", identities);
    }

    private bool TryReadSettings(
        out AssetImporterSettings? settings,
        out string? error)
    {
        settings = null;
        error = null;
        if (!double.TryParse(
                MeshScaleBox.Text,
                NumberStyles.Float,
                CultureInfo.InvariantCulture,
                out double meshScale))
        {
            error = "Mesh uniform scale must use a decimal number.";
            return false;
        }
        if (!int.TryParse(
                SelectedTag(AudioSampleRateBox),
                NumberStyles.None,
                CultureInfo.InvariantCulture,
                out int sampleRate))
        {
            error = "Audio sample rate selection is invalid.";
            return false;
        }

        try
        {
            settings = new AssetImporterSettings(
                SelectedTag(TextureColorSpaceBox),
                SelectedTag(TextureCompressionBox),
                TextureMipmapsBox.IsChecked == true,
                TextureNormalDetectionBox.IsChecked == true,
                meshScale,
                MeshTangentsBox.IsChecked == true,
                MeshCollisionBox.IsChecked == true,
                AudioStreamingBox.IsChecked == true,
                AudioNormalizeBox.IsChecked == true,
                sampleRate).Normalize();
            return true;
        }
        catch (InvalidDataException validationError)
        {
            error = validationError.Message;
            return false;
        }
    }

    private void OnAccept(object sender, RoutedEventArgs e)
    {
        if (!TryReadSettings(out AssetImporterSettings? settings, out string? error))
        {
            ValidationText.Text = error;
            ValidationText.Visibility = Visibility.Visible;
            return;
        }
        Settings = settings!;
        DialogResult = true;
    }

    private void OnCancel(object sender, RoutedEventArgs e)
    {
        DialogResult = false;
    }

    private void OnMinimize(object sender, RoutedEventArgs e)
    {
        WindowState = WindowState.Minimized;
    }

    private void OnTitleBarMouseDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ChangedButton == MouseButton.Left)
            DragMove();
    }

    private void OnPreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key != Key.Escape)
            return;
        e.Handled = true;
        DialogResult = false;
    }

    private static void SelectTag(ComboBox comboBox, string tag)
    {
        foreach (object item in comboBox.Items)
        {
            if (item is ComboBoxItem candidate &&
                string.Equals(
                    candidate.Tag?.ToString(),
                    tag,
                    StringComparison.Ordinal))
            {
                comboBox.SelectedItem = candidate;
                return;
            }
        }
        throw new InvalidDataException(
            $"Importer settings UI has no option for '{tag}'.");
    }

    private static string SelectedTag(ComboBox comboBox) =>
        (comboBox.SelectedItem as ComboBoxItem)?.Tag?.ToString()
        ?? throw new InvalidDataException(
            "Importer settings UI selection is incomplete.");

    private static string BuildSelectionSummary(
        IReadOnlyCollection<string> paths,
        IReadOnlyCollection<string> kinds)
    {
        if (paths.Count == 0)
            return "No files selected.";
        string typeSummary = string.Join(
            ", ",
            kinds.Select(static kind => kind.ToUpperInvariant()));
        return $"{paths.Count} source file(s)\n{typeSummary}";
    }
}
