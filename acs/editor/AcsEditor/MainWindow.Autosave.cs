// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Threading;

namespace AcsEditor;

public partial class MainWindow
{
    private sealed class AutosaveTracker
    {
        internal string DocumentId = "";
        internal string ObservedHash = "";
        internal string LastWrittenHash = "";
        internal DateTimeOffset LastContentChangeUtc = DateTimeOffset.MinValue;
        internal DateTimeOffset LastErrorUtc = DateTimeOffset.MinValue;
        internal readonly AutosaveGenerationGate Gate = new();

        internal void ResetState(string documentId = "")
        {
            DocumentId = documentId;
            ObservedHash = "";
            LastWrittenHash = "";
            LastContentChangeUtc = DateTimeOffset.MinValue;
        }
    }

    private static readonly TimeSpan AutosaveDebounce = TimeSpan.FromSeconds(4);
    private readonly DispatcherTimer _autosaveTimer = new()
    {
        Interval = TimeSpan.FromSeconds(2),
    };
    private readonly AutosaveTracker _autosave2D = new();
    private readonly AutosaveTracker _autosave3D = new();
    private readonly HashSet<string> _recoveryPromptedDocuments = new(StringComparer.Ordinal);
    private readonly DateTimeOffset _autosaveSessionStartedUtc = DateTimeOffset.UtcNow;
    private readonly CancellationTokenSource _autosaveCancellation = new();
    private readonly SemaphoreSlim _autosaveMaintenanceLock = new(1, 1);
    private SceneAutosaveStore? _autosaveStore;
    private SceneRecoveryCandidate? _pendingInitialRecoveryCandidate;
    private Task<SceneRecoveryDecision>? _activeRecoveryPromptTask;
    private bool _autosaveStopping;
    private bool _autosaveInitialized;
    private bool _pendingInitialRecoveryActivationHooked;

    internal static bool ShouldDeferInitialRecoveryPrompt(
        bool initialActivationSuppressed,
        bool windowIsActive,
        bool allowWhileInactive = false) =>
        initialActivationSuppressed && !windowIsActive && !allowWhileInactive;

