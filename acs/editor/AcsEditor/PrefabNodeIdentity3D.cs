// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using AcsEditor.Packaging;

namespace AcsEditor;

/// <summary>legacy 3D Prefab原本へ不足しているPSID3Dを補う副作用なしの計算器。</summary>
internal static class PrefabNodeIdentity3D
{
    private const ulong FnvPrime = 1099511628211UL;
    private const ulong FirstSeed = 14695981039346656037UL;
    private const ulong SecondSeed = 7809847782465536322UL;

    /// <summary>既存identityを保持し、不足nodeへ決定論的な32桁source node IDを追加する。</summary>
    internal static bool TryEnsureSource(
        string sourceIdentity,
        string sourceText,
        out string updatedSource,
        out int addedCount,
        out string error)
    {
        updatedSource = sourceText ?? "";
        addedCount = 0;
        error = "";
        if (string.IsNullOrWhiteSpace(sourceIdentity))
            return Reject(updatedSource, "3D Prefab原本のsource identityがありません。", out updatedSource, out addedCount, out error);
        if (sourceText == null)
            return Reject(updatedSource, "3D Prefab原本がありません。", out updatedSource, out addedCount, out error);

        CanonicalSceneAdapterInspection inspection = CanonicalSceneAdapter.InspectText(sourceText, ".acs3d");
        if (inspection.HasErrors)
        {
            CanonicalSceneAdapterDiagnostic diagnostic = inspection.Diagnostics.First(
                static item => item.Severity == CanonicalSceneAdapterSeverity.Error);
            return Reject(sourceText, $"3D Prefab原本が不正です: {diagnostic.Code} (line {diagnostic.Line})", out updatedSource, out addedCount, out error);
        }

        List<SourceLine> lines = SplitLines(sourceText);
        var nodeLines = new Dictionary<int, int>();
        var identities = new Dictionary<int, string>();
        var usedIdentities = new HashSet<string>(StringComparer.Ordinal);
        for (int index = 0; index < lines.Count; ++index)
        {
            string[] tokens = Tokens(lines[index].Text);
            if (tokens.Length >= 3 && tokens[0] == "N3D" && TryInt(tokens[1], out int nodeId))
            {
                nodeLines[nodeId] = index;
                continue;
            }
            if (tokens.Length != 3 || tokens[0] != "PSID3D" || !TryInt(tokens[1], out int identityNode)) continue;
            if (!identities.TryAdd(identityNode, tokens[2]) || !usedIdentities.Add(tokens[2]))
                return Reject(sourceText, "3D Prefab原本内でPSID3Dが重複しています。", out updatedSource, out addedCount, out error);
        }
        if (nodeLines.Count == 0)
            return Reject(sourceText, "3D Prefab原本にN3Dがありません。", out updatedSource, out addedCount, out error);

        foreach ((int nodeId, int lineIndex) in nodeLines.OrderByDescending(static item => item.Value))
        {
            if (identities.ContainsKey(nodeId)) continue;
            string sourceNodeId = BuildSourceNodeId(sourceIdentity, nodeId);
            if (!usedIdentities.Add(sourceNodeId))
                return Reject(sourceText, "生成したPSID3Dが原本内で衝突しました。", out updatedSource, out addedCount, out error);
            InsertAfter(lines, lineIndex, $"PSID3D {nodeId.ToString(CultureInfo.InvariantCulture)} {sourceNodeId}");
            addedCount++;
        }

        updatedSource = JoinLines(lines);
        return true;
    }

    /// <summary>source pathと数値node IDをnative fallbackと同じFNV-1aへ畳み込む。</summary>
    internal static string BuildSourceNodeId(string sourceIdentity, int nodeId)
    {
        byte[] sourceBytes = Encoding.UTF8.GetBytes(sourceIdentity);
        ulong first = HashSourceNode(sourceBytes, nodeId, FirstSeed);
        ulong second = HashSourceNode(sourceBytes, nodeId, SecondSeed);
        return first.ToString("x16", CultureInfo.InvariantCulture) + second.ToString("x16", CultureInfo.InvariantCulture);
    }

    /// <summary>失敗結果を入力不変で返す。</summary>
    private static bool Reject(string sourceText, string message, out string updatedSource, out int addedCount, out string error)
    {
        updatedSource = sourceText;
        addedCount = 0;
        error = message;
        return false;
    }

    /// <summary>UTF-8 source identity、区切り、little-endian node IDを順番に畳み込む。</summary>
    private static ulong HashSourceNode(IReadOnlyList<byte> sourceBytes, int nodeId, ulong seed)
    {
        ulong hash = seed;
        unchecked
        {
            foreach (byte value in sourceBytes)
            {
                hash ^= value;
                hash *= FnvPrime;
            }
            hash ^= 0xffUL;
            hash *= FnvPrime;
            uint encodedId = (uint)nodeId;
            for (int byteIndex = 0; byteIndex < 4; ++byteIndex)
            {
                hash ^= (encodedId >> (byteIndex * 8)) & 0xffU;
                hash *= FnvPrime;
            }
        }
        return hash;
    }

    /// <summary>空白区切りtokenを返す。</summary>
    private static string[] Tokens(string line) => line.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);

    /// <summary>ASCII整数をInvariantCultureで読む。</summary>
    private static bool TryInt(string value, out int parsed) => int.TryParse(value, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out parsed);

    /// <summary>改行文字を各行へ保持したまま入力を分割する。</summary>
    private static List<SourceLine> SplitLines(string text)
    {
        var lines = new List<SourceLine>();
        int start = 0;
        int cursor = 0;
        while (cursor < text.Length)
        {
            if (text[cursor] != '\r' && text[cursor] != '\n')
            {
                cursor++;
                continue;
            }
            int endingLength = text[cursor] == '\r' && cursor + 1 < text.Length && text[cursor + 1] == '\n' ? 2 : 1;
            lines.Add(new(text[start..cursor], text.Substring(cursor, endingLength)));
            cursor += endingLength;
            start = cursor;
        }
        if (start < text.Length || lines.Count == 0) lines.Add(new(text[start..], ""));
        return lines;
    }

    /// <summary>指定行直後へsourceと同じ改行形式でdirectiveを追加する。</summary>
    private static void InsertAfter(IList<SourceLine> lines, int index, string text)
    {
        string ending = lines.Select(static line => line.Ending).FirstOrDefault(static value => value.Length > 0) ?? "\n";
        if (lines[index].Ending.Length == 0)
        {
            lines[index].Ending = ending;
            lines.Insert(index + 1, new(text, ""));
            return;
        }
        lines.Insert(index + 1, new(text, ending));
    }

    /// <summary>保持した改行文字を含めてsource textを再構成する。</summary>
    private static string JoinLines(IEnumerable<SourceLine> lines)
    {
        var builder = new StringBuilder();
        foreach (SourceLine line in lines) builder.Append(line.Text).Append(line.Ending);
        return builder.ToString();
    }

    /// <summary>本文と直後の改行を保持する1行。</summary>
    private sealed class SourceLine(string text, string ending)
    {
        internal string Text { get; } = text;
        internal string Ending { get; set; } = ending;
    }
}
