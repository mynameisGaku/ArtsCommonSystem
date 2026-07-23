// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;

namespace AcsEditor;

internal static class AssetAdvancedManagementSelfTest
{
    internal static int Run(TextWriter output)
    {
        ArgumentNullException.ThrowIfNull(output);
        int passed = 0;
        int failed = 0;
        string root = Path.Combine(
            Path.GetTempPath(),
            "acs-asset-advanced-selftest-" + Guid.NewGuid().ToString("N"));
        string sourceProject = Path.Combine(root, "SourceProject");
        string sourceAssets = Path.Combine(sourceProject, "Assets");
        string targetProject = Path.Combine(root, "TargetProject");
        string targetAssets = Path.Combine(targetProject, "Assets");

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

        static bool Throws<T>(Action action) where T : Exception
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

        static bool RejectsForHeldMutationLock(Action action)
        {
            try
            {
                action();
                return false;
            }
            catch (IOException error)
            {
                return error.Message.Contains(
                           "another editor",
                           StringComparison.OrdinalIgnoreCase) &&
                       error.Message.Contains(
                           "mutation lock",
                           StringComparison.OrdinalIgnoreCase);
            }
        }

        static void Write(string path, string text)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, text, new UTF8Encoding(false));
        }

        try
        {
            Directory.CreateDirectory(sourceAssets);
            Directory.CreateDirectory(targetAssets);
            string sourceMaterial = Path.Combine(sourceAssets, "Source.acsmat");
            string targetMaterial = Path.Combine(sourceAssets, "Target.acsmat");
            string targetGraph = targetMaterial + ".graph.json";
            string consumer = Path.Combine(sourceAssets, "UsesMaterial.acsbp");
            Write(sourceMaterial, "ACSMAT 1\nname Source\nkind pbr\n");
            Write(targetMaterial, "ACSMAT 1\nname Target\nkind pbr\n");
            Write(
                targetGraph,
                "{\"material\":\"Assets/Target.acsmat\",\"absolute\":\"" +
                targetMaterial.Replace("\\", "\\\\", StringComparison.Ordinal) +
                "\"}\n");
            Write(consumer, "ACSBP 1\n");

            var database = new AssetDatabase(sourceProject, sourceAssets);
            database.Refresh(verifyContent: true);
            var workflow = new AssetManagementWorkflow(database);
            database.TryGetByPath(sourceMaterial, out AssetRecord? sourceRecord);
            database.TryGetByPath(targetMaterial, out AssetRecord? targetRecord);
            database.TryGetByPath(consumer, out AssetRecord? consumerRecord);
            Check(sourceRecord != null && targetRecord != null && consumerRecord != null,
                "advanced management fixture assets are indexed");

            using (AssetMutationLockProcessHolder held =
                   AssetMutationLockProcessHolder.Start(sourceAssets))
            {
                Check(
                    RejectsForHeldMutationLock(() =>
                        database.Refresh(verifyContent: true)) &&
                    RejectsForHeldMutationLock(() =>
                        AssetCreationWorkflow.Create(
                            sourceAssets,
                            sourceAssets,
                            AcsAssetTemplate.Blueprint)) &&
                    RejectsForHeldMutationLock(() =>
                        workflow.Duplicate(new[] { sourceMaterial })) &&
                    File.Exists(sourceMaterial) &&
                    !File.Exists(Path.Combine(sourceAssets, "Blueprint.acsbp")) &&
                    !File.Exists(Path.Combine(sourceAssets, "Source1.acsmat")),
                    "a separate process lock rejects Refresh, Create, and Duplicate before mutation");
            }

            Write(
                consumer,
                "ACSBP 1\n" +
                "material Assets/Source.acsmat\n" +
                $"asset-id {sourceRecord!.AssetId}\n" +
                $"absolute {sourceMaterial}\n" +
                "not-a-reference Assets/Source.acsmat.bak\n");
            database.Refresh(verifyContent: true);
            database.TryGetByPath(consumer, out consumerRecord);
            database.UpdateImportMetadata(
                consumerRecord!.AssetId,
                consumer,
                "blueprint",
                3,
                new[] { sourceRecord.AssetId },
                new[]
                {
                    new KeyValuePair<string, string>(
                        "materialPath",
                        "Assets/Source.acsmat"),
                    new KeyValuePair<string, string>(
                        "materialId",
                        sourceRecord.AssetId),
                });

            AssetReplaceReferencesPreview metadataStalePreview =
                workflow.PreviewReplaceReferences(
                    new[] { sourceRecord.AssetId },
                    targetRecord!.AssetId);
            var competingDatabase = new AssetDatabase(sourceProject, sourceAssets);
            competingDatabase.Refresh(verifyContent: true);
            competingDatabase.TryGetByPath(consumer, out AssetRecord? competingConsumer);
            competingDatabase.UpdateImportMetadata(
                competingConsumer!.AssetId,
                competingConsumer.Metadata.Source,
                competingConsumer.Metadata.Importer,
                competingConsumer.Metadata.ImporterVersion,
                competingConsumer.Metadata.Dependencies,
                competingConsumer.Metadata.ImportSettings.Append(
                    new KeyValuePair<string, string>(
                        "externalMarker",
                        "preserve-me")));
            Check(Throws<IOException>(() =>
                      workflow.CommitReplaceReferences(metadataStalePreview)) &&
                  database.TryGetByPath(consumer, out AssetRecord? refreshedConsumer) &&
                  refreshedConsumer!.Metadata.ImportSettings.GetValueOrDefault(
                      "externalMarker") == "preserve-me" &&
                  refreshedConsumer.Metadata.Dependencies.SequenceEqual(
                      new[] { sourceRecord.AssetId },
                      StringComparer.OrdinalIgnoreCase),
                "Replace References refreshes under its lease and preserves a competing metadata update");

            AssetReplaceReferencesPreview stalePreview =
                workflow.PreviewReplaceReferences(
                    new[] { sourceRecord.AssetId },
                    targetRecord!.AssetId);
            Check(stalePreview.Edits.Any(edit =>
                      edit.RelativePath.Equals(
                          "UsesMaterial.acsbp",
                          StringComparison.OrdinalIgnoreCase) &&
                      edit.ContentChanged &&
                      edit.MetadataChanged) &&
                  stalePreview.Edits.Sum(static edit => edit.ReplacementCount) >= 5,
                "Replace References preview reports content and metadata changes without mutation");
            File.AppendAllText(consumer, "preview changed\n", new UTF8Encoding(false));
            Check(Throws<IOException>(() =>
                    workflow.CommitReplaceReferences(stalePreview)) &&
                  File.ReadAllText(consumer).Contains(
                      "Assets/Source.acsmat",
                      StringComparison.Ordinal),
                "Replace References rejects a stale preview before writing");

            AssetReplaceReferencesPreview preview =
                workflow.PreviewReplaceReferences(
                    new[] { sourceRecord.AssetId },
                    targetRecord.AssetId);
            string beforeContendedReplace = File.ReadAllText(consumer);
            using (AssetMutationLockProcessHolder held =
                   AssetMutationLockProcessHolder.Start(sourceAssets))
            {
                Check(
                    RejectsForHeldMutationLock(() =>
                        workflow.CommitReplaceReferences(preview)) &&
                    File.ReadAllText(consumer) == beforeContendedReplace,
                    "a separate process lock rejects Replace References before writing");
            }
            AssetReplaceReferencesResult replaced =
                workflow.CommitReplaceReferences(preview);
            database.TryGetByPath(consumer, out consumerRecord);
            string replacedText = File.ReadAllText(consumer);
            Check(replaced.ContentFileCount >= 1 &&
                  replaced.MetadataAssetCount >= 1 &&
                  replacedText.Contains("Assets/Target.acsmat", StringComparison.Ordinal) &&
                  replacedText.Contains(targetRecord.AssetId, StringComparison.Ordinal) &&
                  replacedText.Contains(targetMaterial, StringComparison.OrdinalIgnoreCase) &&
                  !replacedText.Contains(sourceRecord.AssetId, StringComparison.OrdinalIgnoreCase) &&
                  replacedText.Contains(
                      "Assets/Source.acsmat.bak",
                      StringComparison.Ordinal) &&
                  consumerRecord!.Metadata.Dependencies.SequenceEqual(
                      new[] { targetRecord.AssetId },
                      StringComparer.OrdinalIgnoreCase) &&
                  consumerRecord.Metadata.ImportSettings.GetValueOrDefault("materialPath") ==
                      "Assets/Target.acsmat" &&
                  consumerRecord.Metadata.ImportSettings.GetValueOrDefault("materialId") ==
                      targetRecord.AssetId,
                "Replace References commits GUID/path/content/metadata changes with boundary safety");

            using (AssetMutationLockProcessHolder held =
                   AssetMutationLockProcessHolder.Start(sourceAssets))
            {
                Check(
                    RejectsForHeldMutationLock(() =>
                        workflow.RenameWithRedirector(
                            sourceMaterial,
                            sourceRecord.AssetId,
                            isDirectory: false,
                            "SourceRenamed")) &&
                    File.Exists(sourceMaterial) &&
                    !File.Exists(Path.Combine(sourceAssets, "SourceRenamed.acsmat")),
                    "a separate process lock rejects redirected rename before mutation");
            }
            string renamedSource = workflow.RenameWithRedirector(
                sourceMaterial,
                sourceRecord.AssetId,
                isDirectory: false,
                "SourceRenamed");
            Check(!File.Exists(sourceMaterial) &&
                  File.Exists(renamedSource) &&
                  workflow.TryResolveRedirectedPath(
                      "Assets/Source.acsmat",
                      out string resolvedFirst) &&
                  string.Equals(
                      resolvedFirst,
                      renamedSource,
                      StringComparison.OrdinalIgnoreCase) &&
                  workflow.SnapshotRedirectors().Any(entry =>
                      entry.OriginalRelativePath.Equals(
                          "Source.acsmat",
                          StringComparison.OrdinalIgnoreCase) &&
                      entry.AssetId == sourceRecord.AssetId) &&
                  File.Exists(Path.Combine(
                      sourceAssets,
                      AssetDatabase.InternalDirectoryName,
                      "redirectors.v1.json")),
                "rename publishes a durable redirector that resolves the stable GUID");

            string finalSource = workflow.RenameWithRedirector(
                renamedSource,
                sourceRecord.AssetId,
                isDirectory: false,
                "SourceFinal");
            Check(workflow.TryResolveRedirectedPath(
                      "Source.acsmat",
                      out string resolvedOriginal) &&
                  workflow.TryResolveRedirectedPath(
                      "SourceRenamed.acsmat",
                      out string resolvedIntermediate) &&
                  string.Equals(
                      resolvedOriginal,
                      finalSource,
                      StringComparison.OrdinalIgnoreCase) &&
                  string.Equals(
                      resolvedIntermediate,
                      finalSource,
                      StringComparison.OrdinalIgnoreCase),
                "redirectors follow repeated renames by stable GUID without fragile path chains");

            Write(sourceMaterial, "ACSMAT 1\nname Reused\nkind pbr\n");
            database.Refresh(verifyContent: true);
            database.TryGetByPath(sourceMaterial, out AssetRecord? reusedRecord);
            Check(reusedRecord != null &&
                  reusedRecord.AssetId != sourceRecord.AssetId &&
                  workflow.TryResolveRedirectedPath(
                      "Assets/Source.acsmat",
                      out string resolvedReuse) &&
                  string.Equals(
                      resolvedReuse,
                      sourceMaterial,
                      StringComparison.OrdinalIgnoreCase),
                "a real asset at a reused old path takes precedence over its redirector");
            workflow.Delete(new[] { sourceMaterial });
            Check(workflow.TryResolveRedirectedPath(
                      "Assets/Source.acsmat",
                      out string resolvedAfterReuse) &&
                  string.Equals(
                      resolvedAfterReuse,
                      finalSource,
                      StringComparison.OrdinalIgnoreCase),
                "redirector resumes stable-GUID resolution after a reused path is removed");

            Write(sourceMaterial, "ACSMAT 1\nname ReusedForFixup\nkind pbr\n");
            database.Refresh(verifyContent: true);
            AssetRedirectorFixupPreview fixupPreview =
                workflow.PreviewFixUpRedirectors();
            Check(fixupPreview.Items.Any(item =>
                      item.OriginalRelativePath.Equals(
                          "Source.acsmat",
                          StringComparison.OrdinalIgnoreCase) &&
                      item.Action == AssetRedirectorFixupAction.RemoveShadowedPath),
                "Fix Up Redirectors preview identifies a shadowed old path without mutation");
            AssetRedirectorFixupResult fixedUp =
                default!;
            using (AssetMutationLockProcessHolder held =
                   AssetMutationLockProcessHolder.Start(sourceAssets))
            {
                Check(
                    RejectsForHeldMutationLock(() =>
                        workflow.CommitFixUpRedirectors(fixupPreview)),
                    "a separate process lock rejects redirector registry cleanup");
            }
            var fixupCompetingDatabase = new AssetDatabase(sourceProject, sourceAssets);
            fixupCompetingDatabase.Refresh(verifyContent: true);
            var fixupCompetingWorkflow =
                new AssetManagementWorkflow(fixupCompetingDatabase);
            fixupCompetingWorkflow.Delete(new[] { sourceMaterial });
            string fixupRegistry = Path.Combine(
                sourceAssets,
                AssetDatabase.InternalDirectoryName,
                "redirectors.v1.json");
            byte[] registryBeforeRejectedFixup = File.ReadAllBytes(fixupRegistry);
            Check(
                Throws<IOException>(() =>
                    workflow.CommitFixUpRedirectors(fixupPreview)) &&
                File.ReadAllBytes(fixupRegistry)
                    .SequenceEqual(registryBeforeRejectedFixup) &&
                workflow.TryResolveRedirectedPath(
                    "Assets/Source.acsmat",
                    out string resolvedAfterRejectedFixup) &&
                string.Equals(
                    resolvedAfterRejectedFixup,
                    finalSource,
                    StringComparison.OrdinalIgnoreCase),
                "Fix Up Redirectors rejects a stale shadowed-path preview and preserves the valid redirect");

            Write(sourceMaterial, "ACSMAT 1\nname ReusedForFixup\nkind pbr\n");
            database.Refresh(verifyContent: true);
            fixupPreview = workflow.PreviewFixUpRedirectors();
            fixedUp = workflow.CommitFixUpRedirectors(fixupPreview);
            workflow.Delete(new[] { sourceMaterial });
            Check(fixedUp.RemovedCount == 1 &&
                  !workflow.TryResolveRedirectedPath(
                      "Assets/Source.acsmat",
                      out _) &&
                  workflow.TryResolveRedirectedPath(
                      "Assets/SourceRenamed.acsmat",
                      out string remainingRedirect) &&
                  string.Equals(
                      remainingRedirect,
                      finalSource,
                      StringComparison.OrdinalIgnoreCase),
                "Fix Up Redirectors prunes only previewed unsafe entries and retains valid redirects");

            string moveFolder = Path.Combine(sourceAssets, "MoveFolder");
            string moveChild = Path.Combine(moveFolder, "Child.txt");
            string moveDestination = Path.Combine(sourceAssets, "Moved");
            Write(moveChild, "redirect me\n");
            Directory.CreateDirectory(moveDestination);
            database.Refresh(verifyContent: true);
            database.TryGetByPath(moveChild, out AssetRecord? moveChildRecord);
            using (AssetMutationLockProcessHolder held =
                   AssetMutationLockProcessHolder.Start(sourceAssets))
            {
                Check(
                    RejectsForHeldMutationLock(() =>
                        workflow.MoveWithRedirectors(
                            new[] { moveFolder },
                            moveDestination)) &&
                    Directory.Exists(moveFolder) &&
                    !Directory.Exists(Path.Combine(moveDestination, "MoveFolder")),
                    "a separate process lock rejects redirected moves before mutation");
            }
            AssetMoveResult redirectedMove = workflow.MoveWithRedirectors(
                new[] { moveFolder },
                moveDestination);
            string movedChild = Path.Combine(moveDestination, "MoveFolder", "Child.txt");
            Check(redirectedMove.PublishedPaths.SequenceEqual(
                      new[] { Path.Combine(moveDestination, "MoveFolder") }) &&
                  workflow.TryResolveRedirectedPath(
                      "Assets/MoveFolder/Child.txt",
                      out string resolvedMovedChild) &&
                  string.Equals(
                      resolvedMovedChild,
                      movedChild,
                      StringComparison.OrdinalIgnoreCase) &&
                  database.TryGetByAssetId(
                      moveChildRecord!.AssetId,
                      out AssetRecord? currentMovedChild) &&
                    string.Equals(
                        currentMovedChild?.FullPath,
                        movedChild,
                        StringComparison.OrdinalIgnoreCase),
                "folder move publishes child redirectors while preserving every stable GUID");

            string noOpVictim = Path.Combine(sourceAssets, "NoOpVictim.acsmat");
            string noOpStationary = Path.Combine(sourceAssets, "NoOpStationary.txt");
            Write(noOpVictim, "ACSMAT 1\nname NoOpVictim\nkind pbr\n");
            Write(noOpStationary, "same-parent no-op\n");
            database.Refresh(verifyContent: true);
            database.TryGetByPath(noOpVictim, out AssetRecord? noOpVictimRecord);
            var noOpCompetingDatabase = new AssetDatabase(sourceProject, sourceAssets);
            noOpCompetingDatabase.Refresh(verifyContent: true);
            var noOpCompetingWorkflow =
                new AssetManagementWorkflow(noOpCompetingDatabase);
            string renamedNoOpVictim = noOpCompetingWorkflow.RenameWithRedirector(
                noOpVictim,
                noOpVictimRecord!.AssetId,
                isDirectory: false,
                "NoOpVictimMoved");
            AssetMoveResult noOpMove = workflow.MoveWithRedirectors(
                new[] { noOpStationary },
                sourceAssets);
            Check(noOpMove.Mappings.Count == 0 &&
                  File.Exists(noOpStationary) &&
                  workflow.SnapshotRedirectors().Any(entry =>
                      entry.OriginalRelativePath.Equals(
                          "NoOpVictim.acsmat",
                          StringComparison.OrdinalIgnoreCase) &&
                      entry.AssetId.Equals(
                          noOpVictimRecord.AssetId,
                          StringComparison.OrdinalIgnoreCase) &&
                      entry.CurrentRelativePath.Equals(
                          "NoOpVictimMoved.acsmat",
                          StringComparison.OrdinalIgnoreCase)) &&
                  workflow.TryResolveRedirectedPath(
                      "Assets/NoOpVictim.acsmat",
                      out string resolvedNoOpVictim) &&
                  string.Equals(
                      resolvedNoOpVictim,
                      renamedNoOpVictim,
                      StringComparison.OrdinalIgnoreCase),
                "same-parent no-op move preserves a competing editor's redirector");

            database.TryGetByPath(consumer, out consumerRecord);
            database.UpdateImportMetadata(
                consumerRecord!.AssetId,
                consumer,
                "blueprint",
                3,
                new[] { targetRecord.AssetId },
                consumerRecord.Metadata.ImportSettings);
            AssetMigrationPreview migrationPreview = workflow.PreviewMigrate(
                new[] { consumerRecord.AssetId },
                targetProject);
            Check(migrationPreview.AssetIds.Count == 2 &&
                  migrationPreview.AssetIds.Contains(
                      consumerRecord.AssetId,
                      StringComparer.OrdinalIgnoreCase) &&
                  migrationPreview.AssetIds.Contains(
                      targetRecord.AssetId,
                      StringComparer.OrdinalIgnoreCase) &&
                  migrationPreview.Files.Any(file =>
                      file.RelativePath.Equals(
                          "Target.acsmat.graph.json",
                          StringComparison.OrdinalIgnoreCase)),
                "Migrate preview computes the authoritative dependency closure and asset families");
            string overlappingProject = Path.Combine(sourceAssets, "NestedProject");
            Directory.CreateDirectory(Path.Combine(overlappingProject, "Assets"));
            Check(Throws<InvalidDataException>(() => workflow.PreviewMigrate(
                    new[] { consumerRecord.AssetId },
                    overlappingProject)),
                "Migrate rejects overlapping source and destination Assets roots");
            Directory.Delete(overlappingProject, recursive: true);

            string collision = Path.Combine(targetAssets, "UsesMaterial.acsbp");
            Write(collision, "collision");
            Check(Throws<IOException>(() => workflow.CommitMigrate(migrationPreview)) &&
                  File.ReadAllText(collision) == "collision" &&
                  !File.Exists(Path.Combine(targetAssets, "Target.acsmat")),
                "Migrate invalidates a preview on destination collision and never overwrites");
            File.Delete(collision);

            migrationPreview = workflow.PreviewMigrate(
                new[] { consumerRecord.AssetId },
                targetProject);
            using (AssetMutationLockProcessHolder held =
                   AssetMutationLockProcessHolder.Start(targetAssets))
            {
                Check(
                    RejectsForHeldMutationLock(() =>
                        workflow.CommitMigrate(migrationPreview)) &&
                    !File.Exists(Path.Combine(targetAssets, "UsesMaterial.acsbp")) &&
                    !File.Exists(Path.Combine(targetAssets, "Target.acsmat")),
                    "a separate target-project process lock rejects Migrate before publish");
            }
            using (AssetMutationLockProcessHolder held =
                   AssetMutationLockProcessHolder.Start(sourceAssets))
            {
                Check(
                    RejectsForHeldMutationLock(() =>
                        workflow.CommitMigrate(migrationPreview)) &&
                    !File.Exists(Path.Combine(targetAssets, "UsesMaterial.acsbp")) &&
                    !File.Exists(Path.Combine(targetAssets, "Target.acsmat")),
                    "a separate source-project process lock rejects Migrate before copying");
            }
            AssetMigrationResult migrated = workflow.CommitMigrate(migrationPreview);
            var targetDatabase = new AssetDatabase(targetProject, targetAssets);
            targetDatabase.RefreshForCook();
            targetDatabase.TryGetByAssetId(
                consumerRecord.AssetId,
                out AssetRecord? migratedConsumer);
            targetDatabase.TryGetByAssetId(
                targetRecord.AssetId,
                out AssetRecord? migratedTarget);
            string migratedConsumerText = File.ReadAllText(
                Path.Combine(targetAssets, "UsesMaterial.acsbp"));
            string migratedGraphText = File.ReadAllText(
                Path.Combine(targetAssets, "Target.acsmat.graph.json"));
            bool migrationVerified = migrated.AssetCount == 2 &&
                  migrated.FileCount == migrationPreview.Files.Count &&
                  migratedConsumer != null &&
                  migratedTarget != null &&
                  migratedConsumer.RelativePath == "UsesMaterial.acsbp" &&
                  migratedTarget.RelativePath == "Target.acsmat" &&
                  migratedConsumer.Metadata.Dependencies.SequenceEqual(
                      new[] { targetRecord.AssetId },
                      StringComparer.OrdinalIgnoreCase) &&
                  migratedConsumer.Metadata.Source.Replace('\\', '/').Equals(
                      Path.Combine(targetAssets, "UsesMaterial.acsbp").Replace('\\', '/'),
                      StringComparison.OrdinalIgnoreCase) &&
                  migratedConsumerText.Contains(
                      Path.Combine(targetAssets, "Target.acsmat"),
                      StringComparison.OrdinalIgnoreCase) &&
                  !migratedConsumerText.Contains(
                      sourceAssets,
                      StringComparison.OrdinalIgnoreCase) &&
                  migratedGraphText.Contains(
                      Path.Combine(targetAssets, "Target.acsmat")
                          .Replace("\\", "\\\\", StringComparison.Ordinal),
                      StringComparison.OrdinalIgnoreCase);
            if (!migrationVerified)
            {
                output.WriteLine(
                    $"DIAG: result={migrated.AssetCount}/{migrated.FileCount}, " +
                    $"previewFiles={migrationPreview.Files.Count}, " +
                    $"consumerPath={migratedConsumer?.RelativePath ?? "<missing>"}, " +
                    $"targetPath={migratedTarget?.RelativePath ?? "<missing>"}, " +
                    $"deps={string.Join(",", migratedConsumer?.Metadata.Dependencies ?? Array.Empty<string>())}; " +
                    "migrated consumer source=" +
                    (migratedConsumer?.Metadata.Source ?? "<missing>") +
                    "; consumer=" + migratedConsumerText.ReplaceLineEndings("\\n") +
                    "; graph=" + migratedGraphText.ReplaceLineEndings("\\n"));
            }
            Check(migrationVerified,
                "released target lock can be reacquired and Migrate preserves GUID/dependency identity");
            Check(Throws<IOException>(() => workflow.PreviewMigrate(
                    new[] { consumerRecord.AssetId },
                    targetProject)),
                "Migrate refuses every existing destination instead of merging or overwriting");

            string broken = Path.Combine(sourceAssets, "BrokenDependency.acsbp");
            Write(broken, "ACSBP 1\n");
            database.Refresh(verifyContent: true);
            database.TryGetByPath(broken, out AssetRecord? brokenRecord);
            database.UpdateImportMetadata(
                brokenRecord!.AssetId,
                broken,
                "blueprint",
                1,
                new[] { Guid.NewGuid().ToString("N") });
            string emptyTargetProject = Path.Combine(root, "EmptyTarget");
            Directory.CreateDirectory(Path.Combine(emptyTargetProject, "Assets"));
            Check(Throws<InvalidDataException>(() => workflow.PreviewMigrate(
                    new[] { brokenRecord.AssetId },
                    emptyTargetProject)),
                "Migrate fails closed when dependency metadata contains a missing GUID");

            string redirectorRegistry = Path.Combine(
                sourceAssets,
                AssetDatabase.InternalDirectoryName,
                "redirectors.v1.json");
            byte[] redirectorBackup = File.ReadAllBytes(redirectorRegistry);
            File.WriteAllText(redirectorRegistry, "{ invalid", new UTF8Encoding(false));
            Check(Throws<JsonException>(() => workflow.SnapshotRedirectors()),
                "redirector registry corruption fails closed instead of guessing a target");
            File.WriteAllBytes(redirectorRegistry, redirectorBackup);

            string operations = Path.Combine(
                targetAssets,
                AssetDatabase.InternalDirectoryName,
                "operations");
            Check(!Directory.Exists(operations) ||
                  !Directory.EnumerateDirectories(operations).Any(),
                "successful and rejected advanced operations leave no transaction staging");
        }
        catch (Exception error)
        {
            failed++;
            output.WriteLine("FAIL: advanced management unhandled exception: " + error);
        }
        finally
        {
            try
            {
                if (Directory.Exists(root))
                    Directory.Delete(root, recursive: true);
            }
            catch
            {
            }
        }

        output.WriteLine(
            $"Asset advanced management self-test: {passed} PASS / {failed} failures");
        return failed;
    }
}
