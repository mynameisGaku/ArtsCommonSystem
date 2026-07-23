// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Text;

namespace AcsEditor;

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
