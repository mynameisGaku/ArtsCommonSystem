using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;

namespace AcsEditor;

internal sealed record AcsbpCookDocument(
    string[] Lines,
    int ComponentStart,
    int ComponentCount,
    string ComponentExtension)
{
    internal bool HasComponents => ComponentStart >= 0;

    internal bool IsComponentLine(int index) =>
        HasComponents &&
        index >= ComponentStart &&
        index < ComponentStart + ComponentCount;
}

/// <summary>
/// .acsbp / ACSCENE の «CMP (コンポーネント木) ブロック» の解析・組み立てを 1 か所に集約する。
/// 以前は同じ走査/生成が BlueprintEditor (Serialize/Deserialize) と MainWindow
/// (ExtractComponents / ReplaceCmpBlock / AppendCmpBlock / SaveAsBlueprint / ConvertPrefabToBlueprint)
/// に重複していた。CMP の書式 (行数プレフィックス) を変える時はここだけ直せばよい。
/// </summary>
internal static class AcsbpFormat
{
    private const int MaxComponentLines = 1_000_000;
    private static readonly HashSet<string> Scene2DDirectives = new(
        [
            "COMP", "CPROP", "NFLG", "SPRT", "PFAB", "MAT",
            "POLY", "RPLY", "SEL",
        ],
        StringComparer.Ordinal);
    private static readonly HashSet<string> Scene3DDirectives = new(
        [
            "N3D", "MSH3D", "SPR3D", "PLY3D", "CMP3D", "CPROP3D",
            "CAM3D", "FLG3D", "MAT3D", "PFAB3D", "EMPTY3D", "SEL3D",
        ],
        StringComparer.Ordinal);

    /// <summary>テキストを最初の «CMP &lt;n&gt;» ブロック (= コンポーネント木本文) と «それ以外» に分ける。
    /// CMP が無ければ Components は空、Rest は CRLF 除去後の全行。CMP 本文は Rest に含めない
    /// (ACSCENE の COMP/数値行がグラフ parser の C/N と衝突するため)。</summary>
    public static (string Components, string Remainder) SplitCmp(string text)
    {
        var all = text.Replace("\r", "").Split('\n');
        var comp = new StringBuilder();
        var rest = new StringBuilder();
        bool took = false;
        for (int i = 0; i < all.Length; i++)
        {
            if (!took && all[i].StartsWith("CMP ") && int.TryParse(all[i].Substring(4).Trim(), out int cn))
            {
                for (int j = 0; j < cn && i + 1 < all.Length; j++) comp.Append(all[++i]).Append('\n');
                took = true;
            }
            else rest.Append(all[i]).Append('\n');
        }
        return (comp.ToString(), rest.ToString());
    }

    /// <summary>最初の CMP ブロック本文 (コンポーネント木) を返す。無ければ空文字。</summary>
    public static string ExtractCmp(string text) => SplitCmp(text).Components;

    /// <summary>comp を «CMP &lt;行数&gt; + 各行» として sb に追記する (CMP 書式の唯一の生成元)。</summary>
    public static void AppendCmpBlock(StringBuilder sb, string comp)
    {
        var lines = comp.Replace("\r", "").TrimEnd('\n').Split('\n');
        sb.Append("CMP ").Append(lines.Length).Append('\n');
        foreach (var l in lines) sb.Append(l).Append('\n');
    }

    /// <summary>existing の CMP ブロックを comp で «その位置のまま» 差し替える (無ければ末尾に追加)。
    /// CMP 以外 (PARENT / VAR / graph) は温存。</summary>
    public static string ReplaceCmp(string existing, string comp)
    {
        var all = existing.Replace("\r", "").Split('\n');
        var sb = new StringBuilder();
        bool wrote = false;
        for (int i = 0; i < all.Length; i++)
        {
            if (all[i].StartsWith("CMP ") && int.TryParse(all[i].Substring(4).Trim(), out int cn))
            {
                i += cn;   // 古い CMP 行 + cn 行をスキップ
                AppendCmpBlock(sb, comp); wrote = true;
                continue;
            }
            sb.Append(all[i]).Append('\n');
        }
        if (!wrote) AppendCmpBlock(sb, comp);
        return sb.ToString();
    }

