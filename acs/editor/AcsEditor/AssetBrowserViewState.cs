// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace AcsEditor;

internal enum AssetBrowserSearchScope
{
    CurrentFolder = 0,
    Subfolders = 1,
    EntireProject = 2,
}

/// <summary>
/// Pure, fail-closed path policy for Content Browser search scopes. A reusable Filter
/// normalizes the project roots once so large Asset Database snapshots do not repeat
/// root work for every indexed record.
/// </summary>
internal static class AssetBrowserSearchScopePolicy
{
    internal readonly struct Filter
    {
        private readonly string _assetsRoot;
        private readonly string _currentDirectory;

        internal Filter(
            string assetsRoot,
            string currentDirectory,
            AssetBrowserSearchScope scope)
        {
            _assetsRoot = assetsRoot;
            _currentDirectory = currentDirectory;
            Scope = scope;
            IsValid = true;
        }

        internal AssetBrowserSearchScope Scope { get; }
        internal bool IsValid { get; }

        internal bool Includes(string assetPath)
        {
            if (!IsValid ||
                !TryNormalize(assetPath, out string asset) ||
                !IsUnder(asset, _assetsRoot))
            {
                return false;
            }

            return Scope switch
            {
                AssetBrowserSearchScope.CurrentFolder =>
                    Path.GetDirectoryName(asset) is string parent &&
                    PathEquals(parent, _currentDirectory),
                AssetBrowserSearchScope.Subfolders =>
                    IsUnder(asset, _currentDirectory),
                AssetBrowserSearchScope.EntireProject => true,
                _ => false,
            };
        }
    }

    internal static AssetBrowserSearchScope ParseTag(string? tag) =>
        tag?.Trim().ToLowerInvariant() switch
        {
            "subfolders" => AssetBrowserSearchScope.Subfolders,
            "entireproject" => AssetBrowserSearchScope.EntireProject,
            _ => AssetBrowserSearchScope.CurrentFolder,
        };

    internal static Filter CreateFilter(
        string assetsRoot,
        string currentDirectory,
        AssetBrowserSearchScope scope)
    {
        if (!Enum.IsDefined(scope) ||
            !TryNormalize(assetsRoot, out string root) ||
            !TryNormalize(currentDirectory, out string current) ||
            !IsUnderOrEqual(current, root))
        {
            return default;
        }
        return new Filter(root, current, scope);
    }

    internal static bool IncludesAsset(
        string assetsRoot,
        string currentDirectory,
        string assetPath,
        AssetBrowserSearchScope scope) =>
        CreateFilter(assetsRoot, currentDirectory, scope)
            .Includes(assetPath);

    internal static string StatusLabel(AssetBrowserSearchScope scope) =>
        scope switch
        {
            AssetBrowserSearchScope.CurrentFolder => "current folder",
            AssetBrowserSearchScope.Subfolders => "subfolders",
            AssetBrowserSearchScope.EntireProject => "all assets",
            _ => "invalid scope",
        };

    private static bool TryNormalize(string? path, out string normalized)
    {
        normalized = "";
        if (string.IsNullOrWhiteSpace(path) ||
            !Path.IsPathFullyQualified(path))
        {
            return false;
        }
        try
        {
            normalized = Path.TrimEndingDirectorySeparator(
                Path.GetFullPath(path));
            return normalized.Length != 0;
        }
        catch (Exception error) when (
            error is ArgumentException or NotSupportedException or
                PathTooLongException)
        {
            normalized = "";
            return false;
        }
    }

    private static bool IsUnder(string candidate, string root)
    {
        try
        {
            string relative = Path.GetRelativePath(root, candidate);
            return relative != "." &&
                   !Path.IsPathRooted(relative) &&
                   relative != ".." &&
                   !relative.StartsWith(
                       ".." + Path.DirectorySeparatorChar,
                       StringComparison.Ordinal) &&
                   !relative.StartsWith(
                       ".." + Path.AltDirectorySeparatorChar,
                       StringComparison.Ordinal);
        }
        catch (Exception error) when (
            error is ArgumentException or NotSupportedException or IOException)
        {
            return false;
        }
    }

    private static bool IsUnderOrEqual(string candidate, string root) =>
        PathEquals(candidate, root) || IsUnder(candidate, root);

    private static bool PathEquals(string left, string right) =>
        string.Equals(
            Path.TrimEndingDirectorySeparator(left),
            Path.TrimEndingDirectorySeparator(right),
            StringComparison.OrdinalIgnoreCase);
}

