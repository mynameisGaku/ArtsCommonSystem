// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Globalization;
using System.Linq;
using System.Text;

namespace AcsEditor;

internal enum EditorOperationService
{
    Build = 1,
    Package = 2,
}

internal enum EditorOperationSeverity
{
    Info = 1,
    Warning = 2,
    Error = 3,
}

internal enum EditorOperationState
{
    Succeeded = 1,
    Failed = 2,
    Cancelled = 3,
}

internal static class EditorOperationCancellationClassifier
{
    internal static bool IsCancellationRequested(
        bool operationCancellationRequested,
        bool ownerCloseRequested,
        bool lifetimeCancellationRequested) =>
        operationCancellationRequested ||
        ownerCloseRequested ||
        lifetimeCancellationRequested;
}

internal readonly record struct EditorOperationId
{
    internal EditorOperationId(Guid value)
    {
        if (value == Guid.Empty)
            throw new ArgumentException(
                "An editor operation ID cannot be empty.",
                nameof(value));
        Value = value;
    }

    internal Guid Value { get; }

    internal static EditorOperationId New() =>
        new(Guid.NewGuid());

    internal static EditorOperationId Parse(string value)
    {
        if (!Guid.TryParseExact(value, "N", out Guid parsed) ||
            parsed == Guid.Empty)
        {
            throw new FormatException(
                "Editor operation IDs use a nonzero 32-digit GUID.");
        }
        return new EditorOperationId(parsed);
    }

    public override string ToString() =>
        Value.ToString("N");
}

internal sealed record EditorOperationDiagnostic
{
    internal required uint ContractVersion { get; init; }
    internal required EditorOperationId OperationId { get; init; }
    internal required ulong Sequence { get; init; }
    internal required EditorOperationService Service { get; init; }
    internal required EditorOperationSeverity Severity { get; init; }
    internal required string Code { get; init; }
    internal required string Message { get; init; }
    internal string? AssetId { get; init; }
    internal string? Path { get; init; }
    internal bool IsTerminal { get; init; }
}

internal sealed record EditorOperationResult
{
    internal required uint ContractVersion { get; init; }
    internal required EditorOperationId OperationId { get; init; }
    internal required EditorOperationService Service { get; init; }
    internal required EditorOperationState State { get; init; }
    internal required IReadOnlyList<EditorOperationDiagnostic> Diagnostics
    {
        get;
        init;
    }
}

internal static class EditorOperationCodes
{
    internal const string BuildStarted = "ACS.BUILD.STARTED";
    internal const string BuildSucceeded = "ACS.BUILD.SUCCEEDED";
    internal const string BuildFailed = "ACS.BUILD.FAILED";
    internal const string BuildCancelled = "ACS.BUILD.CANCELLED";
    internal const string BuildSceneSaveFailed =
        "ACS.BUILD.SCENE_SAVE_FAILED";
    internal const string BuildLaunchFailed =
        "ACS.BUILD.LAUNCH_FAILED";

    internal const string PackageStarted = "ACS.PACKAGE.STARTED";
    internal const string PackageSucceeded = "ACS.PACKAGE.SUCCEEDED";
    internal const string PackageFailed = "ACS.PACKAGE.FAILED";
    internal const string PackageCancelled = "ACS.PACKAGE.CANCELLED";
    internal const string PackageValidationFailed =
        "ACS.PACKAGE.VALIDATION_FAILED";

    internal const string OperationIncomplete =
        "ACS.OPERATION.INCOMPLETE";
    internal const string DiagnosticsTruncated =
        "ACS.OPERATION.DIAGNOSTICS_TRUNCATED";
    internal const string InvalidDiagnosticCode =
        "ACS.OPERATION.INVALID_DIAGNOSTIC_CODE";

    internal static string PackageIssue(string issueCode)
    {
        if (string.IsNullOrWhiteSpace(issueCode))
            return "ACS.PACKAGE.ISSUE";

        Span<char> normalized =
            stackalloc char[Math.Min(issueCode.Length, 64)];
        int length = 0;
        foreach (char value in issueCode)
        {
            if (length == normalized.Length)
                break;
            char upper = char.ToUpperInvariant(value);
            normalized[length++] =
                upper is >= 'A' and <= 'Z' or >= '0' and <= '9' or '_'
                    ? upper
                    : '_';
        }
        return "ACS.PACKAGE." + new string(normalized[..length]);
    }
}