    /// <summary>
    /// Starts recovery discovery and autosave after the native scene baseline has been captured.
    /// File discovery/checksum work runs off the dispatcher; a recovery prompt is never shown in a
    /// non-interactive process.
    /// </summary>
    private async void InitializeAutosaveAndRecovery()
    {
        // --unattended runs are read-only visual validation. Never
        // interrupt them with a modal recovery prompt or create a recovery
        // snapshot merely because the validator process is terminated.
        if (_autosaveInitialized || _project == null ||
            !Environment.UserInteractive || App.IsNonInteractiveLaunch)
            return;
        _autosaveInitialized = true;
        try
        {
            _autosaveStore = new SceneAutosaveStore();
            bool initialMode3D = _view3d;
            SceneAutosaveIdentity identity = AutosaveIdentity(initialMode3D);
            SceneRecoveryCandidate? candidate = await Task.Run(
                () => _autosaveStore.FindLatest(identity),
                _autosaveCancellation.Token);

            if (candidate != null &&
                CanResolveInitialRecoveryCandidate(candidate, initialMode3D))
            {
                if (ShouldDeferInitialRecoveryPrompt(
                        App.IsInitialActivationSuppressed,
                        IsActive,
                        App.IsInactiveRecoveryPromptAllowed))
                    DeferInitialRecoveryCandidateUntilActivation(candidate);
                else
                    await ResolveRecoveryCandidateAsync(candidate);
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception ex)
        {
            Log("Autosave initialization failed: " + ex.Message);
        }
        finally
        {
            if (!_autosaveStopping && _autosaveStore != null)
            {
                _autosaveTimer.Tick -= OnAutosaveTick;
                _autosaveTimer.Tick += OnAutosaveTick;
                _autosaveTimer.Start();
            }
        }
    }

    private void StopAutosave()
    {
        _autosaveStopping = true;
        ClearPendingInitialRecoveryCandidate();
        _autosaveTimer.Stop();
        _autosaveCancellation.Cancel();
        // Closed is only a fallback; normal close awaits StopAndDiscardSessionRecoveriesAsync.
        // Starting invalidation here still prevents any new worker if shutdown is forced.
        _ = _autosave2D.Gate.InvalidateAndWaitAsync();
        _ = _autosave3D.Gate.InvalidateAndWaitAsync();
    }

    private bool CanResolveInitialRecoveryCandidate(
        SceneRecoveryCandidate candidate,
        bool expectedMode3D)
    {
        return !_autosaveStopping &&
               _project != null &&
               _view3d == expectedMode3D &&
               candidate.Identity.Mode ==
                   (expectedMode3D ? SceneDocumentMode.ThreeD : SceneDocumentMode.TwoD) &&
               !_sceneDirty &&
               string.Equals(
                   AutosaveIdentity(expectedMode3D).DocumentId,
                   candidate.Identity.DocumentId,
                   StringComparison.Ordinal);
    }

    private void DeferInitialRecoveryCandidateUntilActivation(
        SceneRecoveryCandidate candidate)
    {
        if (_autosaveStopping || _pendingInitialRecoveryCandidate != null)
            return;

        _pendingInitialRecoveryCandidate = candidate;
        if (_pendingInitialRecoveryActivationHooked) return;

        _pendingInitialRecoveryActivationHooked = true;
        Activated += OnInitialRecoveryActivation;
    }

    private async void OnInitialRecoveryActivation(object? sender, EventArgs e)
    {
        SceneRecoveryCandidate? candidate = TakePendingInitialRecoveryCandidate();
        if (candidate == null) return;

        bool candidateMode3D =
            candidate.Identity.Mode == SceneDocumentMode.ThreeD;
        if (!CanResolveInitialRecoveryCandidate(candidate, candidateMode3D))
            return;

        try
        {
            // Activated is raised from WM_ACTIVATE. Do not create another
            // top-level owned HWND inside that native activation callback.
            // A dispatcher turn also lets the initial no-activate guard finish
            // removing WS_EX_NOACTIVATE before the prompt is presented.
            await Dispatcher.Yield(DispatcherPriority.Background);
            if (!CanResolveInitialRecoveryCandidate(candidate, candidateMode3D))
                return;
            await ResolveRecoveryCandidateAsync(candidate);
        }
        catch (Exception ex)
        {
            Log("Scene recovery failed after editor activation: " + ex.Message);
        }
    }

    private SceneRecoveryCandidate? TakePendingInitialRecoveryCandidate()
    {
        if (_pendingInitialRecoveryActivationHooked)
        {
            Activated -= OnInitialRecoveryActivation;
            _pendingInitialRecoveryActivationHooked = false;
        }

        SceneRecoveryCandidate? candidate = _pendingInitialRecoveryCandidate;
        _pendingInitialRecoveryCandidate = null;
        return candidate;
    }

    private void ClearPendingInitialRecoveryCandidate() =>
        _ = TakePendingInitialRecoveryCandidate();

    private void OnAutosaveTick(object? sender, EventArgs e)
    {
        if (_autosaveStopping || _autosaveStore == null || Engine == IntPtr.Zero) return;
        TryStartAutosaveMode(use3D: false);
        TryStartAutosaveMode(use3D: true);
    }

    private void TryStartAutosaveMode(bool use3D)
    {
        AutosaveTracker tracker = use3D ? _autosave3D : _autosave2D;
        bool initialized = use3D ? _scene3DInitialized : _scene2DInitialized;
        bool dirty = use3D ? _scene3DDirty : _scene2DDirty;
        if (!initialized || !dirty ||
            _autosaveStore == null || _project == null || _autosaveStopping)
            return;

        tracker.Gate.TryStart(
            generationToken => AutosaveModeAsync(use3D, tracker, generationToken),
            out _);
    }

    private async Task AutosaveModeAsync(
        bool use3D,
        AutosaveTracker tracker,
        CancellationToken generationToken)
    {
        using var linkedCancellation = CancellationTokenSource.CreateLinkedTokenSource(
            generationToken,
            _autosaveCancellation.Token);
        CancellationToken cancellationToken = linkedCancellation.Token;
        try
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (_autosaveStopping || _autosaveStore == null || _project == null)
                return;

            SceneAutosaveIdentity identity = AutosaveIdentity(use3D);
            if (!string.Equals(tracker.DocumentId, identity.DocumentId, StringComparison.Ordinal))
                tracker.ResetState(identity.DocumentId);

            // Native serializers are owned by the UI/native-host thread. Capture quickly here;
            // normalization, hashing and disk writes happen on a worker.
            string content = use3D
                ? EngineInterop.Scene3DText(Engine)
                : EngineInterop.SceneText(Engine);

            string stableHash = await Task.Run(
                () => SceneAutosaveStore.ComputeContentSha256(
                    NormalizeSceneSnapshot(content)),
                cancellationToken);
            cancellationToken.ThrowIfCancellationRequested();
            if (_autosaveStopping) return;

            DateTimeOffset now = DateTimeOffset.UtcNow;
            if (!string.Equals(stableHash, tracker.ObservedHash, StringComparison.Ordinal))
            {
                tracker.ObservedHash = stableHash;
                tracker.LastContentChangeUtc = now;
                return;
            }
            if (now - tracker.LastContentChangeUtc < AutosaveDebounce ||
                string.Equals(stableHash, tracker.LastWrittenHash, StringComparison.Ordinal))
                return;
            bool stillDirty = use3D ? _scene3DDirty : _scene2DDirty;
            if (!stillDirty) return;
            cancellationToken.ThrowIfCancellationRequested();

            var capture = new SceneAutosaveCapture(identity, content, now);
            await Task.Run(
                () => _autosaveStore.WriteSnapshot(capture, cancellationToken),
                cancellationToken);
            cancellationToken.ThrowIfCancellationRequested();
            if (_autosaveStopping) return;

            tracker.LastWrittenHash = stableHash;
            StatusText.Text =
                $"Recovery saved · {(use3D ? ".acs3d" : ".acscene")} · {now.ToLocalTime():T}";
        }
        catch (OperationCanceledException) { }
        catch (Exception ex)
        {
            LogAutosaveError(tracker, "snapshot write", ex);
        }
    }