    /// <summary>コンポーネント木 comp だけを持つ最小の .acsbp テキスト ("ACSBP 1" + CMP ブロック) を組む。</summary>
    public static string WrapComponents(string comp)
    {
        var sb = new StringBuilder();
        sb.Append("ACSBP 1\n");
        AppendCmpBlock(sb, comp);
        return sb.ToString();
    }

    /// <summary>.acsbp / テキスト資産を BOM 無し UTF-8 で書き出す (エディタの全 .acsbp 書き出しで統一)。</summary>
    public static void Write(string path, string text) =>
        System.IO.File.WriteAllText(path, text, new UTF8Encoding(false));

    /// <summary>
    /// Strict package-boundary parser for ACSBP. Unlike the forgiving editor reader, this validates
    /// the CMP line-count boundary and the embedded scene grammar before Cook scans or rewrites any
    /// reference, so malformed input cannot publish a partial closure.
    /// </summary>
    internal static AcsbpCookDocument ParseForCook(string text)
    {
        ArgumentNullException.ThrowIfNull(text);
        if (text.Contains('\0'))
            throw new InvalidDataException("Blueprint payload contains a NUL character.");

        string normalized = text.Replace("\r\n", "\n", StringComparison.Ordinal);
        if (normalized.Contains('\r'))
            throw new InvalidDataException(
                "Blueprint payload contains a bare carriage return.");
        string[] lines = normalized.Split('\n');
        if (lines.Length == 0 ||
            !string.Equals(lines[0], "ACSBP 1", StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "Blueprint payload must begin with exactly 'ACSBP 1'.");
        }

        int componentStart = -1;
        int componentCount = 0;
        string componentExtension = "";
        int parentCount = 0;
        for (int index = 1; index < lines.Length; ++index)
        {
            string line = lines[index];
            if (TryParseCanonicalParentDirective(line, out _))
            {
                if (++parentCount > 1)
                    throw new InvalidDataException(
                        "Blueprint payload may contain at most one PARENT directive.");
                continue;
            }
            if (!TryParseCanonicalCmpDirective(
                    line,
                    out int parsedComponentCount))
                continue;
            if (componentStart >= 0)
                throw new InvalidDataException(
                    "Blueprint payload may contain at most one CMP block.");

            componentCount = parsedComponentCount;
            componentStart = index + 1;
            if ((long)componentStart + componentCount > lines.LongLength)
                throw new InvalidDataException(
                    "Blueprint CMP block is truncated.");
            componentExtension = lines[componentStart] switch
            {
                "ACS3D v2" => ".acs3d",
                "ACSCENE v1" => ".acscene",
                _ => throw new InvalidDataException(
                    "Blueprint CMP block must begin with exactly 'ACS3D v2' or 'ACSCENE v1'."),
            };
            ValidateComponentGrammar(
                lines,
                componentStart,
                componentCount,
                componentExtension);
            index += componentCount;
        }

        return new(
            lines,
            componentStart,
            componentCount,
            componentExtension);
    }

