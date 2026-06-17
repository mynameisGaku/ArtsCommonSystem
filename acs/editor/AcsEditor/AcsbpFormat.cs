using System.Text;

namespace AcsEditor;

/// <summary>
/// .acsbp / ACSCENE の «CMP (コンポーネント木) ブロック» の解析・組み立てを 1 か所に集約する。
/// 以前は同じ走査/生成が BlueprintEditor (Serialize/Deserialize) と MainWindow
/// (ExtractComponents / ReplaceCmpBlock / AppendCmpBlock / SaveAsBlueprint / ConvertPrefabToBlueprint)
/// に重複していた。CMP の書式 (行数プレフィックス) を変える時はここだけ直せばよい。
/// </summary>
internal static class AcsbpFormat
{
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
}