    private void LogAutosaveError(AutosaveTracker tracker, string operation, Exception ex)
    {
        DateTimeOffset now = DateTimeOffset.UtcNow;
        if (now - tracker.LastErrorUtc < TimeSpan.FromSeconds(30)) return;
        tracker.LastErrorUtc = now;
        Log($"Autosave {operation} failed: {ex.Message}");
    }

    private SceneAutosaveIdentity AutosaveIdentity(bool use3D)
    {
        if (_project == null) throw new InvalidOperationException("Autosave requires a project.");
        string? path = use3D ? _scene3DDocumentPath : _scene2DPath;
        if (_view3d == use3D)
            path = _currentScenePath;
        return AutosaveIdentityForPath(use3D, path);
    }

    private SceneAutosaveIdentity AutosaveIdentityForPath(bool use3D, string? path)
    {
        if (_project == null) throw new InvalidOperationException("Autosave requires a project.");
        return SceneAutosaveStore.CreateIdentity(
            _project.ProjectFilePath,
            path,
            use3D ? SceneDocumentMode.ThreeD : SceneDocumentMode.TwoD);
    }

    /// <summary>Finds recovery for an explicit Open target without changing the current document.</summary>
    private SceneRecoveryCandidate? FindRecoveryForOpen(string path, bool use3D)
    {
        if (_autosaveStore == null || _project == null) return null;
        try { return _autosaveStore.FindLatest(AutosaveIdentityForPath(use3D, path)); }
        catch (Exception ex)
        {
            Log("Recovery discovery failed: " + ex.Message);
            return null;
        }
    }

