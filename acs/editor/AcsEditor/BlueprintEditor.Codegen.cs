using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using System.Windows;
using System.Windows.Media;

namespace AcsEditor;

public partial class BlueprintEditor
{
    /// <summary>プロジェクトの Source/ ディレクトリ (MainWindow が設定)。設定時は «エンジンに組み込む» 生成先。</summary>
    public string? SourceDir { get; set; }
    /// <summary>生成後にエンジンビルドを要求する (MainWindow が「🔨 Build」へ束縛)。</summary>
    public Action? BuildRequested;

    // ===== BP → C++ コード生成 (グラフを ACS コンポーネントの .h/.cpp へ) =====
    /// <summary>C++ を生成して書き出す。プロジェクトが開いていれば Source/ へ置いてエンジンビルドへ繋ぐ。</summary>
    public string? GenerateCppFile(bool build)
    {
        string baseName = !string.IsNullOrEmpty(CurrentPath) ? System.IO.Path.GetFileNameWithoutExtension(CurrentPath!) : "GeneratedBp";
        string cls = "F" + SanitizeIdent(baseName);
        bool toSource = !string.IsNullOrEmpty(SourceDir) && System.IO.Directory.Exists(SourceDir);
        string dir = toSource ? SourceDir!
            : (!string.IsNullOrEmpty(CurrentPath) ? System.IO.Path.GetDirectoryName(CurrentPath!)!
            : (DefaultDir != null && System.IO.Directory.Exists(DefaultDir) ? DefaultDir : System.IO.Path.GetTempPath()));
        try
        {
            var (header, source) = GenerateCpp(cls);
            string hp = System.IO.Path.Combine(dir, cls + ".h");
            string cp = System.IO.Path.Combine(dir, cls + ".cpp");
            System.IO.File.WriteAllText(hp, header);
            System.IO.File.WriteAllText(cp, source);
            if (toSource)
            {
                LogSink?.Invoke($"⚙ C++ 生成 → Source/{cls}.h, {cls}.cpp (ACS_CLASS としてエンジンに組み込み)");
                if (build && BuildRequested != null) { LogSink?.Invoke("🔨 エンジンビルドを開始…"); BuildRequested.Invoke(); }
                else LogSink?.Invoke("「🔨 Build」でエンジンへ反映 (リフレクション登録 + コンパイル)。");
            }
            else LogSink?.Invoke($"⚙ C++ 生成: {cls}.h / {cls}.cpp → {dir} (スタンドアロン)");
            return cp;
        }
        catch (Exception ex) { LogSink?.Invoke("C++ 生成エラー: " + ex.Message); return null; }
    }

    private void OnGenerateCpp(object sender, RoutedEventArgs e) => GenerateCppFile(build: true);

    private static string SanitizeIdent(string s)
    {
        var sb = new StringBuilder();
        foreach (char c in s) sb.Append(char.IsLetterOrDigit(c) ? c : '_');
        string r = sb.ToString().Trim('_');
        if (r.Length == 0) r = "Bp";
        if (char.IsDigit(r[0])) r = "_" + r;
        return r;
    }
    private static string CppType(string t) => t switch
    {
        "Bool" => "bool", "Int" => "int", "Float" => "float",
        "String" => "acs::FString", "Vector" => "acs::FVec2", "Vector3" => "acs::FVec3", "Object" => "FNode2D*", _ => "float",
    };
    private static string CppDefault(string t, string v)
    {
        if (string.IsNullOrEmpty(v)) return t switch { "Bool" => "false", "String" => "{}", "Vector" => "{}", "Vector3" => "{}", "Object" => "nullptr", _ => "0" };
        return CppLiteral(v, t);
    }
    private static string CppLiteral(string lit, string type)
    {
        lit = (lit ?? "").Trim();
        switch (type)
        {
            case "Bool":   return Truthy(lit) ? "true" : "false";
            case "String": return "\"" + lit.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"";
            case "Vector": { var p = lit.Split(','); return $"acs::FVec2({(p.Length > 0 ? p[0].Trim() : "0")}f, {(p.Length > 1 ? p[1].Trim() : "0")}f)"; }
            case "Vector3": { var p = lit.Split(','); return $"acs::FVec3({(p.Length > 0 ? p[0].Trim() : "0")}f, {(p.Length > 1 ? p[1].Trim() : "0")}f, {(p.Length > 2 ? p[2].Trim() : "0")}f)"; }
            case "Object": return double.TryParse(lit, out _) ? lit : "nullptr";
            default:       return double.TryParse(lit, NumberStyles.Float, CultureInfo.InvariantCulture, out _) ? lit : "0";
        }
    }

