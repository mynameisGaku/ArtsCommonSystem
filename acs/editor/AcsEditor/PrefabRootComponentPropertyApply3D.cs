// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using AcsEditor.Packaging;

namespace AcsEditor;

/// <summary>3D Prefab root componentから原本へ反映する4成分値。</summary>
internal readonly record struct PrefabRootComponentPropertyValue3D(
    float X,
    float Y,
    float Z,
    float W);

/// <summary>3D Prefab原本のroot component propertyだけを差し替える副作用なしの計算器。</summary>
internal static class PrefabRootComponentPropertyApply3D
{
    /// <summary>型名でroot componentを解決し、選択propertyだけをACS3D原本へ反映する。</summary>
    internal static bool TryBuildSource(
        string sourceText,
        string componentTypeName,
        int property,
        PrefabRootComponentPropertyValue3D value,
        out string updatedSource,
        out string error)
    {
        updatedSource = sourceText ?? "";
        error = "";
        if (sourceText == null)
            return Reject(updatedSource, "3D Prefab原本がありません。", out updatedSource, out error);
        if (string.IsNullOrWhiteSpace(componentTypeName) ||
            componentTypeName.Any(char.IsWhiteSpace))
        {
            return Reject(sourceText, "Apply対象のcomponent型名が不正です。", out updatedSource, out error);
        }
        if (property < 0 || property >= 32)
            return Reject(sourceText, "Apply対象のproperty indexが不正です。", out updatedSource, out error);
        if (!float.IsFinite(value.X) ||
            !float.IsFinite(value.Y) ||
            !float.IsFinite(value.Z) ||
            !float.IsFinite(value.W))
        {
            return Reject(sourceText, "Apply対象のcomponent propertyに有限でない値があります。", out updatedSource, out error);
        }

        CanonicalSceneAdapterInspection inspection =
            CanonicalSceneAdapter.InspectText(sourceText, ".acs3d");
        if (inspection.HasErrors)
        {
            CanonicalSceneAdapterDiagnostic diagnostic = inspection.Diagnostics.First(
                static item => item.Severity == CanonicalSceneAdapterSeverity.Error);
            return Reject(
                sourceText,
                $"3D Prefab原本が不正です: {diagnostic.Code} (line {diagnostic.Line})",
                out updatedSource,
                out error);
        }

        List<SourceLine> lines = SplitLines(sourceText);
        int rootId = -1;
        int rootCount = 0;
        foreach (SourceLine line in lines)
        {
            List<TokenRange> tokens = FindTokens(line.Text);
            if (tokens.Count < 3 || !TokenEquals(line.Text, tokens[0], "N3D")) continue;
            if (!TryParseInt(line.Text, tokens[1], out int nodeId) ||
                !TryParseInt(line.Text, tokens[2], out int parentId) ||
                parentId != -1)
            {
                continue;
            }
            rootCount++;
            rootId = nodeId;
        }
        if (rootCount != 1)
        {
            return Reject(
                sourceText,
                "3D Prefab原本はparent=-1のroot N3Dを1件だけ持つ必要があります。",
                out updatedSource,
                out error);
        }

        int targetComponentLine = -1;
        int targetSlot = -1;
        int rootSlot = 0;
        for (int index = 0; index < lines.Count; ++index)
        {
            List<TokenRange> tokens = FindTokens(lines[index].Text);
            if (tokens.Count != 3 ||
                !TokenEquals(lines[index].Text, tokens[0], "CMP3D") ||
                !TryParseInt(lines[index].Text, tokens[1], out int nodeId) ||
                nodeId != rootId)
            {
                continue;
            }
            if (TokenEquals(lines[index].Text, tokens[2], componentTypeName))
            {
                if (targetComponentLine >= 0)
                {
                    return Reject(
                        sourceText,
                        $"3D Prefab rootの{componentTypeName}が重複しています。",
                        out updatedSource,
                        out error);
                }
                targetComponentLine = index;
                targetSlot = rootSlot;
            }
            rootSlot++;
        }
        if (targetComponentLine < 0 || targetSlot < 0)
        {
            return Reject(
                sourceText,
                $"3D Prefab rootに{componentTypeName}がありません。",
                out updatedSource,
                out error);
        }

        int targetPropertyLine = -1;
        int insertAfterLine = targetComponentLine;
        List<TokenRange>? targetPropertyTokens = null;
        for (int index = 0; index < lines.Count; ++index)
        {
            List<TokenRange> tokens = FindTokens(lines[index].Text);
            if (tokens.Count != 8 ||
                !TokenEquals(lines[index].Text, tokens[0], "CPROP3D") ||
                !TryParseInt(lines[index].Text, tokens[1], out int nodeId) ||
                !TryParseInt(lines[index].Text, tokens[2], out int slot) ||
                !TryParseInt(lines[index].Text, tokens[3], out int propertyIndex) ||
                nodeId != rootId ||
                slot != targetSlot)
            {
                continue;
            }
            if (index > insertAfterLine) insertAfterLine = index;
            if (propertyIndex != property) continue;
            if (targetPropertyLine >= 0)
            {
                return Reject(
                    sourceText,
                    "3D Prefab rootのApply対象CPROP3Dが重複しています。",
                    out updatedSource,
                    out error);
            }
            targetPropertyLine = index;
            targetPropertyTokens = tokens;
        }

        string x = FormatValue(value.X);
        string y = FormatValue(value.Y);
        string z = FormatValue(value.Z);
        string w = FormatValue(value.W);
        if (targetPropertyLine >= 0 && targetPropertyTokens != null)
        {
            lines[targetPropertyLine].Text = ReplaceTokens(
                lines[targetPropertyLine].Text,
                targetPropertyTokens,
                new Dictionary<int, string>
                {
                    [4] = x,
                    [5] = y,
                    [6] = z,
                    [7] = w,
                });
        }
        else
        {
            InsertAfter(
                lines,
                insertAfterLine,
                $"CPROP3D {rootId.ToString(CultureInfo.InvariantCulture)} {targetSlot.ToString(CultureInfo.InvariantCulture)} {property.ToString(CultureInfo.InvariantCulture)} {x} {y} {z} {w}");
        }

        updatedSource = JoinLines(lines);
        return true;
    }