    private async Task<SceneRecoveryDecision> PromptRecoveryAsync(
        SceneRecoveryCandidate candidate)
    {
        // Only one recovery choice can be actionable at a time. Because the
        // prompt is deliberately modeless, commands remain available and an
        // explicit Open could otherwise stack another owned prompt.
        if (_activeRecoveryPromptTask is { IsCompleted: false })
        {
            Log(
                "A scene recovery choice is already open. Finish or close it before opening another recovery.",
                "Scene",
                LogLevel.Warn);
            return SceneRecoveryDecision.Cancel;
        }

        StatusText.Text =
            "Recovery snapshot available - choose Recover, Discard, or close the recovery window.";
        Log(
            "Scene recovery is available. The recovery window is non-modal; the editor remains interactive.",
            "Scene",
            LogLevel.Info);

        Task<SceneRecoveryDecision> prompt =
            SceneRecoveryDialog.PromptAsync(this, candidate);
        _activeRecoveryPromptTask = prompt;
        try
        {
            return await prompt;
        }
        finally
        {
            if (ReferenceEquals(_activeRecoveryPromptTask, prompt))
                _activeRecoveryPromptTask = null;
        }
    }

    /// <summary>
    /// Applies verified recovery over an already loaded source baseline. The original scene path
    /// remains active and the recovered document remains dirty until an explicit source save.
    /// </summary>
    private async Task<bool> ApplyRecoveryCandidateAsync(SceneRecoveryCandidate candidate)
    {
        if (_autosaveStore == null || Engine == IntPtr.Zero) return false;
        bool use3D = candidate.Identity.Mode == SceneDocumentMode.ThreeD;
        if (_view3d != use3D)
        {
            Log("Recovery source format does not match the loaded scene source.");
            return false;
        }
        if (!string.Equals(
                AutosaveIdentity(use3D).DocumentId,
                candidate.Identity.DocumentId,
                StringComparison.Ordinal))
        {
            Log("Recovery identity does not match the active scene document.");
            return false;
        }

        await _autosaveMaintenanceLock.WaitAsync();
        AutosaveTracker tracker = use3D ? _autosave3D : _autosave2D;
        try
        {
            // Invalidation is synchronous up to its first await: no new writer can start while the
            // old generation drains. Reading recovery happens off the dispatcher.
            await tracker.Gate.InvalidateAndWaitAsync();
            string content = await Task.Run(
                () => _autosaveStore.ReadVerifiedContent(candidate));

            // The user may edit while discovery, prompting, worker drain, or checksum I/O awaits.
            // Revalidate identity and serialize native state now, immediately before replacement.
            if (_view3d != use3D ||
                !string.Equals(
                    AutosaveIdentity(use3D).DocumentId,
                    candidate.Identity.DocumentId,
                    StringComparison.Ordinal))
            {
                Log("Recovery target changed while it was being prepared; recovery was kept.");
                return false;
            }
            if (!CanApplyRecoveryToFreshNativeState(out string dirtyReason))
            {
                Log("Recovery was not applied: " + dirtyReason + " Recovery was kept.");
                return false;
            }

            bool loaded =
                LoadLegacySceneSourceAsDocument(Engine, use3D, content);
            if (!loaded)
            {
                Log("Recovery snapshot format was rejected; the source scene remains loaded.");
                return false;
            }

            if (use3D) _scene3DInitialized = true;
            else _scene2DInitialized = true;
            SetCurrentScenePath(candidate.Identity.OriginalPath);
            RefreshAfterSceneChange();
            MarkSceneDirty();
            RememberActiveSceneDocumentState();
            ResetSceneDocumentHistory(use3D, markSaved: false);

            string stableHash = SceneAutosaveStore.ComputeContentSha256(
                NormalizeSceneSnapshot(content));
            tracker.ResetState(candidate.Identity.DocumentId);
            tracker.ObservedHash = stableHash;
            tracker.LastWrittenHash = stableHash;
            tracker.LastContentChangeUtc = candidate.CapturedUtc;
            Log(
                $"Recovered unsaved scene source ({(use3D ? ".acs3d" : ".acscene")}) " +
                $"from {candidate.CapturedUtc.ToLocalTime():g}.");
            return true;
        }
        catch (Exception ex)
        {
            Log("Scene recovery failed: " + ex.Message);
            return false;
        }
        finally
        {
            if (!_autosaveStopping)
                tracker.Gate.Resume();
            _autosaveMaintenanceLock.Release();
        }
    }

