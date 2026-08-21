// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using AcsEditor.Packaging;

namespace AcsEditor;

/// <summary>3D Prefab rootから原本へ反映する明示値。</summary>
internal readonly record struct PrefabRootPropertyValues3D(
    bool Visible,
    bool Enabled,
    float Red,
    float Green,
    float Blue,
    float Alpha);

/// <summary>3D Prefab原本のroot propertyだけを差し替える副作用なしの計算器。</summary>
internal static class PrefabRootPropertyApply3D
{
    /// <summary>選択maskの値だけをACS3D原本へ反映し、失敗時は入力をそのまま返す。</summary>
    internal static bool TryBuildSource(
        string sourceText,
        PrefabRootProperty3D applyMask,
        PrefabRootPropertyValues3D values,
        out string updatedSource,
        out string error)
    {
        updatedSource = sourceText ?? "";
        error = "";
        if (sourceText == null)
            return Reject(updatedSource, "3D Prefab原本がありません。", out updatedSource, out error);
        if (applyMask == PrefabRootProperty3D.None ||
            (applyMask & ~PrefabRootProperty3D.All) != PrefabRootProperty3D.None)
        {
            return Reject(sourceText, "Apply対象のroot property maskが不正です。", out updatedSource, out error);
        }
        if ((applyMask & PrefabRootProperty3D.Color) != PrefabRootProperty3D.None &&
            (!float.IsFinite(values.Red) ||
             !float.IsFinite(values.Green) ||
             !float.IsFinite(values.Blue) ||
             !float.IsFinite(values.Alpha)))
        {
            return Reject(sourceText, "Apply対象のColorに有限でない値があります。", out updatedSource, out error);
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
        int rootLineIndex = -1;
        int rootId = -1;
        int rootCount = 0;
        for (int index = 0; index < lines.Count; ++index)
        {
            List<TokenRange> tokens = FindTokens(lines[index].Text);
            if (tokens.Count < 3 ||
                !TokenEquals(lines[index].Text, tokens[0], "N3D"))
            {
                continue;
            }
            if (!TryParseInt(lines[index].Text, tokens[1], out int nodeId) ||
                !TryParseInt(lines[index].Text, tokens[2], out int parentId) ||
                parentId != -1)
            {
                continue;
            }
            rootCount++;
            rootLineIndex = index;
            rootId = nodeId;
        }
        if (rootCount != 1 || rootLineIndex < 0)
        {
            return Reject(
                sourceText,
                "3D Prefab原本はparent=-1のroot N3Dを1件だけ持つ必要があります。",
                out updatedSource,
                out error);
        }

        List<TokenRange> rootTokens = FindTokens(lines[rootLineIndex].Text);
        if (rootTokens.Count < 17)
        {
            return Reject(sourceText, "3D Prefab rootのN3Dが不正です。", out updatedSource, out error);
        }

        int rootFlagsLineIndex = -1;
        List<TokenRange>? rootFlagTokens = null;
        for (int index = 0; index < lines.Count; ++index)
        {
            List<TokenRange> tokens = FindTokens(lines[index].Text);
            if (tokens.Count != 4 ||
                !TokenEquals(lines[index].Text, tokens[0], "FLG3D") ||
                !TryParseInt(lines[index].Text, tokens[1], out int flagsId) ||
                flagsId != rootId)
            {
                continue;
            }
            if (rootFlagsLineIndex >= 0)
            {
                return Reject(
                    sourceText,
                    "3D Prefab rootのFLG3Dが重複しています。",
                    out updatedSource,
                    out error);
            }
            rootFlagsLineIndex = index;
            rootFlagTokens = tokens;
        }

        if ((applyMask & PrefabRootProperty3D.Color) != PrefabRootProperty3D.None)
        {
            var colorReplacements = new Dictionary<int, string>
            {
                [13] = FormatColor(values.Red),
                [14] = FormatColor(values.Green),
                [15] = FormatColor(values.Blue),
                [16] = FormatColor(values.Alpha),
            };
            lines[rootLineIndex].Text = ReplaceTokens(
                lines[rootLineIndex].Text,
                rootTokens,
                colorReplacements);
        }

        PrefabRootProperty3D flagMask = applyMask &
            (PrefabRootProperty3D.Visible | PrefabRootProperty3D.Enabled);
        if (flagMask != PrefabRootProperty3D.None)
        {
            if (rootFlagsLineIndex >= 0 && rootFlagTokens != null)
            {
                var flagReplacements = new Dictionary<int, string>();
                if ((flagMask & PrefabRootProperty3D.Visible) != PrefabRootProperty3D.None)
                    flagReplacements[2] = values.Visible ? "1" : "0";
                if ((flagMask & PrefabRootProperty3D.Enabled) != PrefabRootProperty3D.None)
                    flagReplacements[3] = values.Enabled ? "1" : "0";
                lines[rootFlagsLineIndex].Text = ReplaceTokens(
                    lines[rootFlagsLineIndex].Text,
                    rootFlagTokens,
                    flagReplacements);
            }
            else
            {
                bool visible =
                    (flagMask & PrefabRootProperty3D.Visible) != PrefabRootProperty3D.None
                        ? values.Visible
                        : true;
                bool enabled =
                    (flagMask & PrefabRootProperty3D.Enabled) != PrefabRootProperty3D.None
                        ? values.Enabled
                        : true;
                if (!visible || !enabled)
                {
                    InsertAfter(
                        lines,
                        rootLineIndex,
                        $"FLG3D {rootId.ToString(CultureInfo.InvariantCulture)} {(visible ? 1 : 0)} {(enabled ? 1 : 0)}");
                }
            }
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

    /// <summary>native ACS3D serializerと同じ小数3桁でColorを表す。</summary>
    private static string FormatColor(float value) =>
        value.ToString("F3", CultureInfo.InvariantCulture);

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

    /// <summary>root N3D直後へsourceと同じ改行形式でdirectiveを追加する。</summary>
    private static void InsertAfter(
        IList<SourceLine> lines,
        int index,
        string text)
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
