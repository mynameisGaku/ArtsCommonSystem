// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;
using System.Linq;

namespace AcsEditor;

internal static class AssetBrowserViewStateSelfTest
{
    internal static int Run(TextWriter output)
    {
        ArgumentNullException.ThrowIfNull(output);
        int passed = 0;
        int failed = 0;

        void Check(bool condition, string label)
        {
            if (condition)
            {
                passed++;
                output.WriteLine("PASS: " + label);
            }
            else
            {
                failed++;
                output.WriteLine("FAIL: " + label);
            }
        }

        static bool Throws<TException>(Action action)
            where TException : Exception
        {
            try
            {
                action();
                return false;
            }
            catch (TException)
            {
                return true;
            }
        }

        static bool Matches(
            string? query,
            string? selectedKind = null,
            string name = "Hero Knight",
            string path = "Characters/Hero Knight.acsbp",
            string kind = "blueprint",
            string id = "ABC-123-DEF",
            bool isDirectory = false) =>
            AssetBrowserQuery.Matches(
                query, selectedKind, name, path, kind, id, isDirectory);

        try
        {
            Check(Matches(null) && Matches("") && Matches("   \t"),
                "null, empty, and whitespace queries match without an active kind filter");
            Check(Matches("hero") && Matches("characters") && Matches("BLUEPRINT"),
                "bare terms search name, relative path, and effective kind case-insensitively");
            Check(!Matches("ABC-123-DEF"),
                "bare terms do not accidentally expose the authoritative asset id field");

            Check(Matches("name:\"Hero Knight\"") &&
                  Matches("path:\"Characters/Hero Knight.acsbp\"") &&
                  Matches("type:blue") && Matches("kind:PRINT") &&
                  Matches("id:abc-123"),
                "name, quoted path, type/kind, and id fields address their intended values");
            Check(!Matches("name:Villain") && !Matches("path:Props") &&
                  !Matches("type:material") && !Matches("id:missing") &&
                  !Matches("id:"),
                "field queries and empty field values fail closed on mismatches");
            Check(Matches("name:hero type:blue path:characters id:DEF"),
                "multiple positive fields use deterministic AND semantics");
            Check(!Matches("name:hero type:material"),
                "one failed positive term rejects an otherwise matching asset");

            Check(Matches("hero -path:villains -type:material") &&
                  !Matches("hero -path:characters") &&
                  !Matches("-type:blueprint") &&
                  !Matches("-id:123"),
                "leading-minus exclusions work for bare and typed terms");
            Check(Matches("-name:\"Villain Boss\"") &&
                  !Matches("-name:\"Hero Knight\"") &&
                  !Matches("-\"Hero Knight\""),
                "quoted exclusion values preserve embedded spaces");
            Check(Matches("\"Hero Knight\"") &&
                  Matches("name:\"Hero Knight") &&
                  !Matches("name:\"\""),
                "quoted bare terms and unterminated quotes remain deterministic");
            Check(Matches("-") && Matches("hero -"),
                "a standalone exclusion marker is ignored instead of becoming a match-all ban");

            Check(Matches("path:C:\\Games\\ACS", path: "C:\\Games\\ACS\\Hero.acsbp"),
                "path values may contain a second colon and Windows separators");
            Check(Matches("tag:hero", name: "tag:hero") && !Matches("tag:hero"),
                "unknown field prefixes fall back to literal bare-term matching");

            Check(Matches("hero", selectedKind: " all ") &&
                  Matches("hero", selectedKind: " BLUEPRINT ") &&
                  !Matches("hero", selectedKind: "material"),
                "selected type filtering trims input and recognizes all case-insensitively");
            Check(Matches("type:folder", selectedKind: "folder", kind: "texture", isDirectory: true) &&
                  !Matches("type:texture", selectedKind: "folder", kind: "texture", isDirectory: true),
                "directories consistently use folder as both query and selected-filter kind");

            string boundaryName = new('x', 1024);
            Check(Matches(boundaryName + " -name:" + boundaryName, name: boundaryName),
                "queries longer than 1024 UTF-16 code units are bounded before evaluation");
            string termBoundary =
                string.Join(' ', Enumerable.Repeat("hero", 32)) + " -hero";
            Check(Matches(termBoundary),
                "only the first 32 terms participate and excess exclusions cannot change the result");

            var emptyHistory = new AssetBrowserHistory();
            Check(emptyHistory.Current == null && !emptyHistory.CanGoBack &&
                  !emptyHistory.CanGoForward && emptyHistory.Back() == null &&
                  emptyHistory.Forward() == null,
                "empty history navigation is total and non-mutating");
            Check(Throws<ArgumentNullException>(() => emptyHistory.Reset(null!)) &&
                  Throws<ArgumentException>(() => emptyHistory.Reset("  ")) &&
                  Throws<ArgumentNullException>(() => emptyHistory.Navigate(null!)) &&
                  Throws<ArgumentException>(() => emptyHistory.Navigate("\t")),
                "history rejects null and whitespace paths without publishing an entry");

            Check(emptyHistory.Navigate("Assets") && emptyHistory.Current == "Assets" &&
                  !emptyHistory.CanGoBack && !emptyHistory.CanGoForward,
                "Navigate initializes an empty history");
            Check(!emptyHistory.Navigate("assets") && emptyHistory.Current == "Assets",
                "case-only duplicate navigation is ignored without replacing the canonical path");

            var history = new AssetBrowserHistory();
            history.Reset("A");
            history.Navigate("B");
            history.Navigate("C");
            Check(history.Back() == "B" && history.Back() == "A" && history.Back() == null,
                "Back stops exactly at the oldest retained entry");
            Check(history.Forward() == "B" && history.CanGoForward,
                "Forward returns the next entry after backward navigation");
            Check(!history.Navigate("b") && history.CanGoForward && history.Forward() == "C",
                "a duplicate current path preserves the existing forward chain");

            history.Back();
            Check(history.Navigate("D") && history.Current == "D" &&
                  !history.CanGoForward && history.Back() == "B" &&
                  history.Forward() == "D",
                "new navigation after Back truncates the old forward branch");
            history.Reset("Root");
            Check(history.Current == "Root" && !history.CanGoBack && !history.CanGoForward,
                "Reset atomically replaces all prior history");

            var bounded = new AssetBrowserHistory();
            bounded.Reset("P0");
            for (int index = 1; index <= 70; index++)
                bounded.Navigate("P" + index);
            int backCount = 0;
            while (bounded.Back() != null) backCount++;
            Check(backCount == 63 && bounded.Current == "P7" && !bounded.CanGoBack,
                "history retains exactly the newest 64 entries at capacity");
            Check(bounded.Navigate("Branch") && bounded.Back() == "P7" &&
                  bounded.Forward() == "Branch" && !bounded.CanGoForward,
                "branching from the oldest retained entry truncates all discarded forward entries");

            string dropRoot = Path.GetFullPath(Path.Combine(
                Path.GetTempPath(), "acs-asset-browser-drop-policy", "Assets"));
            string sourceFile = Path.Combine(dropRoot, "Hero.acsbp");
            string sourceFolder = Path.Combine(dropRoot, "Characters");
            string destination = Path.Combine(dropRoot, "Organized");
            var filePayload = new AssetBrowserDragPayload(
                42,
                dropRoot,
                new[] { new AssetBrowserDragEntry(sourceFile, IsDirectory: false) });
            AssetBrowserDropPlan filePlan = AssetBrowserDropPolicy.Evaluate(
                filePayload, 42, dropRoot, destination, destinationIsDirectory: true);
            Check(filePlan.IsValid && filePlan.CanMove && filePlan.CanCopy &&
                  filePlan.SourcePaths.SequenceEqual(new[] { sourceFile },
                      StringComparer.OrdinalIgnoreCase) &&
                  string.Equals(
                      filePlan.DestinationDirectory,
                      destination,
                      StringComparison.OrdinalIgnoreCase),
                "folder-tile drops allow both move and copy for a current file payload");

            var mixedPayload = new AssetBrowserDragPayload(
                42,
                dropRoot,
                new[]
                {
                    new AssetBrowserDragEntry(sourceFile, IsDirectory: false),
                    new AssetBrowserDragEntry(sourceFolder, IsDirectory: true),
                });
            AssetBrowserDropPlan mixedPlan = AssetBrowserDropPolicy.Evaluate(
                mixedPayload, 42, dropRoot, destination, destinationIsDirectory: true);
            Check(mixedPlan.IsValid && mixedPlan.SourcePaths.Count == 2,
                "mixed file and folder selections remain valid internal drop payloads");

            AssetBrowserDropPlan sameParent = AssetBrowserDropPolicy.Evaluate(
                filePayload, 42, dropRoot, dropRoot, destinationIsDirectory: true);
            Check(sameParent.IsValid && !sameParent.CanMove && sameParent.CanCopy,
                "same-parent drops suppress the no-op move while retaining Copy Here");

            Check(!AssetBrowserDropPolicy.Evaluate(
                      filePayload, 41, dropRoot, destination, true).IsValid &&
                  !AssetBrowserDropPolicy.Evaluate(
                      filePayload, 42, dropRoot + "-other", destination, true).IsValid &&
                  !AssetBrowserDropPolicy.Evaluate(
                      filePayload, 42, dropRoot, destination, false).IsValid,
                "stale generations, different projects, and non-folder tiles reject drops");

            var externalPayload = new AssetBrowserDragPayload(
                42,
                dropRoot,
                new[]
                {
                    new AssetBrowserDragEntry(
                        Path.Combine(Path.GetDirectoryName(dropRoot)!, "Outside.acsbp"),
                        IsDirectory: false),
                });
            Check(!AssetBrowserDropPolicy.Evaluate(
                      externalPayload, 42, dropRoot, destination, true).IsValid &&
                  !AssetBrowserDropPolicy.Evaluate(
                      filePayload,
                      42,
                      dropRoot,
                      Path.Combine(Path.GetDirectoryName(dropRoot)!, "Outside"),
                      true).IsValid,
                "source and destination paths cannot escape the current Assets root");

            var folderPayload = new AssetBrowserDragPayload(
                42,
                dropRoot,
                new[] { new AssetBrowserDragEntry(sourceFolder, IsDirectory: true) });
            Check(!AssetBrowserDropPolicy.Evaluate(
                      folderPayload, 42, dropRoot, sourceFolder, true).IsValid &&
                  !AssetBrowserDropPolicy.Evaluate(
                      folderPayload,
                      42,
                      dropRoot,
                      Path.Combine(sourceFolder, "Nested"),
                      true).IsValid,
                "folders cannot be dropped onto themselves or descendants");

            var duplicatePayload = new AssetBrowserDragPayload(
                42,
                dropRoot.ToUpperInvariant(),
                new[]
                {
                    new AssetBrowserDragEntry(sourceFile, IsDirectory: false),
                    new AssetBrowserDragEntry(sourceFile.ToUpperInvariant(), IsDirectory: false),
                });
            AssetBrowserDropPlan duplicatePlan = AssetBrowserDropPolicy.Evaluate(
                duplicatePayload, 42, dropRoot, destination, true);
            Check(duplicatePlan.IsValid && duplicatePlan.SourcePaths.Count == 1,
                "drop policy compares roots and duplicate paths case-insensitively");

            var inconsistentPayload = new AssetBrowserDragPayload(
                42,
                dropRoot,
                new[]
                {
                    new AssetBrowserDragEntry(sourceFile, IsDirectory: false),
                    new AssetBrowserDragEntry(sourceFile, IsDirectory: true),
                });
            var oversizedPayload = new AssetBrowserDragPayload(
                42,
                dropRoot,
                Enumerable.Range(0, 4097)
                    .Select(index => new AssetBrowserDragEntry(
                        Path.Combine(dropRoot, index + ".txt"),
                        IsDirectory: false))
                    .ToArray());
            Check(!AssetBrowserDropPolicy.Evaluate(
                      inconsistentPayload, 42, dropRoot, destination, true).IsValid &&
                  !AssetBrowserDropPolicy.Evaluate(
                      oversizedPayload, 42, dropRoot, destination, true).IsValid,
                "inconsistent duplicates and unbounded selections fail closed");

            string externalRoot = Path.GetFullPath(Path.Combine(
                Path.GetTempPath(),
                "acs-asset-browser-external-drop"));
            string externalA = Path.Combine(externalRoot, "Aircraft.png");
            string externalB = Path.Combine(externalRoot, "Engine.wav");
            AssetBrowserImportDropPlan importPlan =
                AssetBrowserDropPolicy.EvaluateExternalImport(
                    new[] { externalB, externalA, externalA.ToUpperInvariant() },
                    dropRoot,
                    destination,
                    destinationIsDirectory: true);
            Check(importPlan.IsValid &&
                  string.Equals(
                      importPlan.DestinationDirectory,
                      destination,
                      StringComparison.OrdinalIgnoreCase) &&
                  importPlan.SourcePaths.SequenceEqual(
                      new[] { externalA, externalB },
                      StringComparer.OrdinalIgnoreCase),
                "external file drops are canonicalized, deduplicated, and sorted for a folder import");

            AssetBrowserImportDropPlan backgroundImport =
                AssetBrowserDropPolicy.EvaluateExternalImport(
                    new[] { externalA },
                    dropRoot,
                    dropRoot,
                    destinationIsDirectory: true);
            Check(backgroundImport.IsValid &&
                  string.Equals(
                      backgroundImport.DestinationDirectory,
                      dropRoot,
                      StringComparison.OrdinalIgnoreCase),
                "background external drops import into the current Assets directory");

            string databaseInternals = Path.Combine(
                dropRoot,
                AssetDatabase.InternalDirectoryName,
                "payload");
            Check(!AssetBrowserDropPolicy.EvaluateExternalImport(
                      new[] { externalA },
                      dropRoot,
                      Path.Combine(dropRoot, AssetDatabase.InternalDirectoryName),
                      destinationIsDirectory: true).IsValid &&
                  !AssetBrowserDropPolicy.EvaluateExternalImport(
                      new[] { databaseInternals },
                      dropRoot,
                      destination,
                      destinationIsDirectory: true).IsValid &&
                  !AssetBrowserDropPolicy.EvaluateExternalImport(
                      new[] { sourceFile },
                      dropRoot,
                      destination,
                      destinationIsDirectory: true).IsValid &&
                  !AssetBrowserDropPolicy.EvaluateExternalImport(
                      new[] { externalA },
                      dropRoot,
                      Path.Combine(Path.GetDirectoryName(dropRoot)!, "Outside"),
                      destinationIsDirectory: true).IsValid,
                "external import drops cannot target database internals, reimport managed Assets, or escape Assets");

            string[] oversizedExternalDrop = Enumerable.Range(0, 4097)
                .Select(index => Path.Combine(externalRoot, index + ".bin"))
                .ToArray();
            Check(!AssetBrowserDropPolicy.EvaluateExternalImport(
                      null,
                      dropRoot,
                      destination,
                      destinationIsDirectory: true).IsValid &&
                  !AssetBrowserDropPolicy.EvaluateExternalImport(
                      Array.Empty<string>(),
                      dropRoot,
                      destination,
                      destinationIsDirectory: true).IsValid &&
                  !AssetBrowserDropPolicy.EvaluateExternalImport(
                      new[] { "relative.png" },
                      dropRoot,
                      destination,
                      destinationIsDirectory: true).IsValid &&
                  !AssetBrowserDropPolicy.EvaluateExternalImport(
                      oversizedExternalDrop,
                      dropRoot,
                      destination,
                      destinationIsDirectory: true).IsValid &&
                  !AssetBrowserDropPolicy.EvaluateExternalImport(
                      new[] { externalA },
                      dropRoot,
                      destination,
                      destinationIsDirectory: false).IsValid,
                "empty, relative, oversized, and non-folder external import drops fail closed");
        }
        catch (Exception error)
        {
            failed++;
            output.WriteLine("FAIL: unhandled exception: " + error);
        }

        output.WriteLine(
            $"Asset Browser view-state self-test: {passed} PASS / {failed} failures");
        return failed;
    }
}
