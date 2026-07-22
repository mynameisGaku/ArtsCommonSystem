using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;

namespace AcsEditor;

/// <summary>
/// UE 風の ACS_CLASS / ACS_PROPERTY マーカーをヘッダから走査し、ACS_REGISTER_COMPONENT(...) 相当の
/// リフレクション登録コードを 1 つの .gen.cpp に生成する (UHT 相当の軽量コードジェネレータ)。
/// ビルド前に実行される。マーカー自体は gameframework/AcsClass.h で「何も生成しない」マクロ。
/// </summary>
public static class ReflectionCodegen
{
    public const string GenFileName = "__acs_reflect.gen.cpp";

    private sealed record PropInfo(string Type, string Name, string Default, string Specifiers);
    private sealed record MethodInfo(string Name, string Specifiers);
    private sealed record ClassInfo(
        string Name, string Base, bool DeclaresComponentKind,
        List<PropInfo> Props, List<MethodInfo> Methods);

    // ACS_CLASS(...) の後の  class [API] Name : public Base {   を捉える。
    private static readonly Regex ClassRe = new(
        @"ACS_CLASS\s*\([^)]*\)\s*class\s+(?:\w+\s+)?(\w+)\s*:\s*public\s+([\w:]+)\s*\{",
        RegexOptions.Compiled);

    // ACS_PROPERTY(specifiers...)  Type name [= default];  — group1=指定子, 2=型, 3=名前, 4=既定
    private static readonly Regex PropRe = new(
        @"ACS_PROPERTY\s*\(([^)]*)\)\s*([\w:]+)\s+(\w+)\s*(?:=\s*([^;{]+?))?\s*;",
        RegexOptions.Compiled);

    // ACS_FUNCTION(specifiers...)  void Name()  — group1=指定子, 2=名前 (引数なし void のみ)
    private static readonly Regex MethodRe = new(
        @"ACS_FUNCTION\s*\(([^)]*)\)\s*void\s+(\w+)\s*\(\s*\)",
        RegexOptions.Compiled);

    /// <summary>プロジェクトの Source を走査して登録コードを生成する。新規にファイルを作ったら true。</summary>
    public static bool Generate(Project project, Action<string>? log = null)
    {
        string dir = project.SourceDir;
        if (!Directory.Exists(dir)) return false;
        string genPath = Path.Combine(dir, GenFileName);
        bool existedBefore = File.Exists(genPath);

        var includes = new List<string>();
        var registrations = new List<string>();
        var discovered = new List<ClassInfo>();

        foreach (string hPath in Directory.GetFiles(dir, "*.h").OrderBy(p => p, StringComparer.Ordinal))
        {
            string text = StripComments(File.ReadAllText(hPath));
            var classes = ParseClasses(text);
            if (classes.Count == 0) continue;
            includes.Add(Path.GetFileName(hPath));
            discovered.AddRange(classes);
        }

        var bases = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var c in discovered)
            bases[c.Name] = UnqualifiedTypeName(c.Base);
        foreach (var c in discovered)
            registrations.Add(EmitRegistration(c, IsComponentClass(c, bases)));
        int total = discovered.Count;

        var sb = new StringBuilder();
        sb.Append("// ACS Editor reflection codegen による自動生成。直接編集しない。\n");
        sb.Append("// ACS_CLASS / ACS_PROPERTY / ACS_FUNCTION マーカーから生成された型登録。\n");
        sb.Append("#include \"gameframework/Reflect.h\"\n");
        sb.Append("#include \"gameframework/ReflectMethod.h\"\n");
        foreach (string inc in includes.Distinct()) sb.Append($"#include \"{inc}\"\n");
        sb.Append("\n");
        foreach (string reg in registrations) sb.Append(reg);
        if (registrations.Count == 0) sb.Append("// (ACS_CLASS マーカー無し)\n");