    /// <summary>
    /// A cached 750 ms dirty flag is insufficient at this destructive boundary. Capture the
    /// dispatcher-owned native graph and compare it with the persisted clean baseline now.
    /// </summary>
    private bool CanApplyRecoveryToFreshNativeState(out string reason)
    {
        Dispatcher.VerifyAccess();
        reason = "";
        if (Engine == IntPtr.Zero || _savedSceneSnapshot == null)
        {
            SetSceneDirty(true);
            reason = "the current scene has no verified clean baseline.";
            return false;
        }
        if (EngineInterop.acs_editor_play_state(Engine) != 0 || PreviewBtn.IsChecked == true)
        {
            reason = "stop Play/Preview before applying crash recovery.";
            return false;
        }

        try
        {
            string native = NormalizeSceneSnapshot(
                _view3d
                    ? EngineInterop.Scene3DText(Engine)
                    : EngineInterop.SceneText(Engine));
            bool dirty = !RecoveryApplyGuard.CanReplace(_savedSceneSnapshot, native);
            _snapshotCaptureFailed = false;
            SetSceneDirty(dirty);
            _sceneMutationRevision.AcknowledgeWorkspace();
            RememberActiveSceneTrackingState();
            if (!dirty) return true;

            reason = "the active scene changed after recovery discovery.";
            return false;
        }
        catch (Exception ex)
        {
            _snapshotCaptureFailed = true;
            SetSceneDirty(true);
            RememberActiveSceneTrackingState();
            reason = "the latest native scene state could not be verified: " + ex.Message;
            return false;
        }
    }

    private async Task ResolveRecoveryCandidateAsync(SceneRecoveryCandidate candidate)
    {
        if (!_recoveryPromptedDocuments.Add(candidate.Identity.DocumentId))
            return;

        switch (await PromptRecoveryAsync(candidate))
        {
            case SceneRecoveryDecision.Recover:
                await ApplyRecoveryCandidateAsync(candidate);
                break;
            case SceneRecoveryDecision.Discard:
                await DiscardRecoveryAsync(candidate.Identity);
                Log("Scene recovery discarded; source scene was not modified.");
                break;
            default:
                Log("Scene recovery kept for a later startup.");
                break;
        }
    }

    /// <summary>
    /// A mode that was not the startup document is discovered on first visit. Snapshots created by
    /// this running editor are already represented in native memory and must not prompt as crashes.
    /// </summary>
    private async void DiscoverRecoveryForActiveDocument()
    {
        if (_autosaveStore == null || _project == null || _autosaveStopping || _sceneDirty) return;
        SceneAutosaveIdentity identity = AutosaveIdentity(_view3d);
        if (_recoveryPromptedDocuments.Contains(identity.DocumentId)) return;
        try
        {
            SceneRecoveryCandidate? candidate = _autosaveStore.FindLatest(identity);
            if (candidate != null && candidate.CapturedUtc < _autosaveSessionStartedUtc)
                await ResolveRecoveryCandidateAsync(candidate);
        }
        catch (Exception ex) { Log("Recovery discovery failed: " + ex.Message); }
    }

