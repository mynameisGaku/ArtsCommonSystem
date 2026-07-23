// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Threading.Tasks;
using System.Windows;

namespace AcsEditor;

public partial class MainWindow
{
    private enum SaveAllCompletion
    {
        Success,
        Cancelled,
        Failed,
    }

    private enum SceneDocumentWriteCompletion
    {
        Saved,
        Cancelled,
        Failed,
    }

    private readonly record struct SaveAllResult(
        SaveAllCompletion Completion,
        int SavedCount,
        int PlannedCount,
        SceneDocumentMode? StoppedDocument,
        string Detail);

    private readonly record struct SceneDocumentWriteResult(
        SceneDocumentWriteCompletion Completion,
        string Detail);

    private async void OnSaveAllScenes(object sender, RoutedEventArgs e)
    {
        SaveAllResult result = await SaveAllInitializedSceneDocumentsAsync();
        ReportSaveAllResult(result);
    }

    private void ReportSaveAllResult(SaveAllResult result)
    {
        string message = FormatSaveAllResult(result);
        StatusText.Text = message;
        Log(message, "Scene", result.Completion switch
        {
            SaveAllCompletion.Success => LogLevel.Success,
            SaveAllCompletion.Cancelled => LogLevel.Warn,
            _ => LogLevel.Error,
        });
    }

    /// <summary>
    /// Refreshes both native documents, then saves only initialized dirty documents. Each mode has
    /// its own native serializer, so the inactive document can be written without changing the
    /// viewport mode, selection, active path alias, or camera.
    /// </summary>
    private async Task<SaveAllResult> SaveAllInitializedSceneDocumentsAsync()
    {
        if (!TryBeginSceneSourceSave(
                out SceneSourceSaveScope? saveScope,
                out string saveBlockedReason))
        {
            return new SaveAllResult(
                SaveAllCompletion.Failed,
                0,
                0,
                null,
                saveBlockedReason);
        }
        using SceneSourceSaveScope saveLease = saveScope!;

        if (Engine == IntPtr.Zero)
            return new SaveAllResult(
                SaveAllCompletion.Failed, 0, 0, null, "the editor engine is unavailable");
        if (EngineInterop.acs_editor_play_state(Engine) != 0 || PreviewBtn.IsChecked == true)
            return new SaveAllResult(
                SaveAllCompletion.Failed, 0, 0, null,
                "stop Play/Preview before saving scene source");

        if (!TryRefreshAllSceneDirtyStates(out string refreshError))
            return new SaveAllResult(
                SaveAllCompletion.Failed, 0, 0, null, refreshError);

        IReadOnlyList<SceneDocumentMode> plan = SceneSaveAllPlanner.BuildOrder(
            _view3d,
            _scene2DInitialized,
            _scene2DDirty,
            _scene3DInitialized,
            _scene3DDirty);
        int saved = 0;
        foreach (SceneDocumentMode mode in plan)
        {
            SceneDocumentWriteResult write = await SaveSceneDocumentAsync(mode);
            if (write.Completion == SceneDocumentWriteCompletion.Saved)
            {
                ++saved;
                continue;
            }

            return new SaveAllResult(
                write.Completion == SceneDocumentWriteCompletion.Cancelled
                    ? SaveAllCompletion.Cancelled
                    : SaveAllCompletion.Failed,
                saved,
                plan.Count,
                mode,
                write.Detail);
        }

        return new SaveAllResult(
            SaveAllCompletion.Success, saved, plan.Count, null, "");
    }

    /// <summary>
    /// Native serializers own independent 2D/3D graphs. Refresh both snapshots before planning so
    /// edits made since the 750 ms status tick cannot be missed by Save All.
    /// </summary>
    private bool TryRefreshAllSceneDirtyStates(out string error)
    {
        error = "";
        bool dirty2D = _scene2DDirty;
        bool dirty3D = _scene3DDirty;

        if (_scene2DInitialized)
        {
            try
            {
                string current = NormalizeSceneSnapshot(EngineInterop.SceneText(Engine));
                dirty2D = _scene2DSavedSnapshot == null ||
                          !string.Equals(
                              current, _scene2DSavedSnapshot, StringComparison.Ordinal);
            }
            catch (Exception ex)
            {
                _scene2DDirty = true;
                if (!_view3d)
                {
                    _snapshotCaptureFailed = true;
                    SetSceneDirty(true);
                }
                error = ".acscene source state could not be captured: " + ex.Message;
                return false;
            }
        }

        if (_scene3DInitialized)
        {
            try
            {
                string current = NormalizeSceneSnapshot(EngineInterop.Scene3DText(Engine));
                dirty3D = _scene3DSavedSnapshot == null ||
                          !string.Equals(
                              current, _scene3DSavedSnapshot, StringComparison.Ordinal);
            }
            catch (Exception ex)
            {
                _scene2DDirty = dirty2D;
                _scene3DDirty = true;
                if (_view3d)
                    _snapshotCaptureFailed = true;
                else
                    _snapshotCaptureFailed = false;
                SetSceneDirty(_view3d ? true : dirty2D);
                error = ".acs3d source state could not be captured: " + ex.Message;
                return false;
            }
        }

        _scene2DDirty = dirty2D;
        _scene3DDirty = dirty3D;
        _snapshotCaptureFailed = false;
        SetSceneDirty(_view3d ? dirty3D : dirty2D);
        _sceneMutationRevision.AcknowledgeWorkspace();
        return true;
    }

