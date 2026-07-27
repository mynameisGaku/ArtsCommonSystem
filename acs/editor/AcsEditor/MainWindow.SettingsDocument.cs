// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Input;

namespace AcsEditor;

public partial class MainWindow
{
    private EditorDocument? _projectSettingsDocument;
    private EditorDocumentState? _initialProjectSettingsDocumentState;
    private bool _initialProjectSettingsDocumentInitiallySaved = true;
    private readonly ProjectSettingsPersistenceGate
        _projectSettingsPersistenceGate = new();

    private EditorDocumentState CaptureProjectSettingsDocumentState()
    {
        if (Engine == IntPtr.Zero)
            throw new InvalidOperationException("The editor engine is unavailable.");
        string canonical = ProjectSettingsSerialization.Capture(
            buffer => EngineInterop.acs_editor_settings_serialize(
                Engine,
                buffer,
                buffer.Length));
        return _projectSettingsPersistenceGate.ProtectCapture(
            ProjectSettingsDocumentContract.CreateState(canonical));
    }

    private void RestoreProjectSettingsDocumentState(EditorDocumentState state)
    {
        ArgumentNullException.ThrowIfNull(state);
        if (Engine == IntPtr.Zero)
            throw new InvalidOperationException("The editor engine is unavailable.");
        if (!string.Equals(
                state.Payload,
                state.ContentFingerprint,
                StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "The settings history payload does not match its durable fingerprint.");
        }

        ProjectSettingsDocumentContract.Parse(state.Payload);
        EditorDocumentState rollback = CaptureProjectSettingsDocumentState();
        try
        {
            EngineInterop.acs_editor_settings_load_text(Engine, "");
            EngineInterop.acs_editor_settings_load_text(Engine, state.Payload);
            EditorDocumentState restored = CaptureProjectSettingsDocumentState();
            ProjectSettingsDocumentContract.EnsureSourceEntriesPreserved(
                state.Payload,
                restored.Payload);
            if (!string.Equals(
                    restored.ContentFingerprint,
                    state.ContentFingerprint,
                    StringComparison.Ordinal))
            {
                throw new InvalidDataException(
                    "The native settings restore did not reproduce the transaction snapshot.");
            }
        }
        catch (Exception restoreError)
        {
            try
            {
                EngineInterop.acs_editor_settings_load_text(Engine, "");
                EngineInterop.acs_editor_settings_load_text(Engine, rollback.Payload);
                EditorDocumentState rolledBack =
                    CaptureProjectSettingsDocumentState();
                ProjectSettingsDocumentContract.EnsureSourceEntriesPreserved(
                    rollback.Payload,
                    rolledBack.Payload);
                if (!string.Equals(
                        rollback.ContentFingerprint,
                        rolledBack.ContentFingerprint,
                        StringComparison.Ordinal))
                {
                    throw new InvalidDataException(
                        "The native settings rollback did not reproduce its checkpoint.");
                }
            }
            catch (Exception rollbackError)
            {
                string reason =
                    "Project Settings restore failed and its native rollback could not be " +
                    "verified. Persistence is blocked until ProjectSettings.ini is repaired " +
                    "and reloaded.";
                _projectSettingsPersistenceGate.Latch(
                    reason,
                    nativeStateIsUncertain: true);
                _initialProjectSettingsDocumentInitiallySaved = false;
                Log(reason, "Settings", LogLevel.Error);
                throw new InvalidOperationException(
                    reason,
                    new AggregateException(restoreError, rollbackError));
            }
            throw;
        }

        SynchronizeProjectSettingsChrome();
    }

    private EditorDocument EnsureProjectSettingsDocumentRegistered(
        EditorDocumentState? initialState = null,
        bool? initiallySaved = null)
    {
        if (_project == null)
            throw new InvalidOperationException(
                "A project is required for the Project Settings document.");
        if (_projectSettingsDocument != null)
            return _projectSettingsDocument;

        EditorDocumentState state =
            initialState ??
            _initialProjectSettingsDocumentState ??
            CaptureProjectSettingsDocumentState();
        EditorDocument candidate = ProjectSettingsDocumentRegistration.Create(
            SettingsIniPath,
            CaptureProjectSettingsDocumentState,
            RestoreProjectSettingsDocumentState,
            SaveProjectSettingsDocumentThroughHostAsync,
            initiallySaved ??
                _initialProjectSettingsDocumentInitiallySaved,
            state);
        _projectSettingsDocument = _documentHost.Register(candidate);
        return _projectSettingsDocument;
    }

