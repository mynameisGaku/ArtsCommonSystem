// SPDX-License-Identifier: Apache-2.0

using System;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace AcsEditor;

/// <summary>
/// One-way safety latch cleared only by a fully verified source load. In particular, a failed
/// history restore whose native rollback cannot be verified must make every later save fail closed.
/// </summary>
internal sealed class ProjectSettingsPersistenceGate
{
    private const string UncertainFingerprintSuffix =
        "\0ACS_UNCERTAIN_PROJECT_SETTINGS";

    internal string? BlockedReason { get; private set; }
    internal bool IsBlocked => BlockedReason != null;
    private bool ForceDirtyCapture { get; set; }

    internal void Latch(
        string reason,
        bool nativeStateIsUncertain = false)
    {
        if (BlockedReason != null)
        {
            ForceDirtyCapture |= nativeStateIsUncertain;
            return;
        }
        BlockedReason = string.IsNullOrWhiteSpace(reason)
            ? "Project Settings persistence is blocked by an uncertain native state."
            : reason.Trim();
        ForceDirtyCapture = nativeStateIsUncertain;
    }

    internal void ClearAfterVerifiedLoad()
    {
        BlockedReason = null;
        ForceDirtyCapture = false;
    }

    internal EditorDocumentState ProtectCapture(EditorDocumentState state)
    {
        ArgumentNullException.ThrowIfNull(state);
        return ForceDirtyCapture
            ? new EditorDocumentState(
                state.Payload,
                state.ContentFingerprint + UncertainFingerprintSuffix)
            : state;
    }
}

/// <summary>
/// Synchronous UI mutation boundary: admission is checked before native state changes, and a
/// mutation whose hosted transaction cannot be recorded is rolled back before returning.
/// </summary>
internal readonly record struct ProjectSettingsMutationResult(
    bool Succeeded,
    bool NativeStateUncertain,
    string Detail);

internal static class ProjectSettingsMutationAdmission
{
    internal static ProjectSettingsMutationResult TryApply(
        Func<bool> canEdit,
        Func<bool> apply,
        Func<bool> record,
        Func<bool> rollback)
    {
        ArgumentNullException.ThrowIfNull(canEdit);
        ArgumentNullException.ThrowIfNull(apply);
        ArgumentNullException.ThrowIfNull(record);
        ArgumentNullException.ThrowIfNull(rollback);

        try
        {
            if (!canEdit())
            {
                return new ProjectSettingsMutationResult(
                    Succeeded: false,
                    NativeStateUncertain: false,
                    "Project Settings mutation was not admitted.");
            }
        }
        catch (Exception admissionError)
        {
            return new ProjectSettingsMutationResult(
                Succeeded: false,
                NativeStateUncertain: false,
                "Project Settings mutation admission failed: " +
                admissionError.Message);
        }

        bool applied;
        try
        {
            applied = apply();
        }
        catch (Exception applyError)
        {
            return RollBack(
                rollback,
                "Project Settings native mutation raised an exception: " +
                applyError.Message);
        }
        if (!applied)
        {
            return RollBack(
                rollback,
                "The native settings API rejected the mutation.");
        }

        try
        {
            if (record())
            {
                return new ProjectSettingsMutationResult(
                    Succeeded: true,
                    NativeStateUncertain: false,
                    "");
            }
        }
        catch (Exception recordError)
        {
            return RollBack(
                rollback,
                "Project Settings transaction recording failed: " +
                recordError.Message);
        }
        return RollBack(
            rollback,
            "Document Host rejected the Project Settings mutation.");
    }

    private static ProjectSettingsMutationResult RollBack(
        Func<bool> rollback,
        string reason)
    {
        try
        {
            if (rollback())
            {
                return new ProjectSettingsMutationResult(
                    Succeeded: false,
                    NativeStateUncertain: false,
                    reason + " The native mutation was rolled back.");
            }
            return new ProjectSettingsMutationResult(
                Succeeded: false,
                NativeStateUncertain: true,
                reason + " Native rollback was rejected.");
        }
        catch (Exception rollbackError)
        {
            return new ProjectSettingsMutationResult(
                Succeeded: false,
                NativeStateUncertain: true,
                reason + " Native rollback failed: " + rollbackError.Message);
        }
    }
}

