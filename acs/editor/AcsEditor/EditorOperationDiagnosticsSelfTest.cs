// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace AcsEditor;

internal static class EditorOperationDiagnosticsSelfTest
{
    internal static int Run(TextWriter output)
    {
        int failures = 0;
        void Check(bool condition, string description)
        {
            output.WriteLine(
                $"{(condition ? "PASS" : "FAIL")}: {description}");
            if (!condition)
                failures++;
        }

        static bool Throws<T>(Action action)
            where T : Exception
        {
            try
            {
                action();
                return false;
            }
            catch (T)
            {
                return true;
            }
        }

        static bool ContainsUnicodeFormat(string value)
        {
            foreach (Rune rune in value.EnumerateRunes())
            {
                if (Rune.GetUnicodeCategory(rune) ==
                    System.Globalization.UnicodeCategory.Format)
                {
                    return true;
                }
            }
            return false;
        }

        static bool IsWellFormedUtf16(string value)
        {
            for (int index = 0; index < value.Length; index++)
            {
                if (char.IsHighSurrogate(value[index]))
                {
                    if (index + 1 >= value.Length ||
                        !char.IsLowSurrogate(value[index + 1]))
                    {
                        return false;
                    }
                    index++;
                }
                else if (char.IsLowSurrogate(value[index]))
                {
                    return false;
                }
            }
            return true;
        }

        static bool HasOnlyCompleteUnicodeEscapes(string value)
        {
            for (int index = 0; index + 1 < value.Length; index++)
            {
                if (value[index] != '\\' || value[index + 1] != 'u')
                    continue;
                if (index + 5 >= value.Length)
                    return false;
                for (int digit = index + 2; digit <= index + 5; digit++)
                {
                    if (!Uri.IsHexDigit(value[digit]))
                        return false;
                }
                index += 5;
            }
            return true;
        }

        const string firstIdText =
            "00112233445566778899aabbccddeeff";
        EditorOperationId firstId =
            EditorOperationId.Parse(firstIdText);
        Check(
            EditorOperationDiagnosticContract.Version == 1 &&
            firstId.ToString() == firstIdText &&
            Throws<FormatException>(
                () => EditorOperationId.Parse(Guid.Empty.ToString("N"))),
            "operation diagnostics use a versioned contract and nonempty canonical operation ID");

        var observed = new List<EditorOperationDiagnostic>();
        var journal = new EditorOperationJournal(
            completedCapacity: 4,
            observer: observed.Add);
        using EditorOperationSession build = journal.Begin(
            EditorOperationService.Build,
            EditorOperationCodes.BuildStarted,
            "Build started.",
            assetId: " asset-guid ",
            path: " C:\\Project\\game.acsproject ",
            operationId: firstId);
        Check(
            observed.Count == 1 &&
            observed[0].ContractVersion == 1 &&
            observed[0].OperationId == firstId &&
            observed[0].Sequence == 1 &&
            observed[0].Service == EditorOperationService.Build &&
            observed[0].Severity == EditorOperationSeverity.Info &&
            observed[0].Code == EditorOperationCodes.BuildStarted &&
            observed[0].AssetId == "asset-guid" &&
            observed[0].Path == "C:\\Project\\game.acsproject" &&
            EditorOperationDiagnosticFormatting.LegacyLine(observed[0]) ==
                $"[operation:{firstIdText}] [Build] [Info] " +
                "[ACS.BUILD.STARTED] Build started. " +
                "asset=asset-guid path=C:\\Project\\game.acsproject",
            "typed diagnostics retain service, severity, stable code, message, asset, path, and operation ID");

        EditorOperationDiagnostic exactLegacyBase =
            EditorOperationDiagnosticContract.Create(
                firstId,
                98,
                EditorOperationService.Build,
                EditorOperationSeverity.Info,
                "ACS.BUILD.EXACT_LEGACY_BOUNDARY",
                "Exact boundary.");
        string exactLegacyPrefix =
            EditorOperationDiagnosticFormatting.LegacyLine(
                exactLegacyBase);
        int exactPathLength =
            EditorOperationDiagnosticFormatting.MaximumLegacyLineLength -
            exactLegacyPrefix.Length -
            " path=".Length;
        string exactPath = new('p', exactPathLength);
        EditorOperationDiagnostic exactLegacyFixture =
            EditorOperationDiagnosticContract.Create(
                firstId,
                98,
                EditorOperationService.Build,
                EditorOperationSeverity.Info,
                "ACS.BUILD.EXACT_LEGACY_BOUNDARY",
                "Exact boundary.",
                path: exactPath);
        string exactLegacyLine =
            EditorOperationDiagnosticFormatting.LegacyLine(
                exactLegacyFixture);
        Check(
            exactLegacyLine.Length ==
                EditorOperationDiagnosticFormatting
                    .MaximumLegacyLineLength &&
            exactLegacyLine ==
                exactLegacyPrefix + " path=" + exactPath &&
            !exactLegacyLine.EndsWith(
                EditorOperationDiagnosticFormatting
                    .LegacyTruncationMarker,
                StringComparison.Ordinal),
            "a safe legacy line that exactly reaches the cap is preserved byte-for-byte without false truncation");

        EditorOperationDiagnostic unsafeLegacy =
            EditorOperationDiagnosticContract.Create(
                firstId,
                99,
                EditorOperationService.Build,
                EditorOperationSeverity.Warning,
                "ACS.BUILD.LEGACY_SANITIZE",
                "unsafe\r\nmessage\twith\u001Bcontrol\u2028and\u202Espoof\uD800",
                assetId: "asset\nid\u2066\uDC00",
                path: "C:\\unsafe\rpath\u2029tail\U0001BCA0\U0001F600");
        string safeLegacy =
            EditorOperationDiagnosticFormatting.LegacyLine(unsafeLegacy);
        Check(
            unsafeLegacy.Message.Contains('\n') &&
            unsafeLegacy.AssetId!.Contains('\n') &&
            safeLegacy.Contains(
                "unsafe\\r\\nmessage\\twith\\u001Bcontrol\\u2028and\\u202Espoof\\uD800",
                StringComparison.Ordinal) &&
            safeLegacy.Contains(
                "asset=asset\\nid\\u2066\\uDC00",
                StringComparison.Ordinal) &&
            safeLegacy.Contains(
                "path=C:\\unsafe\\rpath\\u2029tail\\uD82F\\uDCA0\U0001F600",
                StringComparison.Ordinal) &&
            !safeLegacy.Contains('\uD800') &&
            !safeLegacy.Contains('\uDC00') &&
            ContainsUnicodeFormat(unsafeLegacy.Message) &&
            !ContainsUnicodeFormat(safeLegacy) &&
            !safeLegacy.Any(
                value =>
                    char.IsControl(value) ||
                    value is '\u2028' or '\u2029'),
            "legacy formatting escapes line breaks, controls, Unicode Format characters, and unpaired surrogates without mutating structured fields");

        int messagePayloadLength =
            EditorOperationDiagnosticContract.MaximumMessageLength -
            EditorOperationDiagnosticContract.TextTruncationMarker.Length;
        string surrogateBoundaryMessage =
            new string('m', messagePayloadLength - 1) +
            "\U0001F600" +
            new string('z', 64);
        EditorOperationDiagnostic boundedText =
            EditorOperationDiagnosticContract.Create(
                firstId,
                100,
                EditorOperationService.Package,
                EditorOperationSeverity.Error,
                new string('x', 512),
                surrogateBoundaryMessage,
                assetId: new string(
                    'a',
                    EditorOperationDiagnosticContract
                        .MaximumAssetIdLength + 64),
                path: new string(
                    '\u001B',
                    EditorOperationDiagnosticContract
                        .MaximumPathLength + 64));
        Check(
            boundedText.Code ==
                EditorOperationCodes.InvalidDiagnosticCode &&
            boundedText.Message.Length <=
                EditorOperationDiagnosticContract.MaximumMessageLength &&
            boundedText.Message.EndsWith(
                EditorOperationDiagnosticContract.TextTruncationMarker,
                StringComparison.Ordinal) &&
            IsWellFormedUtf16(boundedText.Message) &&
            boundedText.AssetId?.Length ==
                EditorOperationDiagnosticContract.MaximumAssetIdLength &&
            boundedText.AssetId.EndsWith(
                EditorOperationDiagnosticContract.TextTruncationMarker,
                StringComparison.Ordinal) &&
            boundedText.Path?.Length ==
                EditorOperationDiagnosticContract.MaximumPathLength &&
            boundedText.Path.EndsWith(
                EditorOperationDiagnosticContract.TextTruncationMarker,
                StringComparison.Ordinal),
            "hostile text fields truncate deterministically at contract bounds without splitting a valid surrogate pair");

        string edgeWhitespace = new(' ', 65536);
        EditorOperationDiagnostic edgeWhitespaceText =
            EditorOperationDiagnosticContract.Create(
                firstId,
                103,
                EditorOperationService.Build,
                EditorOperationSeverity.Error,
                edgeWhitespace +
                    "ACS.BUILD.EDGE_WHITESPACE" +
                    edgeWhitespace,
                edgeWhitespace +
                    new string(
                        'q',
                        EditorOperationDiagnosticContract
                            .MaximumMessageLength * 2) +
                    edgeWhitespace,
                path:
                    edgeWhitespace +
                    new string(
                        'p',
                        EditorOperationDiagnosticContract
                            .MaximumPathLength * 2) +
                    edgeWhitespace);
        Check(
            edgeWhitespaceText.Code ==
                "ACS.BUILD.EDGE_WHITESPACE" &&
            edgeWhitespaceText.Message.Length ==
                EditorOperationDiagnosticContract.MaximumMessageLength &&
            edgeWhitespaceText.Path?.Length ==
                EditorOperationDiagnosticContract.MaximumPathLength &&
            edgeWhitespaceText.Message.EndsWith(
                EditorOperationDiagnosticContract.TextTruncationMarker,
                StringComparison.Ordinal) &&
            edgeWhitespaceText.Path.EndsWith(
                EditorOperationDiagnosticContract.TextTruncationMarker,
                StringComparison.Ordinal),
            "edge whitespace is trimmed through spans before bounded copies are materialized");

        EditorOperationDiagnostic blankText =
            EditorOperationDiagnosticContract.Create(
                firstId,
                101,
                EditorOperationService.Build,
                EditorOperationSeverity.Warning,
                "not a stable code",
                " \r\n\t ",
                assetId: "\t ",
                path: "\r\n");
        Check(
            blankText.Code ==
                EditorOperationCodes.InvalidDiagnosticCode &&
            blankText.Message ==
                EditorOperationDiagnosticContract.MissingMessage &&
            blankText.AssetId == null &&
            blankText.Path == null,
            "blank and malformed external text maps to deterministic bounded contract fallbacks");

        EditorOperationDiagnostic expansionFixture =
            EditorOperationDiagnosticContract.Create(
                firstId,
                102,
                EditorOperationService.Package,
                EditorOperationSeverity.Error,
                "ACS.PACKAGE.HOSTILE_LEGACY",
                new string(
                    '\u001B',
                    EditorOperationDiagnosticContract.MaximumMessageLength),
                assetId: "asset\u202Ename\uD800",
                path: new string(
                    '\u2066',
                    EditorOperationDiagnosticContract.MaximumPathLength));
        string boundedLegacy =
            EditorOperationDiagnosticFormatting.LegacyLine(
                expansionFixture);
        Check(
            boundedLegacy.Length <=
                EditorOperationDiagnosticFormatting
                    .MaximumLegacyLineLength &&
            boundedLegacy.EndsWith(
                EditorOperationDiagnosticFormatting
                    .LegacyTruncationMarker,
                StringComparison.Ordinal) &&
            !boundedLegacy.Contains(" asset=", StringComparison.Ordinal) &&
            !boundedLegacy.Contains(" path=", StringComparison.Ordinal) &&
            !boundedLegacy.Any(
                value =>
                    char.IsControl(value) ||
                    value is '\u2028' or '\u2029') &&
            !ContainsUnicodeFormat(boundedLegacy) &&
            IsWellFormedUtf16(boundedLegacy) &&
            HasOnlyCompleteUnicodeEscapes(boundedLegacy),
            "legacy formatting streams hostile expansion into one capped line with atomic escapes and deterministic optional-field omission");

        Check(
            build.Report(
                EditorOperationSeverity.Warning,
                "ACS.BUILD.CACHE_STALE",
                "Build cache will be regenerated.") &&
            build.Report(
                EditorOperationSeverity.Info,
                "ACS.BUILD.CONFIGURED",
                "Configure completed."),
            "a live operation accepts ordered nonterminal diagnostics");
        EditorOperationResult succeeded = build.Succeed(
            EditorOperationCodes.BuildSucceeded,
            "Build completed.",
            path: "C:\\Project\\Binaries\\game.exe");
        EditorOperationResult repeated = build.Fail(
            EditorOperationCodes.BuildFailed,
            "Late failure must not replace success.");
        Check(
            ReferenceEquals(succeeded, repeated) &&
            succeeded.State == EditorOperationState.Succeeded &&
            succeeded.Diagnostics.Select(item => item.Sequence)
                .SequenceEqual([1ul, 2ul, 3ul, 4ul]) &&
            succeeded.Diagnostics.Count(item => item.IsTerminal) == 1 &&
            succeeded.Diagnostics[^1].Code ==
                EditorOperationCodes.BuildSucceeded &&
            !build.Report(
                EditorOperationSeverity.Error,
                EditorOperationCodes.BuildFailed,
                "Late diagnostic."),
            "terminal completion is immutable, idempotent, and freezes a deterministic sequence");

        Check(
            journal.ActiveCount == 0 &&
            journal.CompletedSnapshot().Count == 1 &&
            journal.CompletedSnapshot()[0].OperationId == firstId,
            "journal completion atomically moves an operation from active to completed aggregation");

        Check(
            EditorOperationDiagnosticContract.Create(
                    firstId,
                    1,
                    EditorOperationService.Build,
                    EditorOperationSeverity.Error,
                    "acs.build.lowercase",
                    "Invalid code.").Code ==
                EditorOperationCodes.InvalidDiagnosticCode &&
            Throws<ArgumentException>(
                () => EditorOperationDiagnosticContract.Create(
                    default,
                    1,
                    EditorOperationService.Build,
                    EditorOperationSeverity.Error,
                    EditorOperationCodes.BuildFailed,
                    "Missing operation ID.")) &&
            EditorOperationCodes.PackageIssue("bad path/code") ==
                "ACS.PACKAGE.BAD_PATH_CODE",
            "programmer invariants stay strict while unstable text codes fail closed to a deterministic stable code");

        var bounded = new EditorOperationJournal(completedCapacity: 2);
        EditorOperationSession first = bounded.Begin(
            EditorOperationService.Build,
            EditorOperationCodes.BuildStarted,
            "First.",
            operationId: EditorOperationId.Parse(
                "10000000000000000000000000000001"));
        EditorOperationSession second = bounded.Begin(
            EditorOperationService.Build,
            EditorOperationCodes.BuildStarted,
            "Second.",
            operationId: EditorOperationId.Parse(
                "20000000000000000000000000000002"));
        EditorOperationSession third = bounded.Begin(
            EditorOperationService.Package,
            EditorOperationCodes.PackageStarted,
            "Third.",
            operationId: EditorOperationId.Parse(
                "30000000000000000000000000000003"));
        third.Succeed(
            EditorOperationCodes.PackageSucceeded,
            "Third complete.");
        first.Succeed(
            EditorOperationCodes.BuildSucceeded,
            "First complete.");
        second.Succeed(
            EditorOperationCodes.BuildSucceeded,
            "Second complete.");
        IReadOnlyList<EditorOperationResult> boundedSnapshot =
            bounded.CompletedSnapshot();
        Check(
            boundedSnapshot.Count == 2 &&
            boundedSnapshot[0].OperationId == second.OperationId &&
            boundedSnapshot[1].OperationId == third.OperationId,
            "bounded aggregation evicts the oldest start and returns retained results in start order");
        first.Dispose();
        second.Dispose();
        third.Dispose();

        var overflowJournal =
            new EditorOperationJournal(completedCapacity: 1);
        using EditorOperationSession overflow =
            overflowJournal.Begin(
                EditorOperationService.Package,
                EditorOperationCodes.PackageStarted,
                "Overflow fixture.");
        int retainedReports = 0;
        for (int index = 0;
             index <
                 EditorOperationDiagnosticContract
                     .MaximumRetainedDiagnostics * 2;
             index++)
        {
            if (overflow.Report(
                    EditorOperationSeverity.Warning,
                    "ACS.PACKAGE.ITEM_WARNING",
                    $"Package item warning {index}."))
            {
                retainedReports++;
            }
        }
        EditorOperationResult overflowResult = overflow.Succeed(
            EditorOperationCodes.PackageSucceeded,
            "Overflow fixture completed.");
        Check(
            retainedReports ==
                EditorOperationDiagnosticContract
                    .MaximumRetainedDiagnostics - 3 &&
            overflowResult.Diagnostics.Count ==
                EditorOperationDiagnosticContract
                    .MaximumRetainedDiagnostics &&
            overflowResult.Diagnostics
                .Select(item => item.Sequence)
                .SequenceEqual(
                    Enumerable.Range(
                            1,
                            EditorOperationDiagnosticContract
                                .MaximumRetainedDiagnostics)
                        .Select(value => (ulong)value)) &&
            overflowResult.Diagnostics.Count(
                item =>
                    item.Code ==
                    EditorOperationCodes.DiagnosticsTruncated) == 1 &&
            overflowResult.Diagnostics[^2].Code ==
                EditorOperationCodes.DiagnosticsTruncated &&
            overflowResult.Diagnostics[^1].IsTerminal,
            "per-operation aggregation is bounded with one truncation warning and a reserved terminal slot");

        var cancellationJournal =
            new EditorOperationJournal(completedCapacity: 2);
        using EditorOperationSession cancelled =
            cancellationJournal.Begin(
                EditorOperationService.Package,
                EditorOperationCodes.PackageStarted,
                "Cancellation fixture.");
        EditorOperationResult cancelledResult = cancelled.Cancel(
            EditorOperationCodes.PackageCancelled,
            "Cancelled by owner shutdown.");
        cancelled.Dispose();
        Check(
            cancelledResult.State == EditorOperationState.Cancelled &&
            cancelledResult.Diagnostics[^1].Severity ==
                EditorOperationSeverity.Warning &&
            cancelledResult.Diagnostics.Count(item => item.IsTerminal) == 1 &&
            cancellationJournal.ActiveCount == 0,
            "cancellation reaches one durable terminal result before scope disposal");

        Check(
            !EditorOperationCancellationClassifier.IsCancellationRequested(
                operationCancellationRequested: false,
                ownerCloseRequested: false,
                lifetimeCancellationRequested: false) &&
            EditorOperationCancellationClassifier.IsCancellationRequested(
                operationCancellationRequested: true,
                ownerCloseRequested: false,
                lifetimeCancellationRequested: false) &&
            EditorOperationCancellationClassifier.IsCancellationRequested(
                operationCancellationRequested: false,
                ownerCloseRequested: true,
                lifetimeCancellationRequested: false) &&
            EditorOperationCancellationClassifier.IsCancellationRequested(
                operationCancellationRequested: false,
                ownerCloseRequested: false,
                lifetimeCancellationRequested: true),
            "Package exception classification requires an explicit operation, owner, or lifetime cancellation signal");

        var throwingObserver = new EditorOperationJournal(
            completedCapacity: 1,
            observer: _ => throw new InvalidOperationException(
                "Observer failure."));
        using EditorOperationSession isolated =
            throwingObserver.Begin(
                EditorOperationService.Build,
                EditorOperationCodes.BuildStarted,
                "Observer isolation.");
        EditorOperationResult isolatedResult = isolated.Fail(
            EditorOperationCodes.BuildFailed,
            "Expected fixture failure.");
        Check(
            isolatedResult.State == EditorOperationState.Failed &&
            throwingObserver.CompletedSnapshot().Count == 1,
            "diagnostic observers cannot alter lifecycle completion or aggregation");

        string hostileExceptionMessage =
            "\u202E\u001B\uD800" +
            new string(
                'e',
                EditorOperationDiagnosticContract.MaximumMessageLength * 4);
        var hostileJournal =
            new EditorOperationJournal(completedCapacity: 4);
        EditorOperationResult? hostileFailure = null;
        bool hostileBoundaryReturned = false;
        using (EditorOperationSession hostile =
               hostileJournal.Begin(
                   EditorOperationService.Package,
                   "external start code",
                   " ",
                   assetId: new string(
                       'a',
                       EditorOperationDiagnosticContract
                           .MaximumAssetIdLength * 4),
                   path: new string(
                       'p',
                       EditorOperationDiagnosticContract
                           .MaximumPathLength * 2)))
        {
            try
            {
                _ = hostile.Report(
                    EditorOperationSeverity.Error,
                    "external issue/code",
                    hostileExceptionMessage,
                    assetId: hostileExceptionMessage,
                    path: hostileExceptionMessage);
                throw new InvalidOperationException(
                    hostileExceptionMessage);
            }
            catch (Exception error)
            {
                hostileFailure = hostile.Fail(
                    "external terminal/code",
                    error.Message,
                    assetId: hostileExceptionMessage,
                    path: hostileExceptionMessage);
                hostileBoundaryReturned = true;
            }
        }
        Check(
            hostileBoundaryReturned &&
            hostileFailure != null &&
            hostileFailure.State == EditorOperationState.Failed &&
            hostileFailure.Diagnostics.Count == 3 &&
            hostileFailure.Diagnostics.All(
                item =>
                    EditorOperationDiagnosticContract.IsStableCode(
                        item.Code) &&
                    item.Message.Length <=
                        EditorOperationDiagnosticContract
                            .MaximumMessageLength &&
                    (item.AssetId?.Length ?? 0) <=
                        EditorOperationDiagnosticContract
                            .MaximumAssetIdLength &&
                    (item.Path?.Length ?? 0) <=
                        EditorOperationDiagnosticContract
                            .MaximumPathLength) &&
            hostileFailure.Diagnostics[^1].IsTerminal &&
            hostileJournal.ActiveCount == 0,
            "oversized exception, report, and failure-terminal text cannot escape into the owning catch/finally workflow");

        using EditorOperationSession hostileCancel =
            hostileJournal.Begin(
                EditorOperationService.Package,
                EditorOperationCodes.PackageStarted,
                "Cancel fixture.");
        EditorOperationResult hostileCancelled = hostileCancel.Cancel(
            "external cancel code",
            hostileExceptionMessage,
            assetId: hostileExceptionMessage,
            path: hostileExceptionMessage);
        Check(
            hostileCancelled.State == EditorOperationState.Cancelled &&
            hostileCancelled.Diagnostics[^1].Code ==
                EditorOperationCodes.InvalidDiagnosticCode &&
            hostileCancelled.Diagnostics[^1].IsTerminal,
            "cancellation terminal reporting is total for hostile external text");

        EditorOperationSession hostileDispose =
            hostileJournal.Begin(
                EditorOperationService.Build,
                "external dispose code",
                hostileExceptionMessage,
                path: hostileExceptionMessage);
        hostileDispose.Dispose();
        Check(
            hostileDispose.Result?.State ==
                EditorOperationState.Failed &&
            hostileDispose.Result.Diagnostics[^1].Code ==
                EditorOperationCodes.OperationIncomplete &&
            hostileJournal.ActiveCount == 0,
            "scope disposal always records an incomplete terminal after hostile start text");

        EditorOperationSession? reentrant = null;
        bool observerSawUnlockedSession = false;
        var reentrantJournal = new EditorOperationJournal(
            completedCapacity: 1,
            observer: diagnostic =>
            {
                if (reentrant == null ||
                    diagnostic.Code != "ACS.BUILD.OBSERVER_PROBE")
                {
                    return;
                }
                Task<bool> nested = Task.Run(
                    () => reentrant.Report(
                        EditorOperationSeverity.Info,
                        "ACS.BUILD.OBSERVER_REENTRY",
                        "Nested observer diagnostic."));
                observerSawUnlockedSession =
                    nested.Wait(TimeSpan.FromSeconds(2)) &&
                    nested.Result;
            });
        using (reentrant = reentrantJournal.Begin(
                   EditorOperationService.Build,
                   EditorOperationCodes.BuildStarted,
                   "Reentrant observer fixture."))
        {
            reentrant.Report(
                EditorOperationSeverity.Info,
                "ACS.BUILD.OBSERVER_PROBE",
                "Probe observer lock ownership.");
            reentrant.Succeed(
                EditorOperationCodes.BuildSucceeded,
                "Reentrant observer fixture complete.");
        }
        Check(
            observerSawUnlockedSession &&
            reentrantJournal.CompletedSnapshot()[0].Diagnostics
                .Select(item => item.Code)
                .SequenceEqual(
                [
                    EditorOperationCodes.BuildStarted,
                    "ACS.BUILD.OBSERVER_PROBE",
                    "ACS.BUILD.OBSERVER_REENTRY",
                    EditorOperationCodes.BuildSucceeded,
                ]),
            "observer callbacks run outside the session lock and may safely re-enter reporting");

        var concurrentObserved = new List<string>();
        object concurrentObservedGate = new();
        using var concurrentBarrier = new Barrier(2);
        using var releaseConcurrentObserver =
            new ManualResetEventSlim(initialState: false);
        bool observerReachedBarrier = false;
        var concurrentJournal = new EditorOperationJournal(
            completedCapacity: 1,
            observer: diagnostic =>
            {
                if (diagnostic.Code ==
                    "ACS.BUILD.CONCURRENT_BEFORE_TERMINAL")
                {
                    observerReachedBarrier =
                        concurrentBarrier.SignalAndWait(
                            TimeSpan.FromSeconds(2));
                    _ = releaseConcurrentObserver.Wait(
                        TimeSpan.FromSeconds(2));
                }
                lock (concurrentObservedGate)
                    concurrentObserved.Add(diagnostic.Code);
            });
        using EditorOperationSession concurrent =
            concurrentJournal.Begin(
                EditorOperationService.Build,
                EditorOperationCodes.BuildStarted,
                "Concurrent observer ordering fixture.");
        Task<bool> concurrentReport = Task.Run(
            () => concurrent.Report(
                EditorOperationSeverity.Info,
                "ACS.BUILD.CONCURRENT_BEFORE_TERMINAL",
                "This diagnostic must publish before terminal."));
        bool testReachedBarrier =
            concurrentBarrier.SignalAndWait(TimeSpan.FromSeconds(2));
        Task<EditorOperationResult>? concurrentComplete = null;
        bool completeReturnedWhileObserverBlocked = false;
        bool terminalPublishedWhileObserverBlocked = false;
        if (testReachedBarrier)
        {
            concurrentComplete = Task.Run(
                () => concurrent.Succeed(
                    EditorOperationCodes.BuildSucceeded,
                    "Concurrent observer ordering fixture complete."));
            completeReturnedWhileObserverBlocked =
                concurrentComplete.Wait(TimeSpan.FromSeconds(2));
            lock (concurrentObservedGate)
            {
                terminalPublishedWhileObserverBlocked =
                    concurrentObserved.Contains(
                        EditorOperationCodes.BuildSucceeded);
            }
        }
        releaseConcurrentObserver.Set();
        bool reportCompleted =
            concurrentReport.Wait(TimeSpan.FromSeconds(2));
        bool terminalCompleted =
            concurrentComplete?.Wait(TimeSpan.FromSeconds(2)) == true;
        string[] concurrentCodes;
        lock (concurrentObservedGate)
        {
            concurrentCodes = concurrentObserved
                .Where(code =>
                    code ==
                        "ACS.BUILD.CONCURRENT_BEFORE_TERMINAL" ||
                    code == EditorOperationCodes.BuildSucceeded)
                .ToArray();
        }
        Check(
            observerReachedBarrier &&
            testReachedBarrier &&
            completeReturnedWhileObserverBlocked &&
            reportCompleted &&
            terminalCompleted &&
            !terminalPublishedWhileObserverBlocked &&
            concurrentCodes.SequenceEqual(
            [
                "ACS.BUILD.CONCURRENT_BEFORE_TERMINAL",
                EditorOperationCodes.BuildSucceeded,
            ]),
            "a single lock-free observer drainer preserves sequence order across concurrent Report and Complete");

        output.WriteLine(
            failures == 0
                ? "Editor operation diagnostics self-test passed."
                : $"Editor operation diagnostics self-test failed: {failures} check(s).");
        return failures;
    }
}