        string content = sb.ToString();
        if (existedBefore && SafeRead(genPath) == content) return false;   // 変化なし → watcher を無駄に起こさない
        File.WriteAllText(genPath, content, new UTF8Encoding(false));
        log?.Invoke($"reflection codegen: {total} 型を生成 ({GenFileName})");
        return !existedBefore;
    }

    private static string SafeRead(string p) { try { return File.ReadAllText(p); } catch { return ""; } }

    private static List<ClassInfo> ParseClasses(string text)
    {
        var result = new List<ClassInfo>();
        foreach (Match m in ClassRe.Matches(text))
        {
            string name = m.Groups[1].Value;
            string baseName = m.Groups[2].Value;
            // class 本体 (対応する '}' まで) を波括弧カウントで切り出す。
            int open = m.Index + m.Length - 1;   // '{' の位置
            string body = ExtractBraceBody(text, open);
            var props = new List<PropInfo>();
            foreach (Match pm in PropRe.Matches(body))
                props.Add(new PropInfo(pm.Groups[2].Value, pm.Groups[3].Value,
                                       pm.Groups[4].Value.Trim(), pm.Groups[1].Value));
            var methods = new List<MethodInfo>();
            foreach (Match mm in MethodRe.Matches(body))
                methods.Add(new MethodInfo(mm.Groups[2].Value, mm.Groups[1].Value));
            bool declaresComponentKind =
                body.Contains("ACS_GAME_COMPONENT_KIND", StringComparison.Ordinal);
            result.Add(new ClassInfo(name, baseName, declaresComponentKind, props, methods));
        }
        return result;
    }

    // openBrace 位置の '{' から対応する '}' までの中身を返す。
    private static string ExtractBraceBody(string text, int openBrace)
    {
        int depth = 0;
        for (int i = openBrace; i < text.Length; i++)
        {
            if (text[i] == '{') depth++;
            else if (text[i] == '}') { depth--; if (depth == 0) return text.Substring(openBrace + 1, i - openBrace - 1); }
        }
        return text.Substring(openBrace + 1);
    }

    private static string UnqualifiedTypeName(string name)
    {
        int separator = name.LastIndexOf("::", StringComparison.Ordinal);
        return separator >= 0 ? name.Substring(separator + 2) : name;
    }

    private static bool IsComponentClass(
        ClassInfo candidate, IReadOnlyDictionary<string, string> bases)
    {
        // 外部 engine component を基底にした場合でも、必須の Kind 宣言があれば
        // 名前ヒューリスティックに頼らず component と判定できる。
        if (candidate.DeclaresComponentKind) return true;

        string current = UnqualifiedTypeName(candidate.Base);
        var visited = new HashSet<string>(StringComparer.Ordinal);
        while (visited.Add(current))
        {
            if (current == "AComponent") return true;
            if (!bases.TryGetValue(current, out string? parent))
                return current.EndsWith("Component", StringComparison.Ordinal);
            current = UnqualifiedTypeName(parent);
        }
        return false;
    }

    private static string EmitRegistration(ClassInfo c, bool isComponent)
    {
        string macro = isComponent ? "ACS_REGISTER_COMPONENT" : "ACS_REGISTER_OBJECT";
        var props = c.Props.Select(p => EmitProp(c.Name, p)).Where(s => s != null).Select(s => s!).ToList();
        var sb = new StringBuilder();
        sb.Append($"{macro}({c.Name}");
        foreach (string p in props) sb.Append(",\n    " + p);
        sb.Append(")\n");
        // ACS_FUNCTION(BlueprintCallable/CallInEditor) void Name() → メソッド登録。
        foreach (var m in c.Methods)
            sb.Append($"ACS_REGISTER_METHOD({c.Name}, {m.Name}, {MethodFlagsFor(m.Specifiers)})\n");
        return sb.ToString();
    }

    // ACS_FUNCTION の指定子 → EMethodFlags 式。BlueprintCallable / CallInEditor を解釈する。
    private static string MethodFlagsFor(string spec)
    {
        string s = spec ?? "";
        var parts = new List<string>();
        if (Regex.IsMatch(s, @"\bBlueprintCallable\b")) parts.Add("::acs::game::METHOD_BP_CALLABLE");
        if (Regex.IsMatch(s, @"\bCallInEditor\b"))      parts.Add("::acs::game::METHOD_CALL_IN_EDITOR");
        return parts.Count == 0 ? "::acs::game::METHOD_NONE" : string.Join(" | ", parts);
    }

    // C++ 型 + 既定値 + UPROPERTY 指定子 → ACS_RFIELD_DF (offset + 既定値 + フラグ)。これにより
    // authored 値を実メンバへ適用でき (ReflectApply)、VisibleAnywhere 等の指定子がインスペクタに効く。
    // member は public であること。対応外の型は null (登録しない)。
    private static string? EmitProp(string cls, PropInfo p)
    {
        string t = p.Type.Replace("acs::", "").Replace("game::", "");
        string name = p.Name;
        string d = p.Default;
        string flagsExpr = FlagsFor(p.Specifiers);
        string catExpr = CategoryFor(p.Specifiers);   // Category="…" → "…" / nullptr
        bool isRef = Regex.IsMatch(p.Specifiers ?? "", @"\bObjectRef\b");   // 指定子で参照プロパティ化
        string Field(string kind, params string[] defs)
        {
            var args = new[] { cls, name, "::acs::game::EFieldKind::" + kind, flagsExpr, catExpr }.Concat(defs);
            return "ACS_RFIELD_DFC(" + string.Join(", ", args) + ")";
        }
        switch (t)
        {
            case "float": case "f32": case "double":
                return Field("F32", FloatLit(d, "0.0f"), "0", "0", "0");
            case "int": case "i32": case "u32": case "int32_t": case "uint32_t": case "unsigned":
                return Field(isRef ? "ObjectRef" : (t is "u32" or "uint32_t" or "unsigned" ? "U32" : "I32"),
                             IntLit(d, isRef ? "-1" : "0"), "0", "0", "0");
            case "bool":
                return Field("Bool", BoolLit(d), "0", "0", "0");
            case "FVec2": { var v = Vec(d, 2); return Field("FVec2", v[0], v[1], "0", "0"); }
            case "FVec3": { var v = Vec(d, 3); return Field("FVec3", v[0], v[1], v[2], "0"); }
            case "FVec4": { var v = Vec(d, 4); return Field("FVec4", v[0], v[1], v[2], v[3]); }
            default: return null;   // 未対応型はスキップ
        }
    }

    // ACS_PROPERTY の指定子文字列 → EFieldFlags 式。VisibleAnywhere/ReadOnly=READONLY、
    // Hidden=HIDDEN、Transient=TRANSIENT。EditAnywhere や指定なしは編集可 (FIELD_NONE)。
    // BlueprintReadWrite/ReadOnly/Callable・Category=... は受理して «今は» エディタフラグに影響させない
    // (将来の Blueprint/スクリプト層が使う)。
    private static string FlagsFor(string specifiers)
    {
        string s = specifiers ?? "";
        var parts = new List<string>();
        if (Regex.IsMatch(s, @"\b(VisibleAnywhere|VisibleDefaultsOnly|VisibleInstanceOnly|ReadOnly)\b"))
            parts.Add("::acs::game::FIELD_READONLY");
        if (Regex.IsMatch(s, @"\bHidden\b"))    parts.Add("::acs::game::FIELD_HIDDEN");
        if (Regex.IsMatch(s, @"\bTransient\b")) parts.Add("::acs::game::FIELD_TRANSIENT");
        return parts.Count == 0 ? "::acs::game::FIELD_NONE" : string.Join(" | ", parts);
    }

    // Category="Movement" / Category=Movement → C++ 文字列リテラル "Movement"。未指定は nullptr。
    private static string CategoryFor(string specifiers)
    {
        var m = Regex.Match(specifiers ?? "", "\\bCategory\\s*=\\s*\"?([\\w \\-/]+)\"?");
        return m.Success ? "\"" + m.Groups[1].Value.Trim() + "\"" : "nullptr";
    }

    private static string FloatLit(string d, string fallback)
    {
        if (string.IsNullOrWhiteSpace(d)) return fallback;
        d = d.Trim().TrimEnd('f', 'F');
        return double.TryParse(d, NumberStyles.Float, CultureInfo.InvariantCulture, out double v)
            ? v.ToString("0.0###", CultureInfo.InvariantCulture) + "f" : fallback;
    }
    private static string IntLit(string d, string fallback)
    {
        if (string.IsNullOrWhiteSpace(d)) return fallback;
        d = d.Trim().TrimEnd('u', 'U', 'l', 'L');
        return long.TryParse(d, out long v) ? v.ToString(CultureInfo.InvariantCulture) : fallback;
    }
    private static string BoolLit(string d) =>
        d.Trim().Equals("true", StringComparison.OrdinalIgnoreCase) ? "true" : "false";

    // "{1, 2}" / "FVec2{1.0f, 2.0f}" 等から数値を取り出す。足りない成分は 0。
    private static string[] Vec(string d, int n)
    {
        var nums = Regex.Matches(d ?? "", @"-?\d+(?:\.\d+)?")
            .Select(m => m.Value).ToList();
        var outv = new string[n];
        for (int i = 0; i < n; i++)
            outv[i] = (i < nums.Count ? double.Parse(nums[i], CultureInfo.InvariantCulture) : 0.0)
                        .ToString("0.0###", CultureInfo.InvariantCulture) + "f";
        return outv;
    }

    // 行コメント / ブロックコメントを除去 (マーカー誤検出を防ぐ)。文字列内は簡易に無視。
    private static string StripComments(string s)
    {
        s = Regex.Replace(s, @"/\*.*?\*/", " ", RegexOptions.Singleline);
        s = Regex.Replace(s, @"//[^\n]*", " ");
        return s;
    }
}