    private async ValueTask<EditorDocumentSaveResult>
        SaveProjectSettingsDocumentThroughHostAsync(
            CancellationToken cancellationToken)
    {
        if (_projectSettingsPersistenceGate.BlockedReason is string blockedReason)
        {
            return EditorDocumentSaveResult.Failed(
                blockedReason);
        }
        if (Engine == IntPtr.Zero || _project == null)
        {
            return EditorDocumentSaveResult.Failed(
                "The project settings engine or project is unavailable.");
        }

        EditorDocumentState committed;
        Project project = _project;
        string expectedProjectInitialScene = project.InitialScene;
        try
        {
            cancellationToken.ThrowIfCancellationRequested();
            // Native serialization belongs to the UI/engine thread. Only the bounded filesystem
            // transaction moves to a worker, so Save All does not freeze editor input.
            committed = CaptureProjectSettingsDocumentState();
            ProjectSettingsSaveCommit persisted = await Task.Run(
                () => ProjectManager.SaveProjectSettings(
                    project,
                    committed.Payload),
                cancellationToken);
            EditorDocumentState durable =
                ProjectSettingsDocumentContract.CreateState(
                    persisted.DurableSettingsSource);
            Dispatcher.VerifyAccess();
            EditorDocumentSaveResult convergence =
                ProjectSettingsDurableConvergence.Converge(
                committed,
                durable,
                CaptureProjectSettingsDocumentState,
                RestoreProjectSettingsDocumentState);
            if (convergence.Status != EditorDocumentSaveStatus.Saved)
                return convergence;
            if (!ProjectSettingsProjectReferenceConvergence.TryReconcile(
                    project,
                    expectedProjectInitialScene,
                    persisted.AuthoritativeInitialScene,
                    durable,
                    out string referenceError))
            {
                return EditorDocumentSaveResult.Failed(referenceError);
            }
            return convergence;
        }
        catch (OperationCanceledException)
        {
            return EditorDocumentSaveResult.Cancelled(
                "Project Settings save was cancelled.");
        }
        catch (Exception error)
        {
            return EditorDocumentSaveResult.Failed(
                "Project Settings write failed: " + error.Message);
        }
    }

    private bool CanEditProjectSettings()
    {
        if (_project == null)
            return true;
        if (!_documentHostInitialized)
        {
            Log(
                "Project Settings cannot be changed before Document Host initialization.",
                "Settings",
                LogLevel.Error);
            return false;
        }
        if (_building)
        {
            Log(
                "Project Settings cannot be changed while Build, Run, or Package owns " +
                "the durability boundary.",
                "Settings",
                LogLevel.Warn);
            return false;
        }
        if (_projectSettingsDocument is
            {
                IsSuspended: true
            } or
            {
                IsInTransaction: true
            })
        {
            Log(
                "Project Settings cannot be changed while its hosted document is " +
                "suspended or has an open transaction.",
                "Settings",
                LogLevel.Error);
            return false;
        }
        if (!_projectSettingsPersistenceGate.IsBlocked)
            return true;

        Log(
            "Project Settings are read-only until ProjectSettings.ini is repaired and " +
            "reloaded: " + _projectSettingsPersistenceGate.BlockedReason,
            "Settings",
            LogLevel.Error);
        return false;
    }

    private bool RecordProjectSettingsChange(
        string label,
        string? mergeKey = null)
    {
        if (_project == null ||
            !_documentHostInitialized ||
            !CanEditProjectSettings())
        {
            return false;
        }

        EditorDocument document =
            EnsureProjectSettingsDocumentRegistered();
        if (!ReferenceEquals(_documentHost.ActiveDocument, document))
            _documentHost.Activate(document.Id);
        document.NotifyPotentialChange();
        bool changed = document.Synchronize(
            label,
            mergeKey,
            mergeKey == null ? TimeSpan.Zero : TimeSpan.FromSeconds(1));
        if (!changed)
            return false;
        SynchronizeProjectSettingsChrome();
        CommandManager.InvalidateRequerySuggested();
        return true;
    }

    private bool TryApplyProjectSettingsMutation(
        string label,
        string? mergeKey,
        Func<bool> apply)
    {
        ArgumentNullException.ThrowIfNull(apply);
        if (_project == null || !CanEditProjectSettings())
            return false;

        EditorDocumentState before;
        try
        {
            before = CaptureProjectSettingsDocumentState();
        }
        catch (Exception captureError)
        {
            LatchProjectSettingsPersistence(
                "Project Settings could not capture a verified pre-mutation snapshot: " +
                captureError.Message);
            return false;
        }

        ProjectSettingsMutationResult result =
            ProjectSettingsMutationAdmission.TryApply(
                CanEditProjectSettings,
                apply,
                () => RecordProjectSettingsChange(label, mergeKey),
                () =>
                {
                    RestoreProjectSettingsDocumentState(before);
                    if (_projectSettingsDocument is
                        {
                            IsSuspended: false,
                            IsInTransaction: false
                        } document)
                    {
                        document.Synchronize("Rollback Project Settings mutation");
                    }
                    return true;
                });
        if (!result.Succeeded)
        {
            if (result.NativeStateUncertain)
            {
                LatchProjectSettingsPersistence(result.Detail);
            }
            else if (!string.IsNullOrWhiteSpace(result.Detail))
            {
                Log(result.Detail, "Settings", LogLevel.Warn);
            }
            SynchronizeProjectSettingsChrome();
        }
        return result.Succeeded;
    }