internal readonly record struct ProjectSettingsLoadTicket(
    long Generation,
    CancellationToken CancellationToken);

internal sealed record ProjectSettingsDurabilityCheckpoint(string Utf8Sha256)
{
    private static readonly UTF8Encoding StrictUtf8NoBom = new(false, true);

    internal static ProjectSettingsDurabilityCheckpoint Create(
        string canonicalSettings)
    {
        ArgumentNullException.ThrowIfNull(canonicalSettings);
        byte[] utf8 = StrictUtf8NoBom.GetBytes(canonicalSettings);
        return new ProjectSettingsDurabilityCheckpoint(
            Convert.ToHexString(SHA256.HashData(utf8)).ToLowerInvariant());
    }
}

/// <summary>
/// Owns exactly one settings snapshot request. A newer load, startup failure, project switch, or
/// window close cancels the worker and makes every already-completed older result ineligible for
/// native application.
/// </summary>
internal sealed class ProjectSettingsLoadGenerationGate : IDisposable
{
    private readonly object _sync = new();
    private long _generation;
    private CancellationTokenSource? _current;

    internal ProjectSettingsLoadTicket Begin()
    {
        var next = new CancellationTokenSource();
        CancellationTokenSource? previous;
        long generation;
        lock (_sync)
        {
            previous = _current;
            _current = next;
            generation = ++_generation;
        }
        CancelAndDispose(previous);
        return new ProjectSettingsLoadTicket(generation, next.Token);
    }

    internal bool IsCurrent(ProjectSettingsLoadTicket ticket)
    {
        lock (_sync)
        {
            return _current != null &&
                   ticket.Generation == _generation &&
                   ticket.CancellationToken == _current.Token &&
                   !ticket.CancellationToken.IsCancellationRequested;
        }
    }

    internal void Invalidate()
    {
        CancellationTokenSource? previous;
        lock (_sync)
        {
            previous = _current;
            _current = null;
            _generation++;
        }
        CancelAndDispose(previous);
    }

    public void Dispose() => Invalidate();

    private static void CancelAndDispose(CancellationTokenSource? source)
    {
        if (source == null) return;
        try
        {
            try { source.Cancel(); }
            catch (ObjectDisposedException) { }
            catch (AggregateException) { }
        }
        finally { source.Dispose(); }
    }
}

internal static class ProjectSettingsDurableConvergence
{
    internal static EditorDocumentSaveResult Converge(
        EditorDocumentState committed,
        EditorDocumentState durable,
        Func<EditorDocumentState> captureLive,
        Action<EditorDocumentState> restoreDurable)
    {
        ArgumentNullException.ThrowIfNull(committed);
        ArgumentNullException.ThrowIfNull(durable);
        ArgumentNullException.ThrowIfNull(captureLive);
        ArgumentNullException.ThrowIfNull(restoreDurable);

        EditorDocumentState live = captureLive();
        if (!StatesMatch(live, committed))
        {
            return EditorDocumentSaveResult.Failed(
                "Project Settings changed while durable persistence was in flight; " +
                "the newer live state was preserved.");
        }

        if (!StatesMatch(committed, durable))
        {
            restoreDurable(durable);
            live = captureLive();
            if (!StatesMatch(live, durable))
            {
                return EditorDocumentSaveResult.Failed(
                    "The durable Project Settings checkpoint could not be reproduced in native state.");
            }
        }
        return EditorDocumentSaveResult.Saved(durable.ContentFingerprint);
    }

    private static bool StatesMatch(
        EditorDocumentState left,
        EditorDocumentState right) =>
        string.Equals(
            left.Payload,
            right.Payload,
            StringComparison.Ordinal) &&
        string.Equals(
            left.ContentFingerprint,
            right.ContentFingerprint,
            StringComparison.Ordinal);
}