    private void LoadGraphInto(string body) { _nodes.Clear(); _conns.Clear(); ParseGraph(body, 0, inherited: false); }

    private (string header, string source) GenerateCpp(string cls)
    {
        SyncActiveGraph();
        var saveN = new List<BpNode>(_nodes); var saveC = new List<BpConn>(_conns);
        var functions = _graphOrder.Where(g => g != EventGraphName && _graphIsFunc.Contains(g)).ToList();
        var h = new StringBuilder(); var s = new StringBuilder();
        try
        {
            // Event Graph をロードして event/custom メソッドの本体を作る。
            LoadGraphInto(_graphTexts.TryGetValue(EventGraphName, out var eg) ? eg : "");
            var beginNode = _nodes.FirstOrDefault(n => n.Title.Contains("BeginPlay"));
            var tickNode  = _nodes.FirstOrDefault(n => n.Title.Contains("On Tick"));
            var customs = _nodes.Where(n => n.Title == "Custom Event").Select(n => SanitizeIdent(LiteralOf(n, "name"))).Where(x => x.Length > 0).Distinct().ToList();

            // ----- ヘッダ (実 ACS API: FComponent2D / OnAttach / OnUpdate / ACS_GAME_COMPONENT_KIND) -----
            h.Append("// SPDX-License-Identifier: Apache-2.0\n// Generated from a Blueprint graph by AcsEditor — edit the .acsbp, then regenerate.\n#pragma once\n");
            h.Append("#include \"gameframework/Component2D.h\"\n#include \"gameframework/Node2D.h\"\n#include \"gameframework/AcsClass.h\"\n#include \"container/String.h\"\n#include \"math/Vec.h\"\n\n");
            h.Append("namespace acs::game {\n\n");
            h.Append("ACS_CLASS()\nclass ").Append(cls).Append(" : public FComponent2D {\npublic:\n");
            h.Append("    ACS_GAME_COMPONENT_KIND(").Append(cls).Append(")\n\n");
            foreach (var v in Variables)
                if (!string.IsNullOrWhiteSpace(v.Name))
                    h.Append("    ACS_PROPERTY() ").Append(CppType(v.Type)).Append(' ').Append(SanitizeIdent(v.Name))
                     .Append(" = ").Append(CppDefault(v.Type, v.Value)).Append(";\n");
            if (Variables.Any(v => !string.IsNullOrWhiteSpace(v.Name))) h.Append('\n');
            if (beginNode != null) h.Append("    void OnAttach(FNode2D& node) noexcept override;   // Event On BeginPlay\n");
            if (tickNode  != null) h.Append("    void OnUpdate(f32 dt) noexcept override;           // Event On Tick\n");
            if (customs.Count > 0 || functions.Count > 0)
            {
                h.Append("\nprivate:\n");
                foreach (var c in customs)   h.Append("    void ").Append(c).Append("() noexcept;\n");
                foreach (var f in functions)
                {
                    var (rt, pl, _) = CppFuncSig(f);
                    h.Append("    ").Append(rt).Append(' ').Append(SanitizeIdent(f)).Append('(').Append(pl).Append(") noexcept;   // Blueprint function\n");
                }
            }
            h.Append("};\n\n} // namespace acs::game\n");

            // ----- 実装 -----
            s.Append("// SPDX-License-Identifier: Apache-2.0\n// Generated from a Blueprint graph by AcsEditor.\n");
            s.Append("#include \"").Append(cls).Append(".h\"\n#include \"foundation/Log.h\"\n#include \"gameframework/Random.h\"\n#include <cmath>\n\nnamespace acs::game {\n\n");
            if (beginNode != null) EmitMethod(s, cls, "OnAttach", "FNode2D& node", beginNode);
            if (tickNode  != null) EmitMethod(s, cls, "OnUpdate", "f32 dt", tickNode);
            foreach (var ce in _nodes.Where(n => n.Title == "Custom Event"))
            {
                string nm = SanitizeIdent(LiteralOf(ce, "name"));
                if (nm.Length > 0) EmitMethod(s, cls, nm, "", ce);
            }
            // Function サブグラフ: 各グラフをロードして Function Entry から本体を生成 (型付き引数/戻り値)。
            foreach (var fn in functions)
            {
                var (rt, pl, _) = CppFuncSig(fn);
                LoadGraphInto(_graphTexts.TryGetValue(fn, out var fb) ? fb : "");
                EmitMethod(s, cls, SanitizeIdent(fn), pl, _nodes.FirstOrDefault(n => n.Title == "Function Entry"), rt);
            }
            s.Append("} // namespace acs::game\n");
        }
        finally { _nodes.Clear(); _nodes.AddRange(saveN); _conns.Clear(); _conns.AddRange(saveC); }
        return (h.ToString(), s.ToString());
    }