internal static class EditorOperationDiagnosticContract
{
    internal const uint Version = 1;
    internal const int MaximumRetainedDiagnostics = 256;
    internal const int MaximumCodeLength = 96;
    internal const int MaximumMessageLength = 8192;
    internal const int MaximumAssetIdLength = 256;
    internal const int MaximumPathLength = 32768;
    internal const string TextTruncationMarker = " [truncated]";
    internal const string MissingMessage = "(no diagnostic message)";

    internal static EditorOperationDiagnostic Create(
        EditorOperationId operationId,
        ulong sequence,
        EditorOperationService service,
        EditorOperationSeverity severity,
        string code,
        string message,
        string? assetId = null,
        string? path = null,
        bool isTerminal = false)
    {
        if (operationId.Value == Guid.Empty)
            throw new ArgumentException(
                "The diagnostic requires a nonempty operation ID.",
                nameof(operationId));
        if (sequence == 0u)
            throw new ArgumentOutOfRangeException(
                nameof(sequence),
                "Diagnostic sequences start at one.");
        if (!Enum.IsDefined(service))
            throw new ArgumentOutOfRangeException(nameof(service));
        if (!Enum.IsDefined(severity))
            throw new ArgumentOutOfRangeException(nameof(severity));

        string stableCode = NormalizeCode(code);
        string stableMessage = NormalizeRequired(
            message,
            MaximumMessageLength);

        return new EditorOperationDiagnostic
        {
            ContractVersion = Version,
            OperationId = operationId,
            Sequence = sequence,
            Service = service,
            Severity = severity,
            Code = stableCode,
            Message = stableMessage,
            AssetId = NormalizeOptional(assetId, MaximumAssetIdLength),
            Path = NormalizeOptional(path, MaximumPathLength),
            IsTerminal = isTerminal,
        };
    }

    internal static string NormalizeCode(string? code)
    {
        string source = code ?? "";
        ReadOnlySpan<char> stableCode = source.AsSpan().Trim();
        if (!IsStableCode(stableCode))
            return EditorOperationCodes.InvalidDiagnosticCode;
        return stableCode.Length == source.Length
            ? source
            : stableCode.ToString();
    }

    internal static bool IsStableCode(string code) =>
        IsStableCode(code.AsSpan());

    private static bool IsStableCode(ReadOnlySpan<char> code)
    {
        if (code.Length < 5 ||
            code.Length > MaximumCodeLength ||
            !code.StartsWith("ACS.".AsSpan(), StringComparison.Ordinal) ||
            code[^1] == '.')
        {
            return false;
        }
        bool previousWasDot = false;
        foreach (char value in code)
        {
            bool allowed =
                value is >= 'A' and <= 'Z' or >= '0' and <= '9' or
                    '.' or '_' or '-';
            if (!allowed || value == '.' && previousWasDot)
                return false;
            previousWasDot = value == '.';
        }
        return true;
    }

    private static string NormalizeRequired(
        string? value,
        int maximumLength)
    {
        string source = value ?? "";
        ReadOnlySpan<char> normalized = source.AsSpan().Trim();
        if (normalized.Length == 0)
            return MissingMessage;
        return CopyBounded(source, normalized, maximumLength);
    }

    private static string? NormalizeOptional(
        string? value,
        int maximumLength)
    {
        if (value == null)
            return null;
        ReadOnlySpan<char> normalized = value.AsSpan().Trim();
        if (normalized.Length == 0)
            return null;
        return CopyBounded(value, normalized, maximumLength);
    }

    private static string CopyBounded(
        string source,
        ReadOnlySpan<char> value,
        int maximumLength)
    {
        if (value.Length <= maximumLength)
        {
            return value.Length == source.Length
                ? source
                : value.ToString();
        }

        int retainedLength =
            maximumLength - TextTruncationMarker.Length;
        if (retainedLength > 0 &&
            retainedLength < value.Length &&
            char.IsHighSurrogate(value[retainedLength - 1]) &&
            char.IsLowSurrogate(value[retainedLength]))
        {
            retainedLength--;
        }
        return string.Concat(
            value[..retainedLength],
            TextTruncationMarker);
    }
}

internal sealed class EditorOperationSession : IDisposable
{
    private readonly object _gate = new();
    private readonly List<EditorOperationDiagnostic> _diagnostics = [];
    private readonly Queue<EditorOperationDiagnostic> _pendingNotifications = [];
    private readonly Action<EditorOperationDiagnostic>? _observer;
    private readonly Action<EditorOperationResult> _completed;
    private EditorOperationResult? _result;
    private ulong _nextSequence = 1u;
    private bool _notificationDrainActive;