/// <summary>
/// Pure search/filter evaluator for the Asset View. It intentionally accepts only a small,
/// deterministic subset of Content Browser syntax: bare terms plus name:, path:, type:/kind:,
/// and id:, with an optional leading '-' for exclusion. Quoted values keep spaces together.
/// </summary>
internal static class AssetBrowserQuery
{
    private const int MaxQueryLength = 1024;
    private const int MaxTerms = 32;

    internal static bool Matches(
        string? query,
        string? selectedKind,
        string name,
        string relativePath,
        string kind,
        string assetId,
        bool isDirectory)
    {
        string effectiveKind = isDirectory ? "folder" : kind;
        string kindFilter = selectedKind?.Trim() ?? "";
        if (kindFilter.Length != 0 &&
            !kindFilter.Equals("all", StringComparison.OrdinalIgnoreCase) &&
            !effectiveKind.Equals(kindFilter, StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        IReadOnlyList<string> terms = Tokenize(query);
        foreach (string rawTerm in terms)
        {
            bool exclude = rawTerm.Length > 1 && rawTerm[0] == '-';
            string term = exclude ? rawTerm[1..] : rawTerm;
            bool matched = MatchesTerm(
                term,
                name,
                relativePath,
                effectiveKind,
                assetId);
            if (exclude ? matched : !matched)
                return false;
        }
        return true;
    }

    private static bool MatchesTerm(
        string term,
        string name,
        string relativePath,
        string kind,
        string assetId)
    {
        int separator = term.IndexOf(':');
        if (separator > 0)
        {
            string field = term[..separator];
            string value = term[(separator + 1)..];
            if (value.Length == 0) return false;
            if (field.Equals("name", StringComparison.OrdinalIgnoreCase))
                return Contains(name, value);
            if (field.Equals("path", StringComparison.OrdinalIgnoreCase))
                return Contains(relativePath, value);
            if (field.Equals("type", StringComparison.OrdinalIgnoreCase) ||
                field.Equals("kind", StringComparison.OrdinalIgnoreCase))
            {
                return Contains(kind, value);
            }
            if (field.Equals("id", StringComparison.OrdinalIgnoreCase))
                return Contains(assetId, value);
        }

        return Contains(name, term) ||
               Contains(relativePath, term) ||
               Contains(kind, term);
    }

    private static bool Contains(string value, string term) =>
        value.Contains(term, StringComparison.OrdinalIgnoreCase);

    private static IReadOnlyList<string> Tokenize(string? query)
    {
        string text = (query ?? "").Trim();
        if (text.Length == 0) return Array.Empty<string>();
        if (text.Length > MaxQueryLength) text = text[..MaxQueryLength];

        var terms = new List<string>();
        var current = new StringBuilder();
        bool quoted = false;
        for (int index = 0; index < text.Length && terms.Count < MaxTerms; index++)
        {
            char value = text[index];
            if (value == '"')
            {
                quoted = !quoted;
                continue;
            }
            if (!quoted && char.IsWhiteSpace(value))
            {
                AddTerm(terms, current);
                continue;
            }
            current.Append(value);
        }
        if (terms.Count < MaxTerms) AddTerm(terms, current);
        return terms;
    }

    private static void AddTerm(List<string> terms, StringBuilder current)
    {
        if (current.Length == 0) return;
        string value = current.ToString();
        current.Clear();
        if (value is not "-" && value.Length != 0)
            terms.Add(value);
    }
}

/// <summary>Bounded browser-style Back/Forward history used by the Asset View.</summary>
internal sealed class AssetBrowserHistory
{
    private const int Capacity = 64;
    private readonly List<string> _entries = new();
    private int _cursor = -1;

    internal bool CanGoBack => _cursor > 0;
    internal bool CanGoForward => _cursor >= 0 && _cursor + 1 < _entries.Count;
    internal string? Current => _cursor >= 0 ? _entries[_cursor] : null;

    internal void Reset(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        _entries.Clear();
        _entries.Add(path);
        _cursor = 0;
    }

    internal bool Navigate(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        if (Current != null &&
            string.Equals(Current, path, StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }
        if (_cursor + 1 < _entries.Count)
            _entries.RemoveRange(_cursor + 1, _entries.Count - _cursor - 1);
        _entries.Add(path);
        if (_entries.Count > Capacity)
            _entries.RemoveAt(0);
        _cursor = _entries.Count - 1;
        return true;
    }

    internal string? Back()
    {
        if (!CanGoBack) return null;
        _cursor--;
        return _entries[_cursor];
    }

    internal string? Forward()
    {
        if (!CanGoForward) return null;
        _cursor++;
        return _entries[_cursor];
    }
}