    /// <summary>失敗結果を入力不変で返す。</summary>
    private static bool Reject(
        string sourceText,
        string message,
        out string updatedSource,
        out string error)
    {
        updatedSource = sourceText;
        error = message;
        return false;
    }

    /// <summary>native ACS3D serializerと同じ小数4桁でcomponent値を表す。</summary>
    private static string FormatValue(float value) =>
        value.ToString("F4", CultureInfo.InvariantCulture);

    /// <summary>ASCII整数tokenをInvariantCultureで読む。</summary>
    private static bool TryParseInt(string text, TokenRange token, out int value) =>
        int.TryParse(
            text.AsSpan(token.Start, token.Length),
            NumberStyles.AllowLeadingSign,
            CultureInfo.InvariantCulture,
            out value);

    /// <summary>tokenの内容が指定directiveと一致するか返す。</summary>
    private static bool TokenEquals(string text, TokenRange token, string expected) =>
        text.AsSpan(token.Start, token.Length).SequenceEqual(expected.AsSpan());

    /// <summary>行内の空白区切りtoken位置を順番に返す。</summary>
    private static List<TokenRange> FindTokens(string text)
    {
        var tokens = new List<TokenRange>();
        int cursor = 0;
        while (cursor < text.Length)
        {
            while (cursor < text.Length && char.IsWhiteSpace(text[cursor])) cursor++;
            int start = cursor;
            while (cursor < text.Length && !char.IsWhiteSpace(text[cursor])) cursor++;
            if (cursor > start) tokens.Add(new(start, cursor - start));
        }
        return tokens;
    }

    /// <summary>指定tokenだけを置換し、その他の文字と空白を保持する。</summary>
    private static string ReplaceTokens(
        string text,
        IReadOnlyList<TokenRange> tokens,
        IReadOnlyDictionary<int, string> replacements)
    {
        var builder = new StringBuilder(text.Length + 32);
        int cursor = 0;
        for (int index = 0; index < tokens.Count; ++index)
        {
            TokenRange token = tokens[index];
            builder.Append(text, cursor, token.Start - cursor);
            if (replacements.TryGetValue(index, out string? replacement))
                builder.Append(replacement);
            else
                builder.Append(text, token.Start, token.Length);
            cursor = token.Start + token.Length;
        }
        builder.Append(text, cursor, text.Length - cursor);
        return builder.ToString();
    }

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
            int endingLength =
                text[cursor] == '\r' && cursor + 1 < text.Length && text[cursor + 1] == '\n'
                    ? 2
                    : 1;
            lines.Add(new(
                text[start..cursor],
                text.Substring(cursor, endingLength)));
            cursor += endingLength;
            start = cursor;
        }
        if (start < text.Length || lines.Count == 0)
            lines.Add(new(text[start..], ""));
        return lines;
    }

    /// <summary>指定行直後へsourceと同じ改行形式でdirectiveを追加する。</summary>
    private static void InsertAfter(IList<SourceLine> lines, int index, string text)
    {
        string ending = lines
            .Select(static line => line.Ending)
            .FirstOrDefault(static value => value.Length > 0) ?? "\n";
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
        foreach (SourceLine line in lines)
            builder.Append(line.Text).Append(line.Ending);
        return builder.ToString();
    }

    /// <summary>1つのtoken位置。</summary>
    private readonly record struct TokenRange(int Start, int Length);

    /// <summary>本文と直後の改行を保持する1行。</summary>
    private sealed class SourceLine(string text, string ending)
    {
        internal string Text { get; set; } = text;
        internal string Ending { get; set; } = ending;
    }
}