    internal EditorOperationSession(
        EditorOperationId operationId,
        EditorOperationService service,
        Action<EditorOperationDiagnostic>? observer,
        Action<EditorOperationResult> completed)
    {
        OperationId = operationId;
        Service = service;
        _observer = observer;
        _completed = completed;
    }

    internal EditorOperationId OperationId { get; }
    internal EditorOperationService Service { get; }

    internal bool IsCompleted
    {
        get
        {
            lock (_gate)
                return _result != null;
        }
    }

    internal EditorOperationResult? Result
    {
        get
        {
            lock (_gate)
                return _result;
        }
    }

    internal bool Report(
        EditorOperationSeverity severity,
        string code,
        string message,
        string? assetId = null,
        string? path = null)
    {
        EditorOperationDiagnostic diagnostic;
        bool retained;
        bool startNotificationDrain;
        lock (_gate)
        {
            if (_result != null)
                return false;
            if (_diagnostics.Count >=
                EditorOperationDiagnosticContract
                    .MaximumRetainedDiagnostics - 1)
            {
                return false;
            }
            if (_diagnostics.Count ==
                EditorOperationDiagnosticContract
                    .MaximumRetainedDiagnostics - 2)
            {
                diagnostic = CreateDiagnostic(
                    EditorOperationSeverity.Warning,
                    EditorOperationCodes.DiagnosticsTruncated,
                    "Additional diagnostics were omitted after the " +
                    "per-operation retention limit was reached.",
                    assetId: null,
                    path: null,
                    isTerminal: false);
                retained = false;
            }
            else
            {
                diagnostic = CreateDiagnostic(
                    severity,
                    code,
                    message,
                    assetId,
                    path,
                    isTerminal: false);
                retained = true;
            }
            _diagnostics.Add(diagnostic);
            startNotificationDrain = EnqueueNotificationLocked(diagnostic);
        }
        if (startNotificationDrain)
            DrainNotifications();
        return retained;
    }

    internal EditorOperationResult Succeed(
        string code,
        string message,
        string? assetId = null,
        string? path = null) =>
        Complete(
            EditorOperationState.Succeeded,
            EditorOperationSeverity.Info,
            code,
            message,
            assetId,
            path);

    internal EditorOperationResult Fail(
        string code,
        string message,
        string? assetId = null,
        string? path = null) =>
        Complete(
            EditorOperationState.Failed,
            EditorOperationSeverity.Error,
            code,
            message,
            assetId,
            path);

    internal EditorOperationResult Cancel(
        string code,
        string message,
        string? assetId = null,
        string? path = null) =>
        Complete(
            EditorOperationState.Cancelled,
            EditorOperationSeverity.Warning,
            code,
            message,
            assetId,
            path);

    public void Dispose()
    {
        if (!IsCompleted)
        {
            Fail(
                EditorOperationCodes.OperationIncomplete,
                $"{Service} operation left its scope without a terminal result.");
        }
    }

    private EditorOperationResult Complete(
        EditorOperationState state,
        EditorOperationSeverity severity,
        string code,
        string message,
        string? assetId,
        string? path)
    {
        EditorOperationResult result;
        bool startNotificationDrain;
        lock (_gate)
        {
            if (_result != null)
                return _result;

            EditorOperationDiagnostic terminal = CreateDiagnostic(
                severity,
                code,
                message,
                assetId,
                path,
                isTerminal: true);
            _diagnostics.Add(terminal);
            result = new EditorOperationResult
            {
                ContractVersion = EditorOperationDiagnosticContract.Version,
                OperationId = OperationId,
                Service = Service,
                State = state,
                Diagnostics = new ReadOnlyCollection<EditorOperationDiagnostic>(
                    _diagnostics.ToArray()),
            };
            _result = result;
            startNotificationDrain =
                EnqueueNotificationLocked(terminal);
        }

        if (startNotificationDrain)
            DrainNotifications();
        try
        {
            _completed(result);
        }
        catch
        {
            // Diagnostics must never change the owning operation's outcome.
        }
        return result;
    }

    private bool EnqueueNotificationLocked(
        EditorOperationDiagnostic diagnostic)
    {
        _pendingNotifications.Enqueue(diagnostic);
        if (_notificationDrainActive)
            return false;
        _notificationDrainActive = true;
        return true;
    }