    private async Task<SceneDocumentWriteResult> SaveSceneDocumentAsync(
        SceneDocumentMode mode)
    {
        bool use3D = mode == SceneDocumentMode.ThreeD;
        string? previousPath = use3D ? _scene3DDocumentPath : _scene2DPath;
        string? target = previousPath;

        // Preserve the existing 3D document convention. A newly created 2D document remains
        // untitled and therefore prompts, matching Save Scene.
        if (use3D && string.IsNullOrWhiteSpace(target) &&
            !string.IsNullOrWhiteSpace(Scene3DPath))
            target = Scene3DPath;

        if (string.IsNullOrWhiteSpace(target))
        {
            var dialog = new Microsoft.Win32.SaveFileDialog
            {
                Title = use3D
                    ? "Save Legacy .acs3d Source"
                    : "Save .acscene Source",
                Filter = use3D
                    ? "Legacy ACS 3D Source (*.acs3d)|*.acs3d|All files (*.*)|*.*"
                    : "ACS Scene Source (*.acscene)|*.acscene|All files (*.*)|*.*",
                DefaultExt = use3D ? ".acs3d" : ".acscene",
                AddExtension = true,
                OverwritePrompt = true,
                FileName = use3D ? "scene3d.acs3d" : "scene.acscene",
                InitialDirectory = _project?.AssetsDir,
            };
            if (dialog.ShowDialog(this) != true)
                return new SceneDocumentWriteResult(
                    SceneDocumentWriteCompletion.Cancelled, "path selection was cancelled");
            target = dialog.FileName;
        }

        if (!TryBeginSceneSourceSave(
                out SceneSourceSaveScope? mutationScope,
                out string mutationBlockedReason))
        {
            return new SceneDocumentWriteResult(
                SceneDocumentWriteCompletion.Failed,
                mutationBlockedReason);
        }
        using SceneSourceSaveScope mutationLease = mutationScope!;
        if (!mutationLease.TryAcquireProjectAssetMutationLock(
                out mutationBlockedReason))
        {
            return new SceneDocumentWriteResult(
                SceneDocumentWriteCompletion.Failed,
                mutationBlockedReason);
        }

        try
        {
            target = ValidateSceneDocumentPath(target, use3D);
            string text = use3D
                ? EngineInterop.Scene3DText(Engine)
                : EngineInterop.SceneText(Engine);
            if (_project != null)
            {
                SceneSourceFile.WriteProjectSceneAtomicText(
                    target,
                    text,
                    _project.RootDir,
                    _project.AssetsDir,
                    mode);
            }
            else
            {
                SceneSourceFile.WriteAtomicText(
                    target,
                    text,
                    expectedMode: mode);
            }
            await CommitSavedSceneDocumentAsync(mode, previousPath, target, text);
            return new SceneDocumentWriteResult(SceneDocumentWriteCompletion.Saved, "");
        }
        catch (Exception ex)
        {
            return new SceneDocumentWriteResult(
                SceneDocumentWriteCompletion.Failed, ex.Message);
        }
    }

    private async Task CommitSavedSceneDocumentAsync(
        SceneDocumentMode mode,
        string? previousPath,
        string target,
        string serializedScene)
    {
        bool use3D = mode == SceneDocumentMode.ThreeD;
        if (_view3d == use3D)
        {
            SetCurrentScenePath(target);
            MarkSceneClean(serializedScene);
        }
        else
        {
            string snapshot = NormalizeSceneSnapshot(serializedScene);
            if (use3D)
            {
                _scene3DDocumentPath = target;
                _scene3DSavedSnapshot = snapshot;
                _scene3DDirty = false;
            }
            else
            {
                _scene2DPath = target;
                _scene2DSavedSnapshot = snapshot;
                _scene2DDirty = false;
            }
        }

        NotifySceneDocumentSaved(use3D, target);
        await OnSceneSourceSavedAsync(use3D, previousPath, target);
    }

    private static string FormatSaveAllResult(SaveAllResult result)
    {
        if (result.Completion == SaveAllCompletion.Success)
            return result.SavedCount == 0
                ? "Save All: no dirty scene sources."
                : $"Save All: saved {result.SavedCount} legacy scene " +
                  (result.SavedCount == 1 ? "source." : "sources.");

        string mode = result.StoppedDocument is { } stopped
            ? ModeLabel(stopped)
            : "scene";
        string progress = result.PlannedCount == 0
            ? ""
            : $" ({result.SavedCount}/{result.PlannedCount} saved)";
        return result.Completion == SaveAllCompletion.Cancelled
            ? $"Save All cancelled at {mode}{progress}."
            : $"Save All failed at {mode}{progress}: {result.Detail}";
    }

    private static string ModeLabel(SceneDocumentMode mode) =>
        mode == SceneDocumentMode.ThreeD ? ".acs3d source" : ".acscene source";
}
