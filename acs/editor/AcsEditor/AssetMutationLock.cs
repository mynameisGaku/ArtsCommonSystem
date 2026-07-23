// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Threading;

namespace AcsEditor;

/// <summary>
/// A fail-fast, cross-process lease for mutations below one project's Assets root.
///
/// Callers must acquire their existing process-local operation gate before this lease. The
/// additional per-project monitor serializes lower-level callers in the same process, and
/// same-thread nested acquisitions share one operating-system handle so transactional wrappers
/// can safely call AssetDatabase mutators without dropping the lease. The persistent lock file is
/// intentionally not deleted on release: deleting it after closing the handle could race a new
/// owner and allow a third process to lock a replacement file.
/// </summary>
internal sealed class AssetMutationLock : IDisposable
{
    private const string LockFileName = "asset-mutation.lock";
    private static readonly StringComparison PathComparison =
        StringComparison.OrdinalIgnoreCase;
    private static readonly ConcurrentDictionary<string, object> ProcessGates =
        new(StringComparer.OrdinalIgnoreCase);

    [ThreadStatic]
    private static Dictionary<string, HeldLease>? _threadLeases;

    private readonly string _assetsRoot;
    private HeldLease? _lease;

    private AssetMutationLock(string assetsRoot, HeldLease lease)
    {
        _assetsRoot = assetsRoot;
        _lease = lease;
    }

    internal static AssetMutationLock Acquire(
        string assetsRoot,
        string operation) =>
        AcquireCore(assetsRoot, operation, waitForInProcessOwner: true);

    /// <summary>
    /// UI-thread entry points cannot wait on a worker which may itself be waiting for the
    /// dispatcher. This variant preserves same-thread nesting but fails before mutation when a
    /// different thread or process currently owns the project lease.
    /// </summary>
    internal static AssetMutationLock AcquireFailFast(
        string assetsRoot,
        string operation) =>
        AcquireCore(assetsRoot, operation, waitForInProcessOwner: false);

    private static AssetMutationLock AcquireCore(
        string assetsRoot,
        string operation,
        bool waitForInProcessOwner)
    {
        if (string.IsNullOrWhiteSpace(assetsRoot))
            throw new ArgumentException("Assets root is required.", nameof(assetsRoot));
        if (string.IsNullOrWhiteSpace(operation))
            throw new ArgumentException("Mutation operation is required.", nameof(operation));

        string normalizedAssets = NormalizeDirectory(assetsRoot);
        EnsureOrdinaryDirectory(normalizedAssets, "Assets root");

        object processGate = ProcessGates.GetOrAdd(
            normalizedAssets,
            static _ => new object());
        bool gateEntered;
        if (waitForInProcessOwner)
        {
            Monitor.Enter(processGate);
            gateEntered = true;
        }
        else
        {
            gateEntered = Monitor.TryEnter(processGate);
        }
        if (!gateEntered)
        {
            throw new IOException(
                $"Asset mutation '{operation}' could not start because another editor " +
                $"operation holds the project mutation lock for '{normalizedAssets}'.");
        }
        try
        {
            Dictionary<string, HeldLease> leases =
                _threadLeases ??= new Dictionary<string, HeldLease>(
                    StringComparer.OrdinalIgnoreCase);
            if (leases.TryGetValue(normalizedAssets, out HeldLease? nested))
            {
                nested.Depth++;
                return new AssetMutationLock(normalizedAssets, nested);
            }

            string databaseRoot = NormalizeDirectory(Path.Combine(
                normalizedAssets,
                AssetDatabase.InternalDirectoryName));
            EnsureUnderOrEqual(databaseRoot, normalizedAssets);
            EnsureOrCreateOrdinaryDirectory(databaseRoot, "Asset database directory");

            string lockPath = Path.GetFullPath(Path.Combine(databaseRoot, LockFileName));
            EnsureUnderOrEqual(lockPath, databaseRoot);
            RejectExistingUnsafeLockPath(lockPath);

            FileStream stream;
            try
            {
                // FileShare.None is the cross-process lock. The process-local monitor above
                // serializes independent editor workers while this open remains fail-fast when a
                // different editor owns the project.
                stream = new FileStream(
                    lockPath,
                    FileMode.OpenOrCreate,
                    FileAccess.ReadWrite,
                    FileShare.None,
                    bufferSize: 1,
                    FileOptions.None);
            }
            catch (IOException error)
            {
                throw new IOException(
                    $"Asset mutation '{operation}' could not start because another editor " +
                    $"holds the project mutation lock for '{normalizedAssets}'.",
                    error);
            }

            try
            {
                // Close the create/open race against an attacker replacing either component with
                // a reparse point. Holding the no-share file handle also prevents replacement of
                // the lock file for the lifetime of the lease on Windows.
                EnsureOrdinaryDirectory(normalizedAssets, "Assets root");
                EnsureOrdinaryDirectory(databaseRoot, "Asset database directory");
                EnsureOrdinaryFile(lockPath, "Asset mutation lock");
                var lease = new HeldLease(
                    stream,
                    processGate,
                    Environment.CurrentManagedThreadId);
                leases.Add(normalizedAssets, lease);
                return new AssetMutationLock(normalizedAssets, lease);
            }
            catch
            {
                stream.Dispose();
                throw;
            }
        }
        catch
        {
            Monitor.Exit(processGate);
            throw;
        }
    }