    private void DrainNotifications()
    {
        while (true)
        {
            EditorOperationDiagnostic diagnostic;
            lock (_gate)
            {
                if (_pendingNotifications.Count == 0)
                {
                    _notificationDrainActive = false;
                    return;
                }
                diagnostic = _pendingNotifications.Dequeue();
            }
            Notify(diagnostic);
        }
    }

    private EditorOperationDiagnostic CreateDiagnostic(
        EditorOperationSeverity severity,
        string code,
        string message,
        string? assetId,
        string? path,
        bool isTerminal)
    {
        ulong sequence = _nextSequence;
        _nextSequence = checked(_nextSequence + 1u);
        return EditorOperationDiagnosticContract.Create(
            OperationId,
            sequence,
            Service,
            severity,
            code,
            message,
            assetId,
            path,
            isTerminal);
    }

    private void Notify(EditorOperationDiagnostic diagnostic)
    {
        try
        {
            _observer?.Invoke(diagnostic);
        }
        catch
        {
            // Legacy/UI logging is an observer, not operation control flow.
        }
    }
}

internal sealed class EditorOperationJournal
{
    private sealed record ActiveOperation(long Ordinal);
    private sealed record CompletedOperation(
        long Ordinal,
        EditorOperationResult Result);

    private readonly object _gate = new();
    private readonly int _completedCapacity;
    private readonly Action<EditorOperationDiagnostic>? _observer;
    private readonly Dictionary<EditorOperationId, ActiveOperation> _active = [];
    private readonly List<CompletedOperation> _completed = [];
    private long _nextOrdinal;

    internal EditorOperationJournal(
        int completedCapacity = 128,
        Action<EditorOperationDiagnostic>? observer = null)
    {
        if (completedCapacity <= 0)
            throw new ArgumentOutOfRangeException(nameof(completedCapacity));
        _completedCapacity = completedCapacity;
        _observer = observer;
    }

    internal EditorOperationSession Begin(
        EditorOperationService service,
        string startCode,
        string startMessage,
        string? assetId = null,
        string? path = null,
        EditorOperationId? operationId = null)
    {
        EditorOperationId id = operationId ?? EditorOperationId.New();
        lock (_gate)
        {
            if (_active.ContainsKey(id) ||
                _completed.Any(item => item.Result.OperationId == id))
            {
                throw new InvalidOperationException(
                    $"Editor operation {id} is already known.");
            }
            long ordinal = checked(++_nextOrdinal);
            _active.Add(id, new ActiveOperation(ordinal));
        }

        var session = new EditorOperationSession(
            id,
            service,
            _observer,
            OnCompleted);
        try
        {
            session.Report(
                EditorOperationSeverity.Info,
                startCode,
                startMessage,
                assetId,
                path);
            return session;
        }
        catch
        {
            lock (_gate)
                _active.Remove(id);
            throw;
        }
    }

    internal IReadOnlyList<EditorOperationResult> CompletedSnapshot()
    {
        lock (_gate)
        {
            return new ReadOnlyCollection<EditorOperationResult>(
                _completed
                    .OrderBy(item => item.Ordinal)
                    .Select(item => item.Result)
                    .ToArray());
        }
    }

    internal int ActiveCount
    {
        get
        {
            lock (_gate)
                return _active.Count;
        }
    }

    private void OnCompleted(EditorOperationResult result)
    {
        lock (_gate)
        {
            if (!_active.Remove(
                    result.OperationId,
                    out ActiveOperation? active))
            {
                return;
            }
            _completed.Add(
                new CompletedOperation(active.Ordinal, result));
            while (_completed.Count > _completedCapacity)
            {
                int oldestIndex = 0;
                for (int index = 1; index < _completed.Count; index++)
                {
                    if (_completed[index].Ordinal <
                        _completed[oldestIndex].Ordinal)
                    {
                        oldestIndex = index;
                    }
                }
                _completed.RemoveAt(oldestIndex);
            }
        }
    }
}

internal static class EditorOperationDiagnosticFormatting
{
    internal const int MaximumLegacyLineLength = 16384;
    internal const string LegacyTruncationMarker = " [truncated]";

    internal static string LegacyLine(
        EditorOperationDiagnostic diagnostic)
    {
        var output = new BoundedLegacyLineBuilder();
        output.AppendLiteral(
            $"[operation:{diagnostic.OperationId}] " +
            $"[{diagnostic.Service}] [{diagnostic.Severity}] " +
            $"[{diagnostic.Code}] ");
        output.AppendEscaped(diagnostic.Message ?? "");
        output.AppendOptionalField(" asset=", diagnostic.AssetId);
        output.AppendOptionalField(" path=", diagnostic.Path);
        return output.Finish();
    }

