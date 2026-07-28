// SPDX-License-Identifier: Apache-2.0
// Staged Project Settings workflow for optional package product metadata.

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.Json;

namespace AcsEditor.Packaging;

internal sealed record PackageMetadataApplyResult(
    bool Succeeded,
    string ErrorMessage)
{
    internal static PackageMetadataApplyResult Success { get; } =
        new(true, "");
}

/// <summary>
/// Keeps Package Metadata edits staged until Apply. Validation and persistence
/// intentionally delegate to <see cref="PackageProductMetadataContract"/>, the
/// same contract used by package preflight.
/// </summary>
internal sealed class PackageMetadataEditorSession
{
    private readonly string _projectRoot;
    private readonly string _configDirectory;
    private PackageProductMetadata _baseline;
    private PackageProductMetadataSourceFingerprint _sourceFingerprint;

    private PackageMetadataEditorSession(
        string projectRoot,
        string configDirectory,
        PackageProductMetadataSnapshot snapshot)
    {
        _projectRoot = projectRoot;
        _configDirectory = configDirectory;
        _baseline = snapshot.Metadata;
        _sourceFingerprint = snapshot.Fingerprint;
        Draft = snapshot.Metadata;
    }

    internal PackageProductMetadata Draft { get; private set; }

    internal bool SourceFileExists => _sourceFingerprint.Exists;

    internal bool IsConfigured => !Draft.IsEmpty;

    internal bool IsDirty => Draft != _baseline;

    internal IReadOnlyList<PackageProductMetadataValidationIssue>
        ValidationIssues =>
            PackageProductMetadataContract.GetValidationIssues(Draft);

    internal static PackageMetadataEditorSession Load(string projectRoot)
    {
        string root = ValidateProjectStorage(projectRoot);
        string configDirectory = Path.Combine(root, "Config");
        PackageProductMetadataSnapshot snapshot =
            PackageProductMetadataContract.LoadOptionalSnapshot(
                configDirectory);
        return new(root, configDirectory, snapshot);
    }

    internal void UpdateDraft(PackageProductMetadata draft)
    {
        ArgumentNullException.ThrowIfNull(draft);
        Draft = draft;
    }

    internal void Revert() => Draft = _baseline;

    internal PackageMetadataApplyResult Apply()
    {
        try
        {
            PackageProductMetadataContract.Validate(Draft);
            _ = ValidateProjectStorage(_projectRoot);
            _sourceFingerprint =
                PackageProductMetadataContract.SaveOptionalAtomicExpected(
                _configDirectory,
                Draft,
                _sourceFingerprint);
            _baseline = Draft;
            return PackageMetadataApplyResult.Success;
        }
        catch (Exception error) when (
            error is IOException or UnauthorizedAccessException or
                InvalidDataException or ArgumentException or
                NotSupportedException or JsonException or
                DecoderFallbackException)
        {
            return new(false, error.Message);
        }
    }

    private static string ValidateProjectStorage(string projectRoot)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(projectRoot);
        string root =
            Path.TrimEndingDirectorySeparator(
                Path.GetFullPath(projectRoot));
        SceneSourceFile.ValidateProjectRootDirectory(root);
        string config = Path.Combine(root, "Config");
        if (File.Exists(config))
        {
            throw new InvalidDataException(
                "The project Config path is a file, not a directory.");
        }
        if (!Directory.Exists(config))
        {
            throw new DirectoryNotFoundException(
                $"Config directory does not exist: {config}");
        }
        FileAttributes attributes = File.GetAttributes(config);
        if ((attributes &
             (FileAttributes.Directory | FileAttributes.ReparsePoint)) !=
            FileAttributes.Directory)
        {
            throw new InvalidDataException(
                "The project Config directory must be an ordinary directory under the project root.");
        }
        return root;
    }
}