internal static class ProjectSettingsProjectReferenceConvergence
{
    internal static bool TryReconcile(
        Project project,
        string expectedLiveReference,
        string authoritativeReference,
        EditorDocumentState durableSettings,
        out string detail)
    {
        ArgumentNullException.ThrowIfNull(project);
        ArgumentNullException.ThrowIfNull(expectedLiveReference);
        ArgumentNullException.ThrowIfNull(authoritativeReference);
        ArgumentNullException.ThrowIfNull(durableSettings);

        try
        {
            string currentReference = project.InitialScene;
            if (!string.Equals(
                    currentReference,
                    expectedLiveReference,
                    StringComparison.OrdinalIgnoreCase) &&
                !string.Equals(
                    currentReference,
                    authoritativeReference,
                    StringComparison.OrdinalIgnoreCase))
            {
                detail =
                    "Project InitialScene changed while Project Settings persistence was in flight; " +
                    "the newer Project state was preserved.";
                return false;
            }

            string normalizedAuthoritative =
                SceneSourceFile.NormalizeProjectSceneReference(
                    project.RootDir,
                    project.AssetsDir,
                    authoritativeReference);
            if (!ProjectSettingsDocumentContract.Parse(durableSettings.Payload)
                    .TryGetValue(
                        new ProjectSettingKey("Game", "DefaultScene"),
                        out string? configuredReference))
            {
                detail =
                    "The durable Project Settings checkpoint has no Game.DefaultScene.";
                return false;
            }
            string normalizedConfigured =
                SceneSourceFile.NormalizeProjectSceneReference(
                    project.RootDir,
                    project.AssetsDir,
                    configuredReference);
            if (!string.Equals(
                    normalizedConfigured,
                    normalizedAuthoritative,
                    StringComparison.OrdinalIgnoreCase))
            {
                detail =
                    "Durable Game.DefaultScene does not match the authoritative project manifest.";
                return false;
            }

            project.InitialScene = normalizedAuthoritative;
            detail = "";
            return true;
        }
        catch (Exception error)
        {
            detail =
                "The authoritative project scene reference could not be reconciled: " +
                error.Message;
            return false;
        }
    }
}

internal static class ProjectSettingsBuildDurabilityGate
{
    internal static async ValueTask<EditorDocumentSaveResult> SaveAsync(
        EditorDocumentHost host,
        EditorDocument settingsDocument,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(host);
        ArgumentNullException.ThrowIfNull(settingsDocument);
        try
        {
            if (cancellationToken.IsCancellationRequested)
            {
                return EditorDocumentSaveResult.Cancelled(
                    "Project Settings durability save was cancelled before preflight.");
            }
            if (settingsDocument.IsSuspended)
            {
                return EditorDocumentSaveResult.Failed(
                    "Project Settings is suspended by Play/Preview.");
            }
            if (settingsDocument.IsInTransaction)
            {
                return EditorDocumentSaveResult.Failed(
                    "Project Settings still has an open transaction.");
            }
            settingsDocument.Synchronize(
                "Build/Package settings durability preflight");
            cancellationToken.ThrowIfCancellationRequested();

            EditorDocumentSaveResult result =
                await host.SaveDocumentAsync(
                    settingsDocument.Id,
                    cancellationToken);
            if (result.Status == EditorDocumentSaveStatus.Saved &&
                settingsDocument.IsDirty)
            {
                return EditorDocumentSaveResult.Failed(
                    "Project Settings changed while the Build/Package durability save was in flight.");
            }
            return result;
        }
        catch (OperationCanceledException)
        {
            return EditorDocumentSaveResult.Cancelled(
                "Project Settings durability save was cancelled.");
        }
        catch (Exception error)
        {
            return EditorDocumentSaveResult.Failed(
                "Project Settings durability preflight failed: " +
                error.Message);
        }
    }
}

/// <summary>Creates the single project-owned Settings document hosted by the editor shell.</summary>
internal static class ProjectSettingsDocumentRegistration
{
    internal const int SaveOrder = 200;

    internal static EditorDocumentId CreateId(string settingsPath) =>
        EditorDocumentId.ForFile("settings", settingsPath);

    internal static EditorDocument Create(
        string settingsPath,
        Func<EditorDocumentState> capture,
        Action<EditorDocumentState> restore,
        EditorDocumentSaveContract save,
        bool initiallySaved,
        EditorDocumentState? initialState = null) =>
        new(
            CreateId(settingsPath),
            "Project Settings",
            settingsPath,
            capture,
            restore,
            save,
            initiallySaved,
            initialState: initialState,
            saveOrder: SaveOrder);
}