    /// <summary>関数の C++ シグネチャ (戻り型, 引数並び, 戻り値あり) を求める。引数=Function Entry の出力, 戻り=Return の入力。</summary>
    private (string retType, string paramList, bool hasRet) CppFuncSig(string fname)
    {
        var (args, rets) = FunctionSignature(fname);
        string rt = rets.Count > 0 ? CppType(rets[0].Type) : "void";
        string pl = string.Join(", ", args.Select(a => $"{CppType(a.Type)} {SanitizeIdent(a.Name)}"));
        return (rt, pl, rets.Count > 0);
    }

    private void EmitMethod(StringBuilder s, string cls, string method, string args, BpNode? eventNode, string retType = "void")
    {
        s.Append(retType).Append(' ').Append(cls).Append("::").Append(method).Append('(').Append(args).Append(") noexcept {\n");
        if (eventNode != null) GenStmt(s, ExecNextFirst(eventNode), "    ", new HashSet<int>(), 0);
        if (retType != "void") s.Append("    return ").Append(CppDefault(retType == "int" ? "Int" : retType == "bool" ? "Bool" : "Float", "")).Append(";\n");   // 既定の戻り (Return 未到達時)
        s.Append("}\n\n");
    }

    private string LiteralOf(BpNode n, string pinName)
    {
        int idx = n.Inputs.FindIndex(p => p.Name == pinName);
        return idx >= 0 && n.Literals.TryGetValue(idx, out var v) ? v : "";
    }
    private BpNode? ExecNext(BpNode n, string pinName)
    {
        int po = n.Outputs.FindIndex(p => p.Kind == PinKind.Exec && p.Name == pinName);
        if (po < 0) return null;
        foreach (var c in _conns) if (c.FromNode == n.Id && c.FromPin == po) return NodeById(c.ToNode);
        return null;
    }
    private BpNode? ExecNextFirst(BpNode? n)
    {
        if (n == null) return null;
        for (int po = 0; po < n.Outputs.Count; po++)
            if (n.Outputs[po].Kind == PinKind.Exec)
                foreach (var c in _conns) if (c.FromNode == n.Id && c.FromPin == po) return NodeById(c.ToNode);
        return null;
    }

