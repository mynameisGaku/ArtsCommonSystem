// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using AcsEditor.Packaging;

namespace AcsEditor;

/// <summary>3D Prefab child nodeから原本へ反映する明示値。</summary>
internal readonly record struct PrefabNodePropertyValues3D(
    bool Visible,
    bool Enabled,
    float Red,
    float Green,
    float Blue,
    float Alpha,
    float PositionX,
    float PositionY,
    float PositionZ,
    float RotationX,
    float RotationY,
    float RotationZ,
    float ScaleX,
    float ScaleY,
    float ScaleZ,
    string MaterialPath);

/// <summary>PSID3Dで選んだ3D Prefab原本nodeのpropertyだけを差し替える副作用なしの計算器。</summary>
internal static class PrefabNodePropertyApply3D
{
    /// <summary>source node IDに対応するnodeの選択値だけを更新し、失敗時は入力を保持する。</summary>
    internal static bool TryBuildSource(
        string sourceText,
        string sourceNodeId,
        PrefabNodeProperty3D applyMask,
        PrefabNodePropertyValues3D values,
        out string updatedSource,
        out string error)
    {
        updatedSource = sourceText ?? "";
        error = "";
        if (sourceText == null)
            return Reject(updatedSource, "3D Prefab原本がありません。", out updatedSource, out error);
        if (!IsCanonicalSourceNodeId(sourceNodeId))
            return Reject(sourceText, "Apply対象のsource node IDが不正です。", out updatedSource, out error);
        if (applyMask == PrefabNodeProperty3D.None ||
            (applyMask & ~PrefabNodeProperty3D.All) != PrefabNodeProperty3D.None)
        {
            return Reject(sourceText, "Apply対象のnode property maskが不正です。", out updatedSource, out error);
        }
        if ((applyMask & PrefabNodeProperty3D.Color) != PrefabNodeProperty3D.None &&
            (!float.IsFinite(values.Red) ||
             !float.IsFinite(values.Green) ||
             !float.IsFinite(values.Blue) ||
             !float.IsFinite(values.Alpha)))
        {
            return Reject(sourceText, "Apply対象のColorに有限でない値があります。", out updatedSource, out error);
        }
        if ((applyMask & PrefabNodeProperty3D.Transform) != PrefabNodeProperty3D.None &&
            (!float.IsFinite(values.PositionX) ||
             !float.IsFinite(values.PositionY) ||
             !float.IsFinite(values.PositionZ) ||
             !float.IsFinite(values.RotationX) ||
             !float.IsFinite(values.RotationY) ||
             !float.IsFinite(values.RotationZ) ||
             !float.IsFinite(values.ScaleX) ||
             !float.IsFinite(values.ScaleY) ||
             !float.IsFinite(values.ScaleZ)))
        {
            return Reject(sourceText, "Apply対象のTransformに有限でない値があります。", out updatedSource, out error);
        }
        if ((applyMask & PrefabNodeProperty3D.Material) != PrefabNodeProperty3D.None &&
            !IsValidMaterialPath(values.MaterialPath))
        {
            return Reject(sourceText, "Apply対象のMaterial pathが不正です。", out updatedSource, out error);
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
        int targetNodeId = -1;
        int identityMatches = 0;
        for (int index = 0; index < lines.Count; ++index)
        {
            List<TokenRange> tokens = FindTokens(lines[index].Text);
            if (tokens.Count != 3 ||
                !TokenEquals(lines[index].Text, tokens[0], "PSID3D") ||
                !TokenEquals(lines[index].Text, tokens[2], sourceNodeId) ||
                !TryParseInt(lines[index].Text, tokens[1], out int nodeId))
            {
                continue;
            }
            targetNodeId = nodeId;
            identityMatches++;
        }
        if (identityMatches != 1)
            return Reject(sourceText, "Apply対象のPSID3Dを一意に解決できません。", out updatedSource, out error);

        int targetLineIndex = -1;
        List<TokenRange>? targetTokens = null;
        for (int index = 0; index < lines.Count; ++index)
        {
            List<TokenRange> tokens = FindTokens(lines[index].Text);
            if (tokens.Count < 17 ||
                !TokenEquals(lines[index].Text, tokens[0], "N3D") ||
                !TryParseInt(lines[index].Text, tokens[1], out int nodeId) ||
                nodeId != targetNodeId)
            {
                continue;
            }
            if (targetLineIndex >= 0)
                return Reject(sourceText, "Apply対象のN3Dが重複しています。", out updatedSource, out error);
            targetLineIndex = index;
            targetTokens = tokens;
        }
        if (targetLineIndex < 0 || targetTokens == null)
            return Reject(sourceText, "Apply対象のN3Dがありません。", out updatedSource, out error);

        int flagsLineIndex = -1;
        List<TokenRange>? flagTokens = null;
        for (int index = 0; index < lines.Count; ++index)
        {
            List<TokenRange> tokens = FindTokens(lines[index].Text);
            if (tokens.Count != 4 ||
                !TokenEquals(lines[index].Text, tokens[0], "FLG3D") ||
                !TryParseInt(lines[index].Text, tokens[1], out int flagsId) ||
                flagsId != targetNodeId)
            {
                continue;
            }
            if (flagsLineIndex >= 0)
                return Reject(sourceText, "Apply対象nodeのFLG3Dが重複しています。", out updatedSource, out error);
            flagsLineIndex = index;
            flagTokens = tokens;
        }

        int materialLineIndex = -1;
        List<TokenRange>? materialTokens = null;
        for (int index = 0; index < lines.Count; ++index)
        {
            List<TokenRange> tokens = FindTokens(lines[index].Text);
            if (tokens.Count < 3 ||
                !TokenEquals(lines[index].Text, tokens[0], "MAT3D") ||
                !TryParseInt(lines[index].Text, tokens[1], out int materialId) ||
                materialId != targetNodeId)
            {
                continue;
            }
            if (materialLineIndex >= 0)
                return Reject(sourceText, "Apply対象nodeのMAT3Dが重複しています。", out updatedSource, out error);
            materialLineIndex = index;
            materialTokens = tokens;
        }

        var nodeReplacements = new Dictionary<int, string>();
        if ((applyMask & PrefabNodeProperty3D.Position) != PrefabNodeProperty3D.None)
        {
            nodeReplacements[4] = FormatNumber(values.PositionX);
            nodeReplacements[5] = FormatNumber(values.PositionY);
            nodeReplacements[6] = FormatNumber(values.PositionZ);
        }
        if ((applyMask & PrefabNodeProperty3D.Rotation) != PrefabNodeProperty3D.None)
        {
            nodeReplacements[7] = FormatNumber(values.RotationX);
            nodeReplacements[8] = FormatNumber(values.RotationY);
            nodeReplacements[9] = FormatNumber(values.RotationZ);
        }
        if ((applyMask & PrefabNodeProperty3D.Scale) != PrefabNodeProperty3D.None)
        {
            nodeReplacements[10] = FormatNumber(values.ScaleX);
            nodeReplacements[11] = FormatNumber(values.ScaleY);
            nodeReplacements[12] = FormatNumber(values.ScaleZ);
        }
        if ((applyMask & PrefabNodeProperty3D.Color) != PrefabNodeProperty3D.None)
        {
            nodeReplacements[13] = FormatNumber(values.Red);
            nodeReplacements[14] = FormatNumber(values.Green);
            nodeReplacements[15] = FormatNumber(values.Blue);
            nodeReplacements[16] = FormatNumber(values.Alpha);
        }
        if (nodeReplacements.Count > 0)
        {
            lines[targetLineIndex].Text = ReplaceTokens(
                lines[targetLineIndex].Text,
                targetTokens,
                nodeReplacements);
        }

        bool insertMaterial = false;
        if ((applyMask & PrefabNodeProperty3D.Material) != PrefabNodeProperty3D.None)
        {
            if (materialLineIndex >= 0 && materialTokens != null)
            {
                if (values.MaterialPath.Length == 0)
                {
                    lines.RemoveAt(materialLineIndex);
                    if (flagsLineIndex > materialLineIndex) flagsLineIndex--;
                }
                else
                {
                    lines[materialLineIndex].Text =
                        lines[materialLineIndex].Text[..materialTokens[2].Start] +
                        values.MaterialPath;
                }
            }
            else
            {
                insertMaterial = values.MaterialPath.Length > 0;
            }
        }

        PrefabNodeProperty3D flagMask = applyMask &
            (PrefabNodeProperty3D.Visible | PrefabNodeProperty3D.Enabled);
        if (flagMask != PrefabNodeProperty3D.None)
        {
            if (flagsLineIndex >= 0 && flagTokens != null)
            {
                var replacements = new Dictionary<int, string>();
                if ((flagMask & PrefabNodeProperty3D.Visible) != PrefabNodeProperty3D.None)
                    replacements[2] = values.Visible ? "1" : "0";
                if ((flagMask & PrefabNodeProperty3D.Enabled) != PrefabNodeProperty3D.None)
                    replacements[3] = values.Enabled ? "1" : "0";
                lines[flagsLineIndex].Text = ReplaceTokens(
                    lines[flagsLineIndex].Text,
                    flagTokens,
                    replacements);
            }
            else
            {
                bool visible =
                    (flagMask & PrefabNodeProperty3D.Visible) != PrefabNodeProperty3D.None
                        ? values.Visible
                        : true;
                bool enabled =
                    (flagMask & PrefabNodeProperty3D.Enabled) != PrefabNodeProperty3D.None
                        ? values.Enabled
                        : true;
                if (!visible || !enabled)
                {
                    InsertAfter(
                        lines,
                        targetLineIndex,
                        $"FLG3D {targetNodeId.ToString(CultureInfo.InvariantCulture)} {(visible ? 1 : 0)} {(enabled ? 1 : 0)}");
                }
            }
        }
        if (insertMaterial)
        {
            InsertAfter(
                lines,
                targetLineIndex,
                $"MAT3D {targetNodeId.ToString(CultureInfo.InvariantCulture)} {values.MaterialPath}");
        }

        updatedSource = JoinLines(lines);
        return true;
    }

    /// <summary>32桁小文字hexのsource node IDだけを受理する。</summary>
    private static bool IsCanonicalSourceNodeId(string value) =>
        value is { Length: 32 } && value.All(static character =>
            character is >= '0' and <= '9' or >= 'a' and <= 'f');

    /// <summary>空文字または改行を含まない259-byte以下の.acsmat pathだけを受理する。</summary>
    private static bool IsValidMaterialPath(string? value) =>
        value != null &&
        (value.Length == 0 ||
         (!string.IsNullOrWhiteSpace(value) &&
          !value.Contains('\r') &&
          !value.Contains('\n') &&
          Encoding.UTF8.GetByteCount(value) <= 259 &&
          value.EndsWith(".acsmat", StringComparison.OrdinalIgnoreCase)));

    /// <summary>失敗結果を入力不変で返す。</summary>
    private static bool Reject(string sourceText, string message, out string updatedSource, out string error)
    {
        updatedSource = sourceText;
        error = message;
        return false;
    }

    /// <summary>native ACS3D serializerと同じ小数3桁で数値を表す。</summary>
    private static string FormatNumber(float value) =>
        value.ToString("F3", CultureInfo.InvariantCulture);

    /// <summary>ASCII整数tokenをInvariantCultureで読む。</summary>
    private static bool TryParseInt(string text, TokenRange token, out int value) =>
        int.TryParse(text.AsSpan(token.Start, token.Length), NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out value);

    /// <summary>tokenの内容が指定値と一致するか返す。</summary>
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
    private static string ReplaceTokens(string text, IReadOnlyList<TokenRange> tokens, IReadOnlyDictionary<int, string> replacements)
    {
        var builder = new StringBuilder(text.Length + 32);
        int cursor = 0;
        for (int index = 0; index < tokens.Count; ++index)
        {
            TokenRange token = tokens[index];
            builder.Append(text, cursor, token.Start - cursor);
            if (replacements.TryGetValue(index, out string? replacement)) builder.Append(replacement);
            else builder.Append(text, token.Start, token.Length);
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
            int endingLength = text[cursor] == '\r' && cursor + 1 < text.Length && text[cursor + 1] == '\n' ? 2 : 1;
            lines.Add(new(text[start..cursor], text.Substring(cursor, endingLength)));
            cursor += endingLength;
            start = cursor;
        }
        if (start < text.Length || lines.Count == 0) lines.Add(new(text[start..], ""));
        return lines;
    }

    /// <summary>対象N3D直後へsourceと同じ改行形式でdirectiveを追加する。</summary>
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

    /// <summary>1つのtoken位置。</summary>
    private readonly record struct TokenRange(int Start, int Length);

    /// <summary>本文と直後の改行を保持する1行。</summary>
    private sealed class SourceLine(string text, string ending)
    {
        internal string Text { get; set; } = text;
        internal string Ending { get; set; } = ending;
    }
}