    /// <summary>
    /// Parses the writer's canonical «PARENT &lt;asset path&gt;» form.
    /// A PARENT token followed by any ASCII whitespace is reserved, so malformed
    /// whitespace cannot turn an inheritance edge into an ignored graph line.
    /// </summary>
    internal static bool TryParseCanonicalParentDirective(
        string line,
        out string path)
    {
        path = "";
        if (!HasAsciiDirectiveToken(line, "PARENT"))
            return false;

        const string prefix = "PARENT ";
        if (!line.StartsWith(prefix, StringComparison.Ordinal))
            throw new InvalidDataException(
                "Blueprint PARENT must use exactly one ASCII space after the directive.");

        string candidate = line[prefix.Length..];
        if (candidate.Length == 0 ||
            IsAsciiWhitespace(candidate[0]) ||
            IsAsciiWhitespace(candidate[^1]) ||
            candidate.Any(static value =>
                IsAsciiWhitespace(value) && value != ' '))
        {
            throw new InvalidDataException(
                "Blueprint PARENT must contain one canonical non-empty asset path.");
        }
        path = candidate;
        return true;
    }

    private static bool TryParseCanonicalCmpDirective(
        string line,
        out int componentCount)
    {
        componentCount = 0;
        if (!HasAsciiDirectiveToken(line, "CMP"))
            return false;

        const string prefix = "CMP ";
        if (!line.StartsWith(prefix, StringComparison.Ordinal))
            throw new InvalidDataException(
                "Blueprint CMP must use exactly one ASCII space after the directive.");

        string countText = line[prefix.Length..];
        if (countText.Length == 0 ||
            countText.Any(static value => value is < '0' or > '9') ||
            !int.TryParse(
                countText,
                NumberStyles.None,
                CultureInfo.InvariantCulture,
                out componentCount) ||
            componentCount is < 1 or > MaxComponentLines ||
            !string.Equals(
                countText,
                componentCount.ToString(CultureInfo.InvariantCulture),
                StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                $"Blueprint CMP count must be one canonical integer between 1 and {MaxComponentLines}.");
        }
        return true;
    }

    private static bool HasAsciiDirectiveToken(
        string line,
        string directive)
    {
        if (!line.StartsWith(directive, StringComparison.Ordinal))
            return false;
        return line.Length == directive.Length ||
               IsAsciiWhitespace(line[directive.Length]);
    }

    private static bool IsAsciiWhitespace(char value) =>
        value == ' ' || value is >= '\t' and <= '\r';

    private static void ValidateComponentGrammar(
        IReadOnlyList<string> lines,
        int start,
        int count,
        string extension)
    {
        int end = start + count;
        if (extension == ".acs3d")
        {
            for (int index = start + 1; index < end; ++index)
            {
                string directive = FirstToken(lines[index]);
                if (directive.Length == 0 || !Scene3DDirectives.Contains(directive))
                {
                    throw new InvalidDataException(
                        $"Unknown ACS3D directive '{directive}' in Blueprint CMP block.");
                }
            }
            return;
        }

        if (count < 2 ||
            !int.TryParse(
                lines[start + 1],
                NumberStyles.None,
                CultureInfo.InvariantCulture,
                out int nodeCount) ||
            nodeCount is < 0 or > MaxComponentLines)
        {
            throw new InvalidDataException(
                "Blueprint ACSCENE CMP block has an invalid node count.");
        }
        int firstDirective = start + 2 + nodeCount;
        if (firstDirective > end)
            throw new InvalidDataException(
                "Blueprint ACSCENE CMP block is missing node records.");
        for (int index = start + 2; index < firstDirective; ++index)
        {
            string first = FirstToken(lines[index]);
            if (!int.TryParse(
                    first,
                    NumberStyles.Integer,
                    CultureInfo.InvariantCulture,
                    out _))
            {
                throw new InvalidDataException(
                    "Blueprint ACSCENE CMP block contains a malformed node record.");
            }
        }
        for (int index = firstDirective; index < end; ++index)
        {
            string directive = FirstToken(lines[index]);
            if (directive.Length == 0 || !Scene2DDirectives.Contains(directive))
            {
                throw new InvalidDataException(
                    $"Unknown ACSCENE directive '{directive}' in Blueprint CMP block.");
            }
        }
    }

    private static string FirstToken(string line)
    {
        int separator = line.IndexOfAny([' ', '\t']);
        return separator < 0 ? line : line[..separator];
    }
}