    /// <summary>exec チェーンを C++ 文へ変換 (Branch=if/else, For Loop=for, Sequence=順次)。</summary>
    private void GenStmt(StringBuilder s, BpNode? n, string ind, HashSet<int> visited, int depth)
    {
        if (n == null || depth > 256 || !visited.Add(n.Id)) return;
        string t = n.Title;
        if (t == "Return")
        {
            int di = n.Inputs.FindIndex(p => p.Kind == PinKind.Data);
            if (di >= 0) s.Append(ind).Append("return ").Append(GenArg(n, n.Inputs[di].Name, 0)).Append(";\n");
            else s.Append(ind).Append("return;\n");
            return;
        }
        if (t == "Branch")
        {
            s.Append(ind).Append("if (").Append(GenArg(n, "cond", 0)).Append(") {\n");
            GenStmt(s, ExecNext(n, "True"), ind + "    ", new HashSet<int>(visited), depth + 1);
            s.Append(ind).Append("} else {\n");
            GenStmt(s, ExecNext(n, "False"), ind + "    ", new HashSet<int>(visited), depth + 1);
            s.Append(ind).Append("}\n");
            return;
        }
        if (t == "For Loop")
        {
            string iv = $"i{n.Id}";
            s.Append(ind).Append("for (int ").Append(iv).Append(" = ").Append(GenArg(n, "first", 0))
             .Append("; ").Append(iv).Append(" <= ").Append(GenArg(n, "last", 0)).Append("; ++").Append(iv).Append(") {\n");
            GenStmt(s, ExecNext(n, "Loop Body"), ind + "    ", new HashSet<int>(visited), depth + 1);
            s.Append(ind).Append("}\n");
            GenStmt(s, ExecNext(n, "Completed"), ind, visited, depth + 1);
            return;
        }
        if (t == "While")
        {
            s.Append(ind).Append("while (").Append(GenArg(n, "cond", 0)).Append(") {\n");
            GenStmt(s, ExecNext(n, "Loop Body"), ind + "    ", new HashSet<int>(visited), depth + 1);
            s.Append(ind).Append("}\n");
            GenStmt(s, ExecNext(n, "Completed"), ind, visited, depth + 1);
            return;
        }
        if (t == "Sequence")
        {
            for (int po = 0; po < n.Outputs.Count; po++)
                if (n.Outputs[po].Kind == PinKind.Exec)
                    foreach (var c in _conns) if (c.FromNode == n.Id && c.FromPin == po)
                        GenStmt(s, NodeById(c.ToNode), ind, new HashSet<int>(visited), depth + 1);
            return;
        }
        s.Append(ind).Append(GenNodeStmt(n)).Append('\n');
        GenStmt(s, ExecNextFirst(n), ind, visited, depth + 1);   // 線形ノードは次の exec を辿る
    }

    private string GenNodeStmt(BpNode n)
    {
        string t = n.Title;
        switch (t)
        {
            case "Set Variable":  return $"{SanitizeIdent(n.VarRef)} = {GenArg(n, "value", 0)};";
            case "Print String":  return GenLog(GenArg(n, "text", 0));
            // シーン操作 → 実 API (Owner().Local() の transform)。target は «自ノード» 前提でスケルトン化。
            case "Set Position":  return $"Owner().Local().position = FVec2{{ (f32)({GenArg(n, "x", 0)}), (f32)({GenArg(n, "y", 0)}) }};";
            case "Set Scale":     return $"Owner().Local().scale = FVec2{{ (f32)({GenArg(n, "sx", 0)}), (f32)({GenArg(n, "sy", 0)}) }};";
            case "Set Rotation":  return $"Owner().Local().rotation = (f32)({GenArg(n, "deg", 0)});";
            case "Call Function":
            {
                string fname = SanitizeIdent(LiteralOf(n, "name"));
                var argEx = new List<string>();
                for (int i = 2; i < n.Inputs.Count; i++) if (n.Inputs[i].Kind == PinKind.Data) argEx.Add(GenArg(n, n.Inputs[i].Name, 0));
                string call = $"{fname}({string.Join(", ", argEx)})";
                return n.Outputs.Any(p => p.Kind == PinKind.Data) ? $"auto _r{n.Id} = {call};" : $"{call};";
            }
            case "Delay":         return $"/* Delay({GenArg(n, "duration", 0)}s) — タイマー実装が必要 */";
            // 以下はエンジン API が未確定なのでコンパイル可能なコメントに留める。
            case "Set Color":     return $"/* Set Color (要 sprite component API) */";
            case "Set Visible":   return $"/* Set Visible (要 visibility API) */";
            case "Destroy":       return $"/* Destroy (要 scene API) */";
            case "Spawn Prefab":  return $"/* Spawn Prefab(\"{GenArg(n, "path", 0)}\") (要 spawn API) */";
            case "Publish Event": return $"/* Publish Event (要 message broker) */";
            case "Reparent":      return $"/* Reparent (要 scene API) */";
        }
        return $"/* {t} */";
    }

