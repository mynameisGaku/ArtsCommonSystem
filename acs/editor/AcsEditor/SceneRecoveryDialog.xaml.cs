// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Input;

namespace AcsEditor;

internal enum SceneRecoveryDecision
{
    Recover,
    Discard,
    Cancel,
}

public partial class SceneRecoveryDialog : Window
{
    internal const bool DisablesOwnerDuringPrompt = false;

    internal SceneRecoveryDecision Decision { get; private set; } = SceneRecoveryDecision.Cancel;

    internal SceneRecoveryDialog(SceneRecoveryCandidate candidate)
    {
        InitializeComponent();
        string document = string.IsNullOrWhiteSpace(candidate.Identity.OriginalPath)
            ? "(untitled scene)"
            : candidate.Identity.OriginalPath;
        DocumentText.Text = document;
        DocumentText.ToolTip = document;
        ModeText.Text = candidate.Identity.Mode == SceneDocumentMode.ThreeD
            ? "Scene source: .acs3d (legacy 3D format)"
            : "Scene source: .acscene (legacy 2D format)";
        CapturedText.Text = candidate.CapturedUtc.ToLocalTime().ToString("F");
        SizeText.Text = FormatBytes(candidate.ContentBytes)
                      + "  ·  SHA-256 " + candidate.ContentSha256[..12] + "…";

        FreshnessText.Text = "";
        string? original = candidate.Identity.OriginalPath;
        if (!string.IsNullOrWhiteSpace(original) && File.Exists(original))
        {
            DateTimeOffset sourceWrite = File.GetLastWriteTimeUtc(original);
            if (sourceWrite > candidate.CapturedUtc)
                FreshnessText.Text =
                    "The source file is newer than this recovery snapshot. Review carefully before recovering.";
        }
    }

    internal static bool CanPresentModelessPrompt(
        bool environmentUserInteractive,
        bool applicationAvailable,
        bool ownerVisible) =>
        environmentUserInteractive && applicationAvailable && ownerVisible;

    /// <summary>
    /// Presents recovery without entering WPF's owner-disabling modal loop.
    /// Recovery discovery completes asynchronously after editor startup; using
    /// ShowDialog here used to make the otherwise healthy editor look frozen
    /// whenever the prompt was hidden behind another window. The owned window
    /// still stays above the editor, but the editor can always be moved and the
    /// scene can continue to be inspected while the user decides.
    /// </summary>
    internal static Task<SceneRecoveryDecision> PromptAsync(
        Window owner,
        SceneRecoveryCandidate candidate)
    {
        ArgumentNullException.ThrowIfNull(owner);
        ArgumentNullException.ThrowIfNull(candidate);
        owner.Dispatcher.VerifyAccess();

        if (!CanPresentModelessPrompt(
                Environment.UserInteractive,
                Application.Current != null,
                owner.IsVisible))
        {
            return Task.FromResult(SceneRecoveryDecision.Cancel);
        }

        var completion = new TaskCompletionSource<SceneRecoveryDecision>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var dialog = new SceneRecoveryDialog(candidate)
        {
            Owner = owner,
            // Do not steal focus if recovery discovery finishes while the user
            // is working in another application. As an owned window it becomes
            // visible above the editor when the user returns to it.
            ShowActivated = owner.IsActive,
        };

        EventHandler? onOwnerClosed = null;
        EventHandler? onDialogClosed = null;

        void Complete()
        {
            if (onOwnerClosed != null)
                owner.Closed -= onOwnerClosed;
            if (onDialogClosed != null)
                dialog.Closed -= onDialogClosed;
            completion.TrySetResult(dialog.Decision);
        }

        onDialogClosed = (_, _) => Complete();
        onOwnerClosed = (_, _) =>
        {
            // Owned windows normally close with their owner. Explicitly close
            // as well so the returned task cannot outlive editor shutdown.
            if (dialog.IsVisible)
                dialog.Close();
            else
                Complete();
        };
        dialog.Closed += onDialogClosed;
        owner.Closed += onOwnerClosed;

        try
        {
            dialog.Show();
        }
        catch
        {
            dialog.Closed -= onDialogClosed;
            owner.Closed -= onOwnerClosed;
            throw;
        }
        return completion.Task;
    }

    private void OnRecover(object sender, RoutedEventArgs e)
    {
        Decision = SceneRecoveryDecision.Recover;
        Close();
    }

    private void OnDiscard(object sender, RoutedEventArgs e)
    {
        Decision = SceneRecoveryDecision.Discard;
        Close();
    }

    private void OnCancel(object sender, RoutedEventArgs e)
    {
        Decision = SceneRecoveryDecision.Cancel;
        Close();
    }

    protected override void OnPreviewKeyDown(KeyEventArgs e)
    {
        if (e.Key == Key.Escape)
        {
            Decision = SceneRecoveryDecision.Cancel;
            e.Handled = true;
            Close();
            return;
        }
        base.OnPreviewKeyDown(e);
    }

    private static string FormatBytes(long bytes)
    {
        if (bytes < 1024) return bytes + " B";
        if (bytes < 1024 * 1024) return (bytes / 1024.0).ToString("0.0") + " KiB";
        return (bytes / (1024.0 * 1024.0)).ToString("0.0") + " MiB";
    }
}