    public void Dispose()
    {
        HeldLease? observed = Volatile.Read(ref _lease);
        if (observed == null)
            return;
        if (observed.OwnerThreadId != Environment.CurrentManagedThreadId)
        {
            throw new InvalidOperationException(
                "Asset mutation leases must be disposed by the acquiring thread.");
        }
        HeldLease? lease = Interlocked.Exchange(ref _lease, null);
        if (lease == null)
            return;

        try
        {
            lease.Depth--;
            if (lease.Depth != 0)
                return;

            Dictionary<string, HeldLease>? leases = _threadLeases;
            if (leases == null ||
                !leases.Remove(_assetsRoot, out HeldLease? removed) ||
                !ReferenceEquals(removed, lease))
            {
                throw new InvalidOperationException(
                    "Asset mutation lease ownership was corrupted.");
            }
            lease.Stream.Dispose();
            if (leases.Count == 0)
                _threadLeases = null;
        }
        finally
        {
            Monitor.Exit(lease.ProcessGate);
        }
    }

    private sealed class HeldLease
    {
        internal HeldLease(
            FileStream stream,
            object processGate,
            int ownerThreadId)
        {
            Stream = stream;
            ProcessGate = processGate;
            OwnerThreadId = ownerThreadId;
        }

        internal FileStream Stream { get; }
        internal object ProcessGate { get; }
        internal int OwnerThreadId { get; }
        internal int Depth { get; set; } = 1;
    }

    private static void RejectExistingUnsafeLockPath(string path)
    {
        try
        {
            FileAttributes attributes = File.GetAttributes(path);
            if ((attributes & FileAttributes.Directory) != 0 ||
                (attributes & FileAttributes.ReparsePoint) != 0)
            {
                throw new InvalidDataException(
                    "Asset mutation lock must be an ordinary file.");
            }
        }
        catch (FileNotFoundException)
        {
        }
        catch (DirectoryNotFoundException)
        {
        }
    }

    private static void EnsureOrCreateOrdinaryDirectory(
        string path,
        string label)
    {
        if (!Directory.Exists(path))
            Directory.CreateDirectory(path);
        EnsureOrdinaryDirectory(path, label);
    }

    private static void EnsureOrdinaryDirectory(string path, string label)
    {
        FileAttributes attributes = File.GetAttributes(path);
        if ((attributes & FileAttributes.Directory) == 0 ||
            (attributes & FileAttributes.ReparsePoint) != 0)
        {
            throw new InvalidDataException($"{label} must be an ordinary directory.");
        }
    }

    private static void EnsureOrdinaryFile(string path, string label)
    {
        FileAttributes attributes = File.GetAttributes(path);
        if ((attributes & FileAttributes.Directory) != 0 ||
            (attributes & FileAttributes.ReparsePoint) != 0)
        {
            throw new InvalidDataException($"{label} must be an ordinary file.");
        }
    }

    private static void EnsureUnderOrEqual(string path, string root)
    {
        string normalizedPath = NormalizeDirectory(path);
        string normalizedRoot = NormalizeDirectory(root);
        if (string.Equals(normalizedPath, normalizedRoot, PathComparison))
            return;
        string prefix = normalizedRoot + Path.DirectorySeparatorChar;
        if (!normalizedPath.StartsWith(prefix, PathComparison))
            throw new InvalidDataException("Asset mutation lock path escapes its project.");
    }

    private static string NormalizeDirectory(string path)
    {
        string full = Path.GetFullPath(path);
        string root = Path.GetPathRoot(full) ?? "";
        return full.Length > root.Length
            ? Path.TrimEndingDirectorySeparator(full)
            : full;
    }
}