    /// <summary>Print String を ACS_LOG_INFO へ (文字列リテラルは %s、それ以外は %g キャスト)。</summary>
    private static string GenLog(string arg)
    {
        if (arg.StartsWith("\"")) return $"ACS_LOG_INFO(\"%s\", {arg});";
        return $"ACS_LOG_INFO(\"%g\", static_cast<double>({arg}));";
    }

    /// <summary>入力ピンを C++ 式へ (接続なら上流の式、無ければ定数)。</summary>
    private string GenArg(BpNode n, string pinName, int depth)
    {
        int idx = n.Inputs.FindIndex(p => p.Name == pinName);
        if (idx < 0) return "0";
        foreach (var c in _conns)
            if (c.ToNode == n.Id && c.ToPin == idx)
            {
                var f = NodeById(c.FromNode);
                if (f != null) return GenExpr(f, c.FromPin, depth + 1);
            }
        string lit = n.Literals.TryGetValue(idx, out var lv) ? lv : "";
        return CppLiteral(lit, n.Inputs[idx].Type);
    }

    /// <summary>data 出力ピンを C++ 式へ (演算/変数/Vector/比較などを再帰展開)。</summary>
    private string GenExpr(BpNode n, int outPin, int depth)
    {
        if (depth > 64) return "0";
        switch (n.Title)
        {
            case "Add":      return $"({GenArg(n, "a", depth)} + {GenArg(n, "b", depth)})";
            case "Subtract": return $"({GenArg(n, "a", depth)} - {GenArg(n, "b", depth)})";
            case "Multiply": return $"({GenArg(n, "a", depth)} * {GenArg(n, "b", depth)})";
            case "Divide":   return $"({GenArg(n, "a", depth)} / {GenArg(n, "b", depth)})";
            case "Modulo":   return $"((int)({GenArg(n, "a", depth)}) % (int)({GenArg(n, "b", depth)}))";
            case "And":      return $"({GenArg(n, "a", depth)} && {GenArg(n, "b", depth)})";
            case "Or":       return $"({GenArg(n, "a", depth)} || {GenArg(n, "b", depth)})";
            case "Not":      return $"(!{GenArg(n, "in", depth)})";
            case "Compare":  return $"({GenArg(n, "a", depth)} {OpLiteral(n)} {GenArg(n, "b", depth)})";
            case "Make Vector": return $"FVec2{{ (f32)({GenArg(n, "x", depth)}), (f32)({GenArg(n, "y", depth)}) }}";
            case "Get Variable": return SanitizeIdent(n.VarRef);
            case "Get Self":  return "(&Owner())";
            case "Get Position": return "Owner().Local().position";
            case "Function Entry": return outPin >= 1 && outPin < n.Outputs.Count ? SanitizeIdent(n.Outputs[outPin].Name) : "0";   // 引数
            case "Call Function":  return $"_r{n.Id}";   // 呼出結果
            case "Reroute":   return GenArg(n, "in", depth);
            case "To Float":  return $"(float)({GenArg(n, "in", depth)})";
            case "To Int":    return $"(int)({GenArg(n, "in", depth)})";
            case "To Bool":   return $"(bool)({GenArg(n, "in", depth)})";
            case "To String": return GenArg(n, "in", depth);
            case "For Loop":  return $"i{n.Id}";   // index 出力 = ループ変数
            // 数学 (std math 関数 / 三項式)。
            case "Abs":    return $"std::fabs((float)({GenArg(n, "value", depth)}))";
            case "Negate": return $"(-({GenArg(n, "value", depth)}))";
            case "Sqrt":   return $"std::sqrt((float)({GenArg(n, "value", depth)}))";
            case "Floor":  return $"(int)std::floor((float)({GenArg(n, "value", depth)}))";
            case "Ceil":   return $"(int)std::ceil((float)({GenArg(n, "value", depth)}))";
            case "Round":  return $"(int)std::lround((float)({GenArg(n, "value", depth)}))";
            case "Sin":    return $"std::sin((float)({GenArg(n, "value", depth)}))";
            case "Cos":    return $"std::cos((float)({GenArg(n, "value", depth)}))";
            case "Power":  return $"std::pow((float)({GenArg(n, "base", depth)}), (float)({GenArg(n, "exp", depth)}))";
            case "Sign":   { var v = GenArg(n, "value", depth); return $"(({v}) > 0 ? 1.0f : (({v}) < 0 ? -1.0f : 0.0f))"; }
            case "Min":    { var a = GenArg(n, "a", depth); var b = GenArg(n, "b", depth); return $"(({a}) < ({b}) ? ({a}) : ({b}))"; }
            case "Max":    { var a = GenArg(n, "a", depth); var b = GenArg(n, "b", depth); return $"(({a}) > ({b}) ? ({a}) : ({b}))"; }
            case "Clamp":  { var v = GenArg(n, "value", depth); var lo = GenArg(n, "min", depth); var hi = GenArg(n, "max", depth); return $"(({v}) < ({lo}) ? ({lo}) : (({v}) > ({hi}) ? ({hi}) : ({v})))"; }
            case "Lerp":   { var a = GenArg(n, "a", depth); var b = GenArg(n, "b", depth); var tt = GenArg(n, "t", depth); return $"(({a}) + (({b}) - ({a})) * ({tt}))"; }
            // 比較 (各演算子)。
            case "Greater":       return $"(({GenArg(n, "a", depth)}) >  ({GenArg(n, "b", depth)}))";
            case "Less":          return $"(({GenArg(n, "a", depth)}) <  ({GenArg(n, "b", depth)}))";
            case "Greater Equal": return $"(({GenArg(n, "a", depth)}) >= ({GenArg(n, "b", depth)}))";
            case "Less Equal":    return $"(({GenArg(n, "a", depth)}) <= ({GenArg(n, "b", depth)}))";
            case "Equal":         return $"(({GenArg(n, "a", depth)}) == ({GenArg(n, "b", depth)}))";
            case "Not Equal":     return $"(({GenArg(n, "a", depth)}) != ({GenArg(n, "b", depth)}))";
            // 数学 追加。
            case "Tan":    return $"std::tan((float)({GenArg(n, "value", depth)}))";
            case "Atan2":  return $"std::atan2((float)({GenArg(n, "y", depth)}), (float)({GenArg(n, "x", depth)}))";
            case "Exp":    return $"std::exp((float)({GenArg(n, "value", depth)}))";
            case "Log":    { var v = GenArg(n, "value", depth); return $"(((float)({v}) <= 0.0f) ? 0.0f : std::log((float)({v})))"; }   // 定義域ガード (interpreter と一致)
            case "Deg To Rad": return $"((float)({GenArg(n, "deg", depth)}) * 0.01745329252f)";
            case "Rad To Deg": return $"((float)({GenArg(n, "rad", depth)}) * 57.2957795131f)";
            case "Map Range": { var v = GenArg(n, "value", depth); var i0 = GenArg(n, "inMin", depth); var i1 = GenArg(n, "inMax", depth); var o0 = GenArg(n, "outMin", depth); var o1 = GenArg(n, "outMax", depth); return $"(((float)(({i1}) - ({i0})) == 0.0f) ? (float)({o0}) : (({o0}) + (({o1}) - ({o0})) * ((float)(({v}) - ({i0})) / (float)(({i1}) - ({i0})))))"; }   // 0 除算ガード
            case "Wrap": { var v = GenArg(n, "value", depth); var lo = GenArg(n, "min", depth); var hi = GenArg(n, "max", depth); return $"((((float)({hi}) - (float)({lo})) <= 0.0f) ? (float)({lo}) : ((float)({lo}) + std::fmod(std::fmod((float)({v}) - (float)({lo}), (float)({hi}) - (float)({lo})) + ((float)({hi}) - (float)({lo})), (float)({hi}) - (float)({lo}))))"; }
            case "PingPong": { var t = GenArg(n, "t", depth); var len = GenArg(n, "length", depth); return $"([&]{{ float _L = (float)({len}); if (_L <= 0.0f) return 0.0f; float _m = std::fmod(std::fmod((float)({t}), 2.0f*_L) + 2.0f*_L, 2.0f*_L); return _m <= _L ? _m : 2.0f*_L - _m; }}())"; }
            case "Move Towards": { var c = GenArg(n, "current", depth); var t = GenArg(n, "target", depth); var s = GenArg(n, "step", depth); return $"(std::fabs((float)(({t}) - ({c}))) <= (float)({s}) ? (float)({t}) : (float)({c}) + ((({t}) > ({c})) ? (float)({s}) : -(float)({s})))"; }
            case "SmoothStep": { var a = GenArg(n, "a", depth); var b = GenArg(n, "b", depth); var tt = GenArg(n, "t", depth); return $"(({a}) + (({b}) - ({a})) * (((float)({tt}) * (float)({tt})) * (3.0f - 2.0f * (float)({tt}))))"; }
            // ベクトル (FVec2 = «Vector»、成分ごとに構築。FVec3 は «Vector3»)。
            case "Make Vector3": return $"acs::FVec3((float)({GenArg(n, "x", depth)}), (float)({GenArg(n, "y", depth)}), (float)({GenArg(n, "z", depth)}))";
            case "Break Vector3": { var v = GenArg(n, "in", depth); return $"(({v}).{(outPin == 0 ? "x" : outPin == 1 ? "y" : "z")})"; }
            case "Break Vector":  { var v = GenArg(n, "in", depth); return $"(({v}).{(outPin == 0 ? "x" : "y")})"; }
            case "To Vector3": { var v = GenArg(n, "in", depth); return $"acs::FVec3(({v}).x, ({v}).y, 0.0f)"; }
            case "To Vector2": { var v = GenArg(n, "in", depth); return $"acs::FVec2(({v}).x, ({v}).y)"; }
            case "Vector Add":      { var a = GenArg(n, "a", depth); var b = GenArg(n, "b", depth); return $"acs::FVec2(({a}).x + ({b}).x, ({a}).y + ({b}).y)"; }
            case "Vector Subtract": { var a = GenArg(n, "a", depth); var b = GenArg(n, "b", depth); return $"acs::FVec2(({a}).x - ({b}).x, ({a}).y - ({b}).y)"; }
            case "Vector Scale":    { var v = GenArg(n, "v", depth); var s = GenArg(n, "s", depth); return $"acs::FVec2(({v}).x * (float)({s}), ({v}).y * (float)({s}))"; }
            case "Vector Length":   { var v = GenArg(n, "v", depth); return $"std::sqrt(({v}).x * ({v}).x + ({v}).y * ({v}).y)"; }
            case "Vector Distance": { var a = GenArg(n, "a", depth); var b = GenArg(n, "b", depth); return $"std::sqrt((({a}).x - ({b}).x) * (({a}).x - ({b}).x) + (({a}).y - ({b}).y) * (({a}).y - ({b}).y))"; }
            case "Vector Dot":      { var a = GenArg(n, "a", depth); var b = GenArg(n, "b", depth); return $"(({a}).x * ({b}).x + ({a}).y * ({b}).y)"; }
            case "Vector Normalize": { var v = GenArg(n, "v", depth); return $"([&]{{ auto _v = ({v}); float _l = std::sqrt(_v.x*_v.x + _v.y*_v.y); return _l > 0.0f ? acs::FVec2(_v.x/_l, _v.y/_l) : acs::FVec2(0.0f, 0.0f); }}())"; }
            // 乱数 (エンジンの FRandom)。
            case "Random Float": return $"acs::game::FRandom::Global().RangeF32((f32)({GenArg(n, "min", depth)}), (f32)({GenArg(n, "max", depth)}))";
            case "Random Int":   return $"acs::game::FRandom::Global().RangeInt((i32)({GenArg(n, "min", depth)}), (i32)({GenArg(n, "max", depth)}))";
            case "Random Bool":  return "acs::game::FRandom::Global().NextBool()";
            // 時間。
            case "Get Delta Time": return "dt";
            case "Get Time":       return "0.0f";   // 時刻アクセサは未配線 (interpreter も 0)
        }
        if (n.Title.StartsWith("Spawn")) return $"spawned{n.Id}";
        return $"/*{SanitizeIdent(n.Title)}*/0";
    }

    private string OpLiteral(BpNode n)
    {
        string op = LiteralOf(n, "op").Trim();
        return op.Length == 0 ? "==" : op;
    }
}
