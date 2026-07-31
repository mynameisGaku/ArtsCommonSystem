// SPDX-License-Identifier: Apache-2.0

using System;
using System.Buffers;
using System.Collections.Generic;
using System.IO;
using System.Runtime.CompilerServices;

namespace AcsEditor;

/// <summary>
/// DDC path pool の一貫した累積診断値。
/// </summary>
public readonly record struct DerivedDataCachePathPoolDiagnostics(
    long RequestCount,
    long HitCount,
    long MissCount,
    long EvictionCount,
    long BypassCount,
    int RetainedPathCount,
    long RetainedCodeUnits);

/// <summary>
/// 同じ key から再利用する不変の DDC directory と entry path。
/// </summary>
internal sealed record DerivedDataCachePath(
    string Directory,
    string EntryPath);

/// <summary>
/// DDC owner の寿命内だけ canonical key path を有界に共有する。
/// 呼び出し側は owner 固有 gate で操作と診断取得を直列化する。
/// </summary>
internal sealed class DerivedDataCachePathPool
{
    internal const int DefaultMaximumEntries = 512;
    internal const long DefaultMaximumCodeUnits = 256L * 1024L;

    private static readonly SearchValues<char> CanonicalKeyCharacters =
        SearchValues.Create("0123456789abcdef");
    private readonly string _cacheRoot;
    private readonly string _entryExtension;
    private readonly int _maximumEntries;
    private readonly long _maximumCodeUnits;
    private readonly Dictionary<string, Entry> _entries =
        new(StringComparer.Ordinal);
    private readonly LinkedList<string> _recency = new();
    private long _retainedCodeUnits;
    private long _requestCount;
    private long _hitCount;
    private long _missCount;
    private long _evictionCount;
    private long _bypassCount;
    private string? _lastKey;
    private Entry? _lastEntry;

    /// <summary>
    /// DDC owner 固有の root、拡張子、保持上限を設定する。
    /// </summary>
    internal DerivedDataCachePathPool(
        string cacheRoot,
        string entryExtension,
        int maximumEntries = DefaultMaximumEntries,
        long maximumCodeUnits = DefaultMaximumCodeUnits)
    {
        if (string.IsNullOrWhiteSpace(cacheRoot))
            throw new ArgumentException(
                "DDC cache root is required.",
                nameof(cacheRoot));
        if (string.IsNullOrEmpty(entryExtension) ||
            entryExtension[0] != '.' ||
            entryExtension.IndexOfAny(
                [Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar]) >= 0)
        {
            throw new ArgumentException(
                "DDC entry extension is invalid.",
                nameof(entryExtension));
        }
        if (maximumEntries <= 0)
            throw new ArgumentOutOfRangeException(nameof(maximumEntries));
        if (maximumCodeUnits <= 0)
            throw new ArgumentOutOfRangeException(nameof(maximumCodeUnits));

        _cacheRoot = Path.TrimEndingDirectorySeparator(
            Path.GetFullPath(cacheRoot));
        _entryExtension = entryExtension;
        _maximumEntries = maximumEntries;
        _maximumCodeUnits = maximumCodeUnits;
    }

    /// <summary>
    /// canonical SHA-256 key の directory と entry path を共有する。
    /// </summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal DerivedDataCachePath Intern(string key)
    {
        ArgumentNullException.ThrowIfNull(key);
        if (_lastEntry != null &&
            (ReferenceEquals(_lastKey, key) ||
             string.Equals(_lastKey, key, StringComparison.Ordinal)))
        {
            _requestCount++;
            _hitCount++;
            return _lastEntry.Path;
        }
        return InternSlow(key);
    }

    /// <summary>
    /// 直前 key 以外を検索し、必要なら path を構築して保持する。
    /// </summary>
    [MethodImpl(MethodImplOptions.NoInlining)]
    private DerivedDataCachePath InternSlow(string key)
    {
        if (_entries.TryGetValue(key, out Entry? existing))
        {
            _requestCount++;
            _hitCount++;
            Touch(existing);
            _lastKey = existing.Node.Value;
            _lastEntry = existing;
            return existing.Path;
        }

        ValidateKey(key);
        _requestCount++;
        _missCount++;
        string directory = Path.Combine(_cacheRoot, key[..2]);
        string entryPath = Path.Combine(
            directory,
            key + _entryExtension);
        var path = new DerivedDataCachePath(directory, entryPath);
        long codeUnits = checked(
            key.Length + 1L +
            directory.Length + 1L +
            entryPath.Length + 1L);
        if (codeUnits > _maximumCodeUnits)
        {
            _bypassCount++;
            _lastKey = null;
            _lastEntry = null;
            return path;
        }

        EvictUntilFit(codeUnits);
        LinkedListNode<string> node = _recency.AddFirst(key);
        var entry = new Entry(path, codeUnits, node);
        _entries.Add(key, entry);
        _retainedCodeUnits += codeUnits;
        _lastKey = key;
        _lastEntry = entry;
        return path;
    }

    /// <summary>
    /// path pool の累積値と現在保持量を一括取得する。
    /// </summary>
    internal DerivedDataCachePathPoolDiagnostics CaptureDiagnostics()
    {
        return new(
            _requestCount,
            _hitCount,
            _missCount,
            _evictionCount,
            _bypassCount,
            _entries.Count,
            _retainedCodeUnits);
    }

    /// <summary>
    /// pool の保持参照と累積診断値を解放する。
    /// </summary>
    internal void Reset()
    {
        _entries.Clear();
        _recency.Clear();
        _retainedCodeUnits = 0;
        _requestCount = 0;
        _hitCount = 0;
        _missCount = 0;
        _evictionCount = 0;
        _bypassCount = 0;
        _lastKey = null;
        _lastEntry = null;
    }

    /// <summary>
    /// 新規 path が上限内に収まるまで古い保持参照を外す。
    /// </summary>
    private void EvictUntilFit(long incomingCodeUnits)
    {
        while (_entries.Count >= _maximumEntries ||
               _retainedCodeUnits >
                   _maximumCodeUnits - incomingCodeUnits)
        {
            LinkedListNode<string>? oldest = _recency.Last;
            if (oldest == null) break;
            Remove(oldest.Value);
            _evictionCount++;
        }
    }

    /// <summary>
    /// 使用した entry を次の eviction から最も遠い位置へ移す。
    /// </summary>
    private void Touch(Entry entry)
    {
        _recency.Remove(entry.Node);
        _recency.AddFirst(entry.Node);
    }

    /// <summary>
    /// 指定 key の保持参照と使用量を取り除く。
    /// </summary>
    private void Remove(string key)
    {
        if (!_entries.Remove(key, out Entry? entry)) return;
        _recency.Remove(entry.Node);
        _retainedCodeUnits -= entry.CodeUnits;
        if (ReferenceEquals(_lastEntry, entry))
        {
            _lastKey = null;
            _lastEntry = null;
        }
    }

    /// <summary>
    /// path pool に渡す key が小文字 SHA-256 正規形か検証する。
    /// </summary>
    private static void ValidateKey(string key)
    {
        ArgumentNullException.ThrowIfNull(key);
        if (key.Length != 64 ||
            key.AsSpan().IndexOfAnyExcept(
                CanonicalKeyCharacters) >= 0)
        {
            throw new InvalidDataException(
                "DDC path key is not canonical lowercase SHA-256.");
        }
    }

    /// <summary>
    /// 一つの共有 path と LRU 位置を保持する。
    /// </summary>
    private sealed record Entry(
        DerivedDataCachePath Path,
        long CodeUnits,
        LinkedListNode<string> Node);
}