    private async Task DiscardRecoveryAsync(
        SceneAutosaveIdentity identity,
        bool resumeAfter = true)
    {
        if (_autosaveStore == null) return;
        bool use3D = identity.Mode == SceneDocumentMode.ThreeD;
        AutosaveTracker tracker = use3D ? _autosave3D : _autosave2D;
        await _autosaveMaintenanceLock.WaitAsync();
        try
        {
            await tracker.Gate.InvalidateAndWaitAsync();
            tracker.ResetState();
            await Task.Run(() => _autosaveStore.Discard(identity));
        }
        catch (Exception ex)
        {
            Log("Recovery discard failed: " + ex.Message);
        }
        finally
        {
            if (resumeAfter && !_autosaveStopping)
                tracker.Gate.Resume();
            _autosaveMaintenanceLock.Release();
        }
    }

    /// <summary>Called only after a source scene write succeeds.</summary>
    private async Task OnSceneSourceSavedAsync(
        bool use3D,
        string? previousPath,
        string? savedPath)
    {
        AutosaveTracker tracker = use3D ? _autosave3D : _autosave2D;
        if (_autosaveStore == null || _project == null)
        {
            tracker.ResetState();
            return;
        }

        var identities = new Dictionary<string, SceneAutosaveIdentity>(StringComparer.Ordinal);
        foreach (string? path in new[] { previousPath, savedPath })
        {
            SceneAutosaveIdentity identity = AutosaveIdentityForPath(use3D, path);
            identities[identity.DocumentId] = identity;
        }
        await _autosaveMaintenanceLock.WaitAsync();
        try
        {
            await tracker.Gate.InvalidateAndWaitAsync();
            tracker.ResetState();
            // Explicit source Save is the durability boundary: stale recovery must be gone before
            // the async command completes. At most two bounded identities are removed off-UI.
            await Task.Run(() =>
            {
                foreach (SceneAutosaveIdentity identity in identities.Values)
                    _autosaveStore.Discard(identity);
            });
        }
        catch (Exception ex)
        {
            Log("Saved scene, but stale recovery cleanup failed: " + ex.Message);
        }
        finally
        {
            if (!_autosaveStopping)
                tracker.Gate.Resume();
            _autosaveMaintenanceLock.Release();
        }
    }

    /// <summary>
    /// Graceful close first suppresses/cancels and joins both workers, then removes recoveries.
    /// No old WriteSnapshot can run after the discard phase.
    /// </summary>
    private async Task StopAndDiscardSessionRecoveriesAsync()
    {
        await _autosaveMaintenanceLock.WaitAsync();
        try
        {
            _autosaveStopping = true;
            ClearPendingInitialRecoveryCandidate();
            _autosaveTimer.Stop();
            _autosaveCancellation.Cancel();
            await Task.WhenAll(
                _autosave2D.Gate.InvalidateAndWaitAsync(),
                _autosave3D.Gate.InvalidateAndWaitAsync());
            _autosave2D.ResetState();
            _autosave3D.ResetState();

            if (_autosaveStore == null || _project == null) return;
            var identities = new List<SceneAutosaveIdentity>(2);
            if (_scene2DInitialized) identities.Add(AutosaveIdentity(use3D: false));
            if (_scene3DInitialized) identities.Add(AutosaveIdentity(use3D: true));
            await Task.Run(() =>
            {
                foreach (SceneAutosaveIdentity identity in identities)
                    _autosaveStore.Discard(identity);
            });
        }
        catch (Exception ex) { Log("Recovery cleanup failed: " + ex.Message); }
        finally { _autosaveMaintenanceLock.Release(); }
    }

    private void ResumeAutosaveAfterReplacement(bool use3D)
    {
        AutosaveTracker tracker = use3D ? _autosave3D : _autosave2D;
        tracker.ResetState();
        if (!_autosaveStopping)
            tracker.Gate.Resume();
    }
}
