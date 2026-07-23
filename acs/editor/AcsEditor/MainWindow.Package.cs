// SPDX-License-Identifier: Apache-2.0

using System;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;

namespace AcsEditor;

public partial class MainWindow
{
    private sealed class BuildWorkflowLease : IDisposable
    {
        private MainWindow? _owner;
        private readonly CancellationTokenSource _cancellation = new();
        private readonly TaskCompletionSource<bool> _completion = new(
            TaskCreationOptions.RunContinuationsAsynchronously);

        internal BuildWorkflowLease(MainWindow owner) => _owner = owner;

        internal CancellationToken Token => _cancellation.Token;
        internal Task Completion => _completion.Task;
        internal bool IsCancellationRequested =>
            _cancellation.IsCancellationRequested;

        internal void RequestCancellation()
        {
            try
            {
                _cancellation.Cancel();
            }
            catch (ObjectDisposedException)
            {
            }
        }

        public void Dispose()
        {
            MainWindow? owner = Interlocked.Exchange(ref _owner, null);
            if (owner == null)
                return;
            owner.CompleteBuildWorkflow(this);
            _cancellation.Dispose();
            _completion.TrySetResult(true);
        }
    }

    private BuildWorkflowLease? _activeBuildWorkflow;
    private PackageProjectDialog? _activePackageDialog;

    private BuildWorkflowLease BeginBuildWorkflow()
    {
        if (_activeBuildWorkflow != null)
            throw new InvalidOperationException(
                "Another build/package workflow is already active.");
        var workflow = new BuildWorkflowLease(this);
        _activeBuildWorkflow = workflow;
        return workflow;
    }

    private void CompleteBuildWorkflow(BuildWorkflowLease workflow)
    {
        if (ReferenceEquals(_activeBuildWorkflow, workflow))
            _activeBuildWorkflow = null;
    }

    internal static async Task StopGameProcessForReplacementAsync(
        Process? process,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (process == null || process.HasExited)
            return;

        try
        {
            process.Kill(entireProcessTree: true);
        }
        catch (InvalidOperationException) when (process.HasExited)
        {
            return;
        }
        // Once termination starts, drain it independently of editor-close
        // cancellation. This keeps the dispatcher responsive while ensuring
        // close cannot observe the build workflow as complete with a prior
        // game process still exiting.
        await process.WaitForExitAsync()
            .WaitAsync(TimeSpan.FromSeconds(2));
        cancellationToken.ThrowIfCancellationRequested();
    }

    private async void OnPackageProject(object sender, RoutedEventArgs e)
    {
        if (IsSceneEditingBlocked) return;
        if (_project == null)
        {
            BuildLog("プロジェクトがありません。");
            return;
        }
        if (_building)
        {
            BuildLog("別のビルドが実行中です。");
            return;
        }
        if (!EnsureBuildSceneCompatibility("Package"))
            return;

        ShowBottomTab("build");
        BuildLog($"==== Package Project: {_project.Name} ====");
        using BuildWorkflowLease workflow = BeginBuildWorkflow();
        _building = true;
        SetBuildUiEnabled(false);
        try
        {
            // Source save now awaits autosave generation drain. Keep that await inside the same
            // build exclusion window so repeated shortcuts/clicks cannot start a second package.
            if (!await SaveSceneForBuildAsync())
                return;
            workflow.Token.ThrowIfCancellationRequested();
            var dialog = new PackageProjectDialog(_project, BuildLog);
            _activePackageDialog = dialog;
            try
            {
                await dialog.ShowModelessAsync(this);
            }
            finally
            {
                if (ReferenceEquals(_activePackageDialog, dialog))
                    _activePackageDialog = null;
            }
        }
        catch (OperationCanceledException)
            when (workflow.IsCancellationRequested)
        {
            BuildLog("Package Project cancelled during editor shutdown.");
        }
        catch (Exception error)
        {
            BuildLog("Package Project failed: " + error.Message);
        }
        finally
        {
            _building = false;
            SetBuildUiEnabled(true);
        }
    }

    private async Task<bool> DrainActiveBuildForEditorCloseAsync()
    {
        BuildWorkflowLease? workflow = _activeBuildWorkflow;
        if (workflow != null)
        {
            PackageProjectDialog? dialog = _activePackageDialog;
            if (dialog != null)
            {
                bool dialogDrained = await dialog.RequestOwnerShutdownAsync(
                    PackageProjectDialog.OwnerShutdownDrainTimeout);
                if (!dialogDrained)
                {
                    BuildLog(
                        "Editor close deferred: package cancellation did not " +
                        "finish within the shutdown deadline.");
                    return false;
                }
            }

            // Once the dialog reports drained, every external process and output
            // reader is already terminal. Keep a small, separately bounded grace
            // only for ShowModelessAsync and this command's dispatcher continuation.
            TimeSpan workflowDeadline = dialog == null
                ? PackageProjectDialog.OwnerShutdownDrainTimeout
                : TimeSpan.FromSeconds(2);
            bool workflowDrained =
                await PackageShutdownCoordinator.CancelAndDrainAsync(
                    workflow.Completion,
                    workflow.RequestCancellation,
                    workflowDeadline);
            if (!workflowDrained)
            {
                BuildLog(
                    "Editor close deferred: the active build/package workflow is " +
                    "still draining. No child process was abandoned.");
                return false;
            }
        }
        return true;
    }

    private async Task<bool> DrainStandaloneGameForEditorCloseAsync()
    {
        try
        {
            await StopGameProcessForReplacementAsync(
                _gameProcess,
                CancellationToken.None);
            return true;
        }
        catch (Exception error)
        {
            BuildLog(
                "Editor close deferred: the standalone game process did not " +
                "drain safely: " + error.Message);
            return false;
        }
    }
}
