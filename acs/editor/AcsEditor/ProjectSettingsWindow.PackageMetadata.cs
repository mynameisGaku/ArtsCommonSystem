// SPDX-License-Identifier: Apache-2.0
// Distribution metadata editing inside the Project Settings surface.

using AcsEditor.Packaging;
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace AcsEditor;

public partial class ProjectSettingsWindow
{
    private const string PackageMetadataCategoryName = "Distribution";

    internal static int AggregateProjectSettingsCount(
        int nativeSettingCount,
        bool packageMetadataMatches)
    {
        if (nativeSettingCount < 0)
            throw new ArgumentOutOfRangeException(nameof(nativeSettingCount));
        return checked(
            nativeSettingCount +
            (packageMetadataMatches ? 4 : 0));
    }

    private string _packageMetadataProjectRoot = "";
    private PackageMetadataEditorSession? _packageMetadataSession;
    private bool _packageMetadataUpdating;
    private bool _packageMetadataBusy;
    private bool _packageMetadataApplyBusy;
    private bool _packageMetadataLifetimeEnded;
    private bool _closeAfterPackageMetadataApply;
    private bool _allowPackageMetadataClose;
    private long _packageMetadataLoadGeneration;

    private void InitializePackageMetadata(string projectRoot)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(projectRoot);
        _packageMetadataProjectRoot =
            Path.GetFullPath(projectRoot);
        ConfigurePackageMetadataFieldLimits();
        _ = LoadPackageMetadataAsync();
    }

    private void ConfigurePackageMetadataFieldLimits()
    {
        PublisherBox.MaxLength =
            PackageProductMetadataContract.MaximumPublisherLength;
        DescriptionBox.MaxLength =
            PackageProductMetadataContract.MaximumDescriptionLength;
        CopyrightBox.MaxLength =
            PackageProductMetadataContract.MaximumCopyrightLength;
        SupportUrlBox.MaxLength =
            PackageProductMetadataContract.MaximumSupportUrlLength;
    }

    private bool PackageMetadataMatchesSearch(string searchText)
    {
        string query = searchText.Trim();
        if (query.Length == 0)
            return true;

        IEnumerable<string> searchable =
        [
            PackageMetadataCategoryName,
            "package",
            "distribution",
            "publisher",
            "description",
            "copyright",
            "support URL",
            PackageProductMetadataContract.FileName,
        ];
        if (_packageMetadataSession is { } session)
        {
            searchable = searchable.Concat(
            [
                session.Draft.Publisher,
                session.Draft.Description,
                session.Draft.Copyright,
                session.Draft.SupportUrl,
            ]);
        }
        return searchable.Any(value => ContainsInvariant(value, query));
    }

    private async Task LoadPackageMetadataAsync()
    {
        long generation = ++_packageMetadataLoadGeneration;
        _packageMetadataBusy = true;
        SetPackageMetadataLoadingChrome();

        PackageMetadataEditorSession? loaded = null;
        string loadError = "";
        try
        {
            loaded = await Task.Run(
                () => PackageMetadataEditorSession.Load(
                    _packageMetadataProjectRoot));
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or
                InvalidDataException or JsonException or
                DecoderFallbackException or ArgumentException or
                NotSupportedException)
        {
            loadError = error.Message;
        }

        if (_packageMetadataLifetimeEnded ||
            generation != _packageMetadataLoadGeneration)
        {
            return;
        }

        _packageMetadataBusy = false;
        _packageMetadataSession = loaded;
        PackageMetadataFieldsPanel.IsEnabled = loaded is not null;
        PackageMetadataRetryButton.Visibility =
            loaded is null ? Visibility.Visible : Visibility.Collapsed;
        if (loaded is null)
        {
            ShowPackageMetadataLoadError(loadError);
        }
        else
        {
            PopulatePackageMetadataFields(loaded.Draft);
        }

        // Search category visibility depends on both loaded values and errors.
        RefreshCategoryList(keepSelection: true);
    }

    private void SetPackageMetadataLoadingChrome()
    {
        PackageMetadataFieldsPanel.IsEnabled = false;
        PackageMetadataRetryButton.Visibility = Visibility.Collapsed;
        PackageMetadataStateBorder.Background = ResourceBrush("AccentDim");
        PackageMetadataStateBorder.BorderBrush = ResourceBrush("Accent");
        PackageMetadataStateTitle.Text = "Loading package metadata…";
        PackageMetadataStateText.Text =
            "Reading Config/PackageMetadata.json with package preflight rules.";
        PackageMetadataApplyButton.IsEnabled = false;
        PackageMetadataRevertButton.IsEnabled = false;
    }

    private void ShowPackageMetadataLoadError(string error)
    {
        PackageMetadataStateBorder.Background = Brushes.Transparent;
        PackageMetadataStateBorder.BorderBrush = ResourceBrush("WarnFg");
        PackageMetadataStateTitle.Text = "Package metadata could not be loaded";
        PackageMetadataStateText.Text =
            string.IsNullOrWhiteSpace(error)
                ? "Config/PackageMetadata.json was rejected. Repair it and retry; the Editor will not overwrite ambiguous input."
                : error + " Repair the file and retry; the Editor will not overwrite ambiguous input.";
        PackageMetadataApplyButton.IsEnabled = false;
        PackageMetadataRevertButton.IsEnabled = false;
        PackageMetadataFieldsPanel.IsEnabled = false;
    }

    private void PopulatePackageMetadataFields(
        PackageProductMetadata metadata)
    {
        _packageMetadataUpdating = true;
        try
        {
            PublisherBox.Text = metadata.Publisher;
            DescriptionBox.Text = metadata.Description;
            CopyrightBox.Text = metadata.Copyright;
            SupportUrlBox.Text = metadata.SupportUrl;
        }
        finally
        {
            _packageMetadataUpdating = false;
        }
        RefreshPackageMetadataValidationAndChrome();
    }

    private void OnPackageMetadataFieldChanged(
        object sender,
        TextChangedEventArgs e)
    {
        if (_packageMetadataUpdating ||
            _packageMetadataSession is not { } session)
        {
            return;
        }

        session.UpdateDraft(ReadPackageMetadataDraft());
        RefreshPackageMetadataValidationAndChrome();
    }

    private PackageProductMetadata ReadPackageMetadataDraft() =>
        new(
            1,
            PublisherBox.Text,
            DescriptionBox.Text,
            CopyrightBox.Text,
            SupportUrlBox.Text);

    private void RefreshPackageMetadataValidationAndChrome(
        string saveError = "")
    {
        if (_packageMetadataSession is not { } session)
            return;

        ClearPackageMetadataFieldValidation(
            PublisherBox,
            PublisherValidationText);
        ClearPackageMetadataFieldValidation(
            DescriptionBox,
            DescriptionValidationText);
        ClearPackageMetadataFieldValidation(
            CopyrightBox,
            CopyrightValidationText);
        ClearPackageMetadataFieldValidation(
            SupportUrlBox,
            SupportUrlValidationText);
        PackageMetadataGlobalValidationText.Text = "";
        PackageMetadataGlobalValidationText.Visibility = Visibility.Collapsed;

        IReadOnlyList<PackageProductMetadataValidationIssue> issues =
            session.ValidationIssues;
        foreach (PackageProductMetadataValidationIssue issue in issues)
        {
            switch (issue.Field)
            {
                case "publisher":
                    ShowPackageMetadataFieldValidation(
                        PublisherBox,
                        PublisherValidationText,
                        issue.Message);
                    break;
                case "description":
                    ShowPackageMetadataFieldValidation(
                        DescriptionBox,
                        DescriptionValidationText,
                        issue.Message);
                    break;
                case "copyright":
                    ShowPackageMetadataFieldValidation(
                        CopyrightBox,
                        CopyrightValidationText,
                        issue.Message);
                    break;
                case "supportUrl":
                    ShowPackageMetadataFieldValidation(
                        SupportUrlBox,
                        SupportUrlValidationText,
                        issue.Message);
                    break;
                default:
                    PackageMetadataGlobalValidationText.Text = issue.Message;
                    PackageMetadataGlobalValidationText.Visibility =
                        Visibility.Visible;
                    break;
            }
        }

        PublisherCountText.Text =
            $"{PublisherBox.Text.Length}/{PackageProductMetadataContract.MaximumPublisherLength}";
        DescriptionCountText.Text =
            $"{DescriptionBox.Text.Length}/{PackageProductMetadataContract.MaximumDescriptionLength}";
        CopyrightCountText.Text =
            $"{CopyrightBox.Text.Length}/{PackageProductMetadataContract.MaximumCopyrightLength}";
        SupportUrlCountText.Text =
            $"{SupportUrlBox.Text.Length}/{PackageProductMetadataContract.MaximumSupportUrlLength}";

        PackageMetadataFieldsPanel.IsEnabled = !_packageMetadataBusy;
        PackageMetadataRetryButton.Visibility =
            !string.IsNullOrWhiteSpace(saveError)
                ? Visibility.Visible
                : Visibility.Collapsed;
        PackageMetadataRevertButton.IsEnabled =
            session.IsDirty && !_packageMetadataBusy;
        PackageMetadataApplyButton.IsEnabled =
            session.IsDirty &&
            issues.Count == 0 &&
            !_packageMetadataBusy;

        if (_packageMetadataBusy)
        {
            PackageMetadataStateBorder.Background =
                ResourceBrush("AccentDim");
            PackageMetadataStateBorder.BorderBrush =
                ResourceBrush("Accent");
            PackageMetadataStateTitle.Text = "Applying package metadata…";
            PackageMetadataStateText.Text =
                "The existing file remains intact until the atomic publication succeeds.";
        }
        else if (!string.IsNullOrWhiteSpace(saveError))
        {
            PackageMetadataStateBorder.Background = Brushes.Transparent;
            PackageMetadataStateBorder.BorderBrush =
                ResourceBrush("WarnFg");
            PackageMetadataStateTitle.Text = "Apply failed";
            PackageMetadataStateText.Text =
                saveError + " The previous metadata file was preserved.";
        }
        else if (issues.Count > 0)
        {
            PackageMetadataStateBorder.Background = Brushes.Transparent;
            PackageMetadataStateBorder.BorderBrush =
                ResourceBrush("WarnFg");
            PackageMetadataStateTitle.Text = "Package metadata needs attention";
            PackageMetadataStateText.Text =
                "Fix the inline validation errors before applying.";
        }
        else if (session.IsDirty)
        {
            PackageMetadataStateBorder.Background =
                ResourceBrush("AccentDim");
            PackageMetadataStateBorder.BorderBrush =
                ResourceBrush("Accent");
            PackageMetadataStateTitle.Text = "Unsaved package metadata";
            PackageMetadataStateText.Text =
                session.Draft.IsEmpty
                    ? "Apply removes Config/PackageMetadata.json and returns this project to the unconfigured state."
                    : "Apply writes a canonical Config/PackageMetadata.json used by Package preflight.";
        }
        else if (session.IsConfigured)
        {
            PackageMetadataStateBorder.Background =
                ResourceBrush("AccentDim");
            PackageMetadataStateBorder.BorderBrush =
                ResourceBrush("Accent");
            PackageMetadataStateTitle.Text = "Package metadata configured";
            PackageMetadataStateText.Text =
                "Package preflight will validate and embed these values in the distribution manifest.";
        }
        else
        {
            PackageMetadataStateBorder.Background = Brushes.Transparent;
            PackageMetadataStateBorder.BorderBrush =
                ResourceBrush("Hairline");
            PackageMetadataStateTitle.Text = "Package metadata not configured";
            PackageMetadataStateText.Text =
                session.SourceFileExists
                    ? "The existing metadata file is empty. It remains untouched until Apply; clearing an edited document and applying removes it."
                    : "No PackageMetadata.json exists. Packages remain compatible, but Shipping preflight reports missing distribution metadata.";
        }
    }

    private void ClearPackageMetadataFieldValidation(
        TextBox field,
        TextBlock message)
    {
        message.Text = "";
        message.Visibility = Visibility.Collapsed;
        field.ClearValue(Control.BorderBrushProperty);
        field.ClearValue(ToolTipProperty);
    }

    private void ShowPackageMetadataFieldValidation(
        TextBox field,
        TextBlock message,
        string text)
    {
        message.Text = text;
        message.Visibility = Visibility.Visible;
        field.BorderBrush = ResourceBrush("WarnFg");
        field.ToolTip = text;
    }

    private async void OnApplyPackageMetadata(
        object sender,
        RoutedEventArgs e)
    {
        await ApplyPackageMetadataAsync();
    }

    private async Task ApplyPackageMetadataAsync()
    {
        if (_packageMetadataBusy ||
            _packageMetadataSession is not { } session)
        {
            return;
        }
        if (session.ValidationIssues.Count > 0)
        {
            _closeAfterPackageMetadataApply = false;
            RefreshPackageMetadataValidationAndChrome();
            return;
        }

        _packageMetadataBusy = true;
        _packageMetadataApplyBusy = true;
        RefreshPackageMetadataValidationAndChrome();
        PackageMetadataApplyResult result =
            await Task.Run(session.Apply);
        if (_packageMetadataLifetimeEnded)
            return;

        _packageMetadataBusy = false;
        _packageMetadataApplyBusy = false;
        RefreshPackageMetadataValidationAndChrome(
            result.Succeeded ? "" : result.ErrorMessage);

        if (_closeAfterPackageMetadataApply && result.Succeeded)
        {
            _allowPackageMetadataClose = true;
            Close();
        }
        else if (!result.Succeeded)
        {
            _closeAfterPackageMetadataApply = false;
        }
    }

    private void OnRevertPackageMetadata(
        object sender,
        RoutedEventArgs e)
    {
        if (_packageMetadataBusy ||
            _packageMetadataSession is not { } session)
        {
            return;
        }
        session.Revert();
        PopulatePackageMetadataFields(session.Draft);
    }

    private async void OnRetryPackageMetadataLoad(
        object sender,
        RoutedEventArgs e)
    {
        if (_packageMetadataBusy)
            return;
        if (_packageMetadataSession is { IsDirty: true })
        {
            MessageBoxResult choice = MessageBox.Show(
                this,
                "Reloading PackageMetadata.json discards the staged metadata draft.",
                "Reload Package Metadata",
                MessageBoxButton.OKCancel,
                MessageBoxImage.Warning,
                MessageBoxResult.Cancel);
            if (choice != MessageBoxResult.OK)
                return;
        }
        await LoadPackageMetadataAsync();
    }

    private void ShowPackageMetadataEditor()
    {
        SettingsRowsScroll.Visibility = Visibility.Collapsed;
        PackageMetadataScroll.Visibility = Visibility.Visible;
        CustomSettingPanel.Visibility = Visibility.Collapsed;
        SectionTitleText.Text = "Distribution metadata";
        ResultSummaryText.Text =
            "Publisher and support information embedded by Package";
        VisibleCountText.Text = "4 FIELDS";
    }

    private void ShowSettingsRows()
    {
        PackageMetadataScroll.Visibility = Visibility.Collapsed;
        SettingsRowsScroll.Visibility = Visibility.Visible;
        CustomSettingPanel.Visibility = Visibility.Visible;
    }

    private void OnProjectSettingsWindowClosing(
        object? sender,
        CancelEventArgs e)
    {
        if (_allowPackageMetadataClose ||
            _packageMetadataLifetimeEnded)
        {
            return;
        }
        if (_packageMetadataApplyBusy)
        {
            e.Cancel = true;
            _closeAfterPackageMetadataApply = true;
            return;
        }
        if (_packageMetadataSession is not { IsDirty: true } session)
            return;

        MessageBoxResult choice = MessageBox.Show(
            this,
            "Package metadata has unapplied changes.\n\nApply them before closing Project Settings?",
            "Unsaved Package Metadata",
            MessageBoxButton.YesNoCancel,
            MessageBoxImage.Warning,
            MessageBoxResult.Yes);
        switch (choice)
        {
            case MessageBoxResult.Yes:
                e.Cancel = true;
                _closeAfterPackageMetadataApply = true;
                _ = ApplyPackageMetadataAsync();
                break;
            case MessageBoxResult.No:
                session.Revert();
                _allowPackageMetadataClose = true;
                break;
            default:
                e.Cancel = true;
                break;
        }
    }

    private void OnProjectSettingsWindowClosed(
        object? sender,
        EventArgs e)
    {
        _packageMetadataLifetimeEnded = true;
        _packageMetadataLoadGeneration++;
    }
}
