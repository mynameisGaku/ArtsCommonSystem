// SPDX-License-Identifier: Apache-2.0

using AcsEditor.Packaging;
using System;
using System.IO;
using System.Linq;
using System.Text;

namespace AcsEditor;

internal static class PackageMetadataEditorSelfTest
{
    internal static int Run(TextWriter log)
    {
        int passed = 0;
        int failed = 0;
        string root = Path.Combine(
            Path.GetTempPath(),
            "acs-package-metadata-editor-" + Guid.NewGuid().ToString("N"));
        string config = Path.Combine(root, "Config");

        void Check(bool condition, string name)
        {
            if (condition)
            {
                passed++;
                log.WriteLine($"PASS  {name}");
            }
            else
            {
                failed++;
                log.WriteLine($"FAIL  {name}");
            }
        }

        try
        {
            Directory.CreateDirectory(config);
            var valid = new PackageProductMetadata(
                1,
                "ACS Studio",
                "A deterministic package.",
                "Copyright ACS Studio",
                "https://example.invalid/support");

            Check(
                ProjectSettingsWindow.AggregateProjectSettingsCount(
                    0,
                    packageMetadataMatches: true) == 4 &&
                ProjectSettingsWindow.AggregateProjectSettingsCount(
                    7,
                    packageMetadataMatches: false) == 7,
                "All settings count includes the four visible Distribution fields");

            byte[] first = PackageProductMetadataContract.SerializeCanonical(valid);
            byte[] second = PackageProductMetadataContract.SerializeCanonical(valid);
            string canonical = Encoding.UTF8.GetString(first);
            bool canonicalSerialization =
                first.SequenceEqual(second) &&
                (first.Length < 3 ||
                 first[0] != 0xEF ||
                 first[1] != 0xBB ||
                 first[2] != 0xBF) &&
                canonical.StartsWith(
                    "{\n  \"schemaVersion\": 1,\n  \"publisher\": \"ACS Studio\",",
                    StringComparison.Ordinal) &&
                canonical.EndsWith("}\n", StringComparison.Ordinal);
            if (!canonicalSerialization)
                log.WriteLine("INFO  canonical payload: " + canonical.Replace("\n", "\\n"));
            Check(
                canonicalSerialization,
                "metadata serialization is deterministic canonical UTF-8 without BOM");

            var invalid = new PackageProductMetadata(
                1,
                " padded ",
                "bad\0description",
                new string('c', PackageProductMetadataContract.MaximumCopyrightLength + 1),
                "http://example.invalid/support");
            var invalidFields = PackageProductMetadataContract
                .GetValidationIssues(invalid)
                .Select(issue => issue.Field)
                .ToHashSet(StringComparer.Ordinal);
            Check(
                invalidFields.SetEquals(
                    ["publisher", "description", "copyright", "supportUrl"]),
                "inline issues are produced by the shared package contract for every field");

            PackageProductMetadataContract.SaveOptionalAtomic(config, valid);
            string metadataPath = Path.Combine(
                config,
                PackageProductMetadataContract.FileName);
            byte[] durableBeforeFailure = File.ReadAllBytes(metadataPath);
            PackageProductMetadataSourceFingerprint durableFingerprint =
                PackageProductMetadataContract
                    .LoadOptionalSnapshot(config)
                    .Fingerprint;
            bool publishFailed = false;
            try
            {
                PackageProductMetadataContract.SaveOptionalAtomicExpectedForTest(
                    config,
                    valid with { Publisher = "Replacement Studio" },
                    durableFingerprint,
                    new(
                        BeforePublish: static _ =>
                            throw new IOException(
                                "injected publish failure")));
            }
            catch (IOException)
            {
                publishFailed = true;
            }
            Check(
                publishFailed &&
                File.ReadAllBytes(metadataPath).SequenceEqual(durableBeforeFailure) &&
                !Directory.EnumerateFiles(
                        config,
                        PackageProductMetadataContract.FileName + ".tmp-*")
                    .Any(),
                "failed atomic publication preserves the old file and cleans private staging");

            void WriteCanonical(PackageProductMetadata metadata) =>
                File.WriteAllBytes(
                    metadataPath,
                    PackageProductMetadataContract.SerializeCanonical(metadata));

            string[] RecoveryFiles() =>
                Directory.EnumerateFiles(
                        config,
                        PackageProductMetadataContract.FileName +
                        ".tmp-recovery-*",
                        SearchOption.TopDirectoryOnly)
                    .ToArray();

            void DeleteRecoveryFiles()
            {
                foreach (string recovery in RecoveryFiles())
                    File.Delete(recovery);
            }

            var raceLocal = valid with
            {
                Publisher = "Local Race Publisher",
            };
            var raceExternal = valid with
            {
                Publisher = "External Race Publisher",
            };
            PackageProductMetadataSourceFingerprint replaceRaceExpected =
                PackageProductMetadataContract
                    .LoadOptionalSnapshot(config)
                    .Fingerprint;
            bool replaceRaceRejected = false;
            try
            {
                PackageProductMetadataContract.SaveOptionalAtomicExpectedForTest(
                    config,
                    raceLocal,
                    replaceRaceExpected,
                    new(
                        BeforePublish: _ =>
                            WriteCanonical(raceExternal)));
            }
            catch (IOException)
            {
                replaceRaceRejected = true;
            }
            Check(
                replaceRaceRejected &&
                PackageProductMetadataContract.LoadOptional(config) ==
                    raceExternal &&
                RecoveryFiles().Length == 0,
                "replace race restores the actual external bytes captured by File.Replace");

            PackageProductMetadataContract.SaveOptionalAtomic(config, valid);
            var overlappingExternal = valid with
            {
                Publisher = "Overlapping External Publisher",
            };
            PackageProductMetadataSourceFingerprint overlapExpected =
                PackageProductMetadataContract
                    .LoadOptionalSnapshot(config)
                    .Fingerprint;
            bool overlappingReplaceRejected = false;
            try
            {
                PackageProductMetadataContract.SaveOptionalAtomicExpectedForTest(
                    config,
                    raceLocal,
                    overlapExpected,
                    new(
                        BeforePublish: _ =>
                            WriteCanonical(raceExternal),
                        AfterReplace: (_, _) =>
                            WriteCanonical(overlappingExternal)));
            }
            catch (IOException)
            {
                overlappingReplaceRejected = true;
            }
            string[] replaceRecoveries = RecoveryFiles();
            Check(
                overlappingReplaceRejected &&
                PackageProductMetadataContract.LoadOptional(config) ==
                    overlappingExternal &&
                replaceRecoveries.Length == 1 &&
                File.ReadAllBytes(replaceRecoveries[0]).SequenceEqual(
                    PackageProductMetadataContract.SerializeCanonical(
                        raceExternal)),
                "overlapping replace keeps captured external bytes in recovery");
            DeleteRecoveryFiles();

            PackageProductMetadataContract.SaveOptionalAtomic(config, valid);
            PackageProductMetadataSourceFingerprint deleteRaceExpected =
                PackageProductMetadataContract
                    .LoadOptionalSnapshot(config)
                    .Fingerprint;
            bool deleteRaceRejected = false;
            try
            {
                PackageProductMetadataContract.SaveOptionalAtomicExpectedForTest(
                    config,
                    PackageProductMetadata.Empty,
                    deleteRaceExpected,
                    new(
                        BeforeDeleteMove: _ =>
                            WriteCanonical(raceExternal)));
            }
            catch (IOException)
            {
                deleteRaceRejected = true;
            }
            Check(
                deleteRaceRejected &&
                PackageProductMetadataContract.LoadOptional(config) ==
                    raceExternal &&
                RecoveryFiles().Length == 0,
                "delete race restores mismatched quarantined bytes instead of deleting them");

            PackageProductMetadataContract.SaveOptionalAtomic(config, valid);
            PackageProductMetadataSourceFingerprint deleteOverlapExpected =
                PackageProductMetadataContract
                    .LoadOptionalSnapshot(config)
                    .Fingerprint;
            bool overlappingDeleteRejected = false;
            try
            {
                PackageProductMetadataContract.SaveOptionalAtomicExpectedForTest(
                    config,
                    PackageProductMetadata.Empty,
                    deleteOverlapExpected,
                    new(
                        BeforeDeleteMove: _ =>
                            WriteCanonical(raceExternal),
                        AfterDeleteMove: (_, _) =>
                            WriteCanonical(overlappingExternal)));
            }
            catch (IOException)
            {
                overlappingDeleteRejected = true;
            }
            string[] deleteRecoveries = RecoveryFiles();
            Check(
                overlappingDeleteRejected &&
                PackageProductMetadataContract.LoadOptional(config) ==
                    overlappingExternal &&
                deleteRecoveries.Length == 1 &&
                File.ReadAllBytes(deleteRecoveries[0]).SequenceEqual(
                    PackageProductMetadataContract.SerializeCanonical(
                        raceExternal)),
                "overlapping delete keeps quarantined external bytes as recovery");
            DeleteRecoveryFiles();
            PackageProductMetadataContract.SaveOptionalAtomic(config, valid);

            PackageMetadataEditorSession loaded =
                PackageMetadataEditorSession.Load(root);
            Check(
                loaded.IsConfigured &&
                loaded.SourceFileExists &&
                !loaded.IsDirty &&
                loaded.Draft == valid,
                "editor session strictly loads the existing package metadata");

            loaded.UpdateDraft(valid with
            {
                Publisher = "Replacement Studio",
                SupportUrl = "https://example.invalid/new-support",
            });
            Check(
                loaded.IsDirty && loaded.ValidationIssues.Count == 0,
                "editing creates a valid staged draft without writing immediately");
            loaded.Revert();
            Check(
                !loaded.IsDirty && loaded.Draft == valid,
                "Revert restores the last durable metadata");

            PackageProductMetadata replacement = valid with
            {
                Publisher = "Replacement Studio",
            };
            loaded.UpdateDraft(replacement);
            PackageMetadataApplyResult applied = loaded.Apply();
            Check(
                applied.Succeeded &&
                !loaded.IsDirty &&
                PackageProductMetadataContract.LoadOptional(config) == replacement,
                "Apply atomically persists the staged metadata through the package contract");

            PackageProductMetadata localEdit = replacement with
            {
                Description = "Local staged description.",
            };
            PackageProductMetadata externalEdit = replacement with
            {
                Copyright = "Copyright externally changed",
            };
            loaded.UpdateDraft(localEdit);
            PackageProductMetadataContract.SaveOptionalAtomic(
                config,
                externalEdit);
            PackageMetadataApplyResult conflict = loaded.Apply();
            Check(
                !conflict.Succeeded &&
                conflict.ErrorMessage.Contains(
                    "changed outside this Editor",
                    StringComparison.Ordinal) &&
                loaded.IsDirty &&
                PackageProductMetadataContract.LoadOptional(config) ==
                    externalEdit,
                "Apply rejects an external valid edit by raw source fingerprint");

            loaded = PackageMetadataEditorSession.Load(root);
            loaded.UpdateDraft(PackageProductMetadata.Empty);
            PackageProductMetadata externalBeforeDelete = externalEdit with
            {
                Publisher = "External Delete Guard",
            };
            PackageProductMetadataContract.SaveOptionalAtomic(
                config,
                externalBeforeDelete);
            PackageMetadataApplyResult deleteConflict = loaded.Apply();
            Check(
                !deleteConflict.Succeeded &&
                loaded.IsDirty &&
                PackageProductMetadataContract.LoadOptional(config) ==
                    externalBeforeDelete,
                "empty Apply cannot delete an externally changed valid document");

            loaded = PackageMetadataEditorSession.Load(root);
            loaded.UpdateDraft(PackageProductMetadata.Empty);
            PackageMetadataApplyResult cleared = loaded.Apply();
            Check(
                cleared.Succeeded &&
                !loaded.SourceFileExists &&
                !loaded.IsConfigured &&
                !File.Exists(metadataPath),
                "applying an empty draft removes the optional metadata file");

            PackageMetadataEditorSession missing =
                PackageMetadataEditorSession.Load(root);
            Check(
                !missing.SourceFileExists &&
                !missing.IsConfigured &&
                !missing.IsDirty,
                "a missing file is represented explicitly as not configured");

            File.WriteAllText(
                metadataPath,
                """
                {
                  "schemaVersion": 1,
                  "publisher": "ACS Studio",
                  "unknown": "must fail"
                }
                """,
                new UTF8Encoding(false));
            byte[] malformedBeforeLoad = File.ReadAllBytes(metadataPath);
            bool strictLoadFailed = false;
            try
            {
                _ = PackageMetadataEditorSession.Load(root);
            }
            catch (InvalidDataException)
            {
                strictLoadFailed = true;
            }
            Check(
                strictLoadFailed &&
                File.ReadAllBytes(metadataPath).SequenceEqual(malformedBeforeLoad),
                "strict load rejects unknown input without silently repairing it");

            bool malformedClearRejected = false;
            bool malformedReplaceRejected = false;
            try
            {
                PackageProductMetadataContract.SaveOptionalAtomic(
                    config,
                    PackageProductMetadata.Empty);
            }
            catch (InvalidDataException)
            {
                malformedClearRejected = true;
            }
            try
            {
                PackageProductMetadataContract.SaveOptionalAtomic(
                    config,
                    valid);
            }
            catch (InvalidDataException)
            {
                malformedReplaceRejected = true;
            }
            Check(
                malformedClearRejected &&
                malformedReplaceRejected &&
                File.ReadAllBytes(metadataPath).SequenceEqual(malformedBeforeLoad),
                "empty or non-empty Apply cannot delete or overwrite malformed existing metadata");

            File.Delete(metadataPath);
            string linkedTarget = Path.Combine(root, "linked-target.json");
            File.WriteAllText(linkedTarget, "external", new UTF8Encoding(false));
            bool symlinkAvailable = false;
            try
            {
                File.CreateSymbolicLink(metadataPath, linkedTarget);
                symlinkAvailable = true;
            }
            catch (Exception error) when (
                error is IOException or UnauthorizedAccessException or
                    PlatformNotSupportedException)
            {
                log.WriteLine(
                    "INFO  symlink metadata write check unavailable: " +
                    error.Message);
            }
            if (symlinkAvailable)
            {
                bool linkedWriteRejected = false;
                try
                {
                    PackageProductMetadataContract.SaveOptionalAtomic(
                        config,
                        valid);
                }
                catch (InvalidDataException)
                {
                    linkedWriteRejected = true;
                }
                Check(
                    linkedWriteRejected &&
                    File.ReadAllText(linkedTarget) == "external",
                    "metadata Apply rejects a reparse-point target without changing its destination");
                File.Delete(metadataPath);
            }

            File.WriteAllText(
                metadataPath,
                """
                {
                  "schemaVersion": 1,
                  "publisher": "",
                  "description": "",
                  "copyright": "",
                  "supportUrl": ""
                }
                """,
                new UTF8Encoding(false));
            PackageMetadataEditorSession existingEmpty =
                PackageMetadataEditorSession.Load(root);
            Check(
                existingEmpty.SourceFileExists &&
                !existingEmpty.IsConfigured &&
                !existingEmpty.IsDirty,
                "an existing empty document is reported as unconfigured without mutation");

            File.Delete(metadataPath);
            Directory.Delete(config);
            string linkedConfigTarget = Path.Combine(
                Path.GetTempPath(),
                "acs-package-metadata-linked-config-" +
                Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(linkedConfigTarget);
            bool directorySymlinkAvailable = false;
            try
            {
                Directory.CreateSymbolicLink(config, linkedConfigTarget);
                directorySymlinkAvailable = true;
            }
            catch (Exception error) when (
                error is IOException or UnauthorizedAccessException or
                    PlatformNotSupportedException)
            {
                log.WriteLine(
                    "INFO  symlink Config write check unavailable: " +
                    error.Message);
            }
            if (directorySymlinkAvailable)
            {
                bool sessionRejected = false;
                bool contractRejected = false;
                try
                {
                    _ = PackageMetadataEditorSession.Load(root);
                }
                catch (InvalidDataException)
                {
                    sessionRejected = true;
                }
                try
                {
                    PackageProductMetadataContract.SaveOptionalAtomic(
                        config,
                        valid);
                }
                catch (InvalidDataException)
                {
                    contractRejected = true;
                }
                Check(
                    sessionRejected &&
                    contractRejected &&
                    !File.Exists(Path.Combine(
                        linkedConfigTarget,
                        PackageProductMetadataContract.FileName)),
                    "Project Settings and package persistence reject a reparse-point Config directory");
                Directory.Delete(config);
            }
            Directory.Delete(linkedConfigTarget, recursive: true);
            Directory.CreateDirectory(config);
        }
        catch (Exception error)
        {
            failed++;
            log.WriteLine("FAIL  package metadata editor self-test threw: " + error);
        }
        finally
        {
            try
            {
                Directory.Delete(root, recursive: true);
            }
            catch
            {
            }
        }

        log.WriteLine(
            $"Package metadata editor self-test: {passed} passed, {failed} failed");
        return failed;
    }
}