    private sealed class BoundedLegacyLineBuilder
    {
        private static readonly int MarkerContentLimit =
            MaximumLegacyLineLength - LegacyTruncationMarker.Length;
        private readonly StringBuilder _output =
            new(capacity: 256, maxCapacity: MaximumLegacyLineLength);
        private int _markerSafeLength;
        private bool _truncated;

        internal void AppendLiteral(string value)
        {
            if (_truncated)
                return;
            for (int index = 0; index < value.Length; index++)
            {
                int unitLength =
                    char.IsHighSurrogate(value[index]) &&
                    index + 1 < value.Length &&
                    char.IsLowSurrogate(value[index + 1])
                        ? 2
                        : 1;
                bool appended = unitLength == 2
                    ? AppendPair(value[index], value[index + 1])
                    : AppendChar(value[index]);
                if (!appended)
                {
                    return;
                }
                if (unitLength == 2)
                    index++;
            }
        }

        internal void AppendEscaped(string value)
        {
            if (_truncated)
                return;
            for (int index = 0; index < value.Length; index++)
            {
                char current = value[index];
                if (char.IsHighSurrogate(current) &&
                    index + 1 < value.Length &&
                    char.IsLowSurrogate(value[index + 1]))
                {
                    char low = value[index + 1];
                    if (Rune.GetUnicodeCategory(new Rune(current, low)) ==
                        UnicodeCategory.Format)
                    {
                        if (!AppendUnit(
                                $"\\u{(int)current:X4}" +
                                $"\\u{(int)low:X4}"))
                        {
                            return;
                        }
                    }
                    else if (!AppendPair(current, low))
                    {
                        return;
                    }
                    index++;
                    continue;
                }

                string unit = current switch
                {
                    '\r' => "\\r",
                    '\n' => "\\n",
                    '\t' => "\\t",
                    '\u2028' => "\\u2028",
                    '\u2029' => "\\u2029",
                    _ when char.IsControl(current) =>
                        $"\\u{(int)current:X4}",
                    _ when char.GetUnicodeCategory(current) ==
                        UnicodeCategory.Format =>
                        $"\\u{(int)current:X4}",
                    _ when char.GetUnicodeCategory(current) ==
                        UnicodeCategory.Surrogate =>
                        $"\\u{(int)current:X4}",
                    _ => "",
                };
                bool appended = unit.Length == 0
                    ? AppendChar(current)
                    : AppendUnit(unit);
                if (!appended)
                    return;
            }
        }

        internal void AppendOptionalField(
            string label,
            string? value)
        {
            if (_truncated || value == null)
                return;

            int fieldStart = _output.Length;
            int markerSafeAtFieldStart = _markerSafeLength;
            AppendLiteral(label);
            AppendEscaped(value);
            if (_truncated)
            {
                // Optional metadata is atomic. Prefer the operation prefix and
                // message over a partial asset/path field.
                _output.Length =
                    fieldStart <= MarkerContentLimit
                        ? fieldStart
                        : markerSafeAtFieldStart;
                _markerSafeLength = _output.Length;
            }
        }

        internal string Finish()
        {
            if (_truncated)
                _output.Append(LegacyTruncationMarker);
            return _output.ToString();
        }

        private bool AppendUnit(string unit)
        {
            if (!CanAppend(unit.Length))
            {
                MarkTruncated();
                return false;
            }
            _output.Append(unit);
            RecordMarkerSafeLength();
            return true;
        }

        private bool AppendChar(char value)
        {
            if (!CanAppend(1))
            {
                MarkTruncated();
                return false;
            }
            _output.Append(value);
            RecordMarkerSafeLength();
            return true;
        }

        private bool AppendPair(char high, char low)
        {
            if (!CanAppend(2))
            {
                MarkTruncated();
                return false;
            }
            _output.Append(high);
            _output.Append(low);
            RecordMarkerSafeLength();
            return true;
        }

        private bool CanAppend(int length) =>
            _output.Length <= MaximumLegacyLineLength - length;

        private void RecordMarkerSafeLength()
        {
            if (_output.Length <= MarkerContentLimit)
                _markerSafeLength = _output.Length;
        }

        private void MarkTruncated()
        {
            _truncated = true;
            _output.Length = _markerSafeLength;
        }
    }
}