    private void LatchProjectSettingsPersistence(string reason)
    {
        string detail =
            string.IsNullOrWhiteSpace(reason)
                ? "Project Settings native state is uncertain."
                : reason.Trim();
        _projectSettingsPersistenceGate.Latch(
            detail,
            nativeStateIsUncertain: true);
        _initialProjectSettingsDocumentInitiallySaved = false;
        _projectSettingsDocument?.NotifyPotentialChange();
        Log(
            detail +
            " Editing and persistence are blocked until a verified source reload.",
            "Settings",
            LogLevel.Error);
        CommandManager.InvalidateRequerySuggested();
    }

    private void SynchronizeProjectSettingsChrome()
    {
        try
        {
            SyncAaCombo();
            SyncQualityMenu();
            SynchronizeSnapSettingsFromProject();
        }
        catch (Exception error)
        {
            try
            {
                Log(
                    "Project Settings UI synchronization failed: " +
                    error.Message,
                    "Settings",
                    LogLevel.Error);
            }
            catch
            {
                // Chrome is an observer. It cannot invalidate a verified native restore or
                // committed settings transaction.
            }
        }
    }

    private async Task<bool> SaveProjectSettingsForBuildAsync(
        CancellationToken cancellationToken) =>
        await SaveProjectSettingsCheckpointAsync(cancellationToken) != null;

    private async Task<ProjectSettingsDurabilityCheckpoint?>
        SaveProjectSettingsCheckpointAsync(
            CancellationToken cancellationToken)
    {
        if (!_documentHostInitialized || _projectSettingsDocument == null)
        {
            BuildLog(
                "Project Settings save failed: Document Host is unavailable. " +
                "Build/Run/Package was aborted.");
            return null;
        }

        EditorDocumentSaveResult result =
            await ProjectSettingsBuildDurabilityGate.SaveAsync(
                _documentHost,
                _projectSettingsDocument,
                cancellationToken);
        if (result.Status == EditorDocumentSaveStatus.Saved)
        {
            try
            {
                Dispatcher.VerifyAccess();
                if (_projectSettingsDocument.IsDirty)
                {
                    throw new InvalidOperationException(
                        "Project Settings remained dirty after the durable host save.");
                }
                if (result.SavedFingerprint == null)
                {
                    throw new InvalidOperationException(
                        "The durable host save did not return its exact published Settings source.");
                }
                return ProjectSettingsDurabilityCheckpoint.Create(
                    result.SavedFingerprint);
            }
            catch (Exception error)
            {
                BuildLog(
                    "Project Settings checkpoint capture failed: " +
                    error.Message +
                    " Build/Run/Package was aborted.");
                return null;
            }
        }

        string detail = string.IsNullOrWhiteSpace(result.Detail)
            ? "No durable Project Settings checkpoint was produced."
            : result.Detail;
        BuildLog(
            "Project Settings save failed: " + detail +
            " Build/Run/Package was aborted.");
        return null;
    }

    private void ApplyPersistedInitialSceneReference(
        string sceneReference,
        string durableSettingsSource)
    {
        if (_projectSettingsDocument?.IsDirty != true)
        {
            _projectSettingsLoadGeneration.Invalidate();
            IReadOnlyDictionary<ProjectSettingKey, string> durableEntries =
                ProjectSettingsDocumentContract.Parse(
                    durableSettingsSource);
            if (!durableEntries.TryGetValue(
                    new ProjectSettingKey("Game", "DefaultScene"),
                    out string? durableReference) ||
                !string.Equals(
                    durableReference,
                    sceneReference,
                    StringComparison.Ordinal))
            {
                throw new InvalidDataException(
                    "The committed Settings snapshot does not contain the new startup-scene reference.");
            }

            ApplyProjectSettingsSourceOrDefaults(
                durableSettingsSource,
                sourceError: null);
            if (_projectSettingsPersistenceGate.IsBlocked ||
                !_initialProjectSettingsDocumentInitiallySaved ||
                _projectSettingsDocument?.IsDirty == true)
            {
                throw new InvalidOperationException(
                    "The committed Settings snapshot could not become the verified native baseline.");
            }
            return;
        }
        if (!TryApplyProjectSettingsMutation(
                "Update startup scene",
                "settings.Game.DefaultScene",
                () => EngineInterop.acs_editor_settings_set(
                    Engine,
                    "Game",
                    "DefaultScene",
                    sceneReference) != 0))
        {
            throw new InvalidOperationException(
                "The in-memory Game.DefaultScene setting could not follow the moved scene.");
        }
    }
}
