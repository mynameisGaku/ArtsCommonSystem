// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// acsbuild — ACS Build Tool (UE の UnrealBuildTool 相当の薄い版)
// -----------------------------------------------------------------------------
// 各 src/<mod>/<Name>.Build.cs (AcsModule 派生) を本ツールへコンパイル取り込みし、
// reflection で全モジュールを発見 → ディレクトリを走査してソース/ヘッダを自動収集 →
// CMake の Module.cmake を生成する。CMake はそのまま裏方のビルドバックエンドとして残す。
//
//   dotnet run --project tools/acsbuild -- gen            … 全モジュールの Module.cmake を生成
//   dotnet run --project tools/acsbuild -- gen --module Event
//   dotnet run --project tools/acsbuild -- --check        … 生成結果と既存を突き合わせ (上書きしない)
//   オプション: --root <repo>  (既定: src/ と engine/ を持つ親を自動探索)
// =============================================================================
using System.Reflection;
using System.Text;
using Acs.Build;

string? root = FindRoot(args);
if (root is null) { Console.Error.WriteLine("error: repo root not found (pass --root <path>)"); return 2; }

bool check = args.Contains("--check");
string? only = GetArg(args, "--module");
string srcRoot = Path.Combine(root, "src");

var modules = Assembly.GetExecutingAssembly().GetTypes()
    .Where(t => typeof(AcsModule).IsAssignableFrom(t) && !t.IsAbstract)
    .Select(t => (AcsModule)Activator.CreateInstance(t)!)
    .Where(m => only is null || string.Equals(m.Name, only, StringComparison.OrdinalIgnoreCase))
    .OrderBy(m => m.Name, StringComparer.Ordinal)
    .ToList();

if (modules.Count == 0) { Console.Error.WriteLine("error: no *.Build.cs modules found"); return 2; }

Console.WriteLine($"acsbuild: {modules.Count} module(s), root={root}, mode={(check ? "check" : "gen")}");
int problems = 0;

foreach (var m in modules)
{
    string dir = Path.Combine(srcRoot, m.Name.ToLowerInvariant());
    if (!Directory.Exists(dir)) { Console.Error.WriteLine($"  MISS {m.Name,-16} dir not found: {dir}"); problems++; continue; }

    var r = Collect(dir, m);
    string outPath = Path.Combine(dir, "Module.cmake");

    if (check)
    {
        if (m.Conditionals.Count > 0)
        {
            // 組み立て形式は既存も ${var} を使うため厳密比較は省略 (ビルドで検証)。
            Console.WriteLine($"  ASM  {m.Name,-16} (assembled: {r.Conds.Count} conditional group(s) — validated by build)");
        }
        else
        {
            var (ok, detail) = Compare(outPath, m, r.Sources, r.Headers);
            Console.WriteLine($"  {(ok ? "OK  " : "DIFF")} {m.Name,-16} {detail}");
            if (!ok) problems++;
        }
    }
    else
    {
        File.WriteAllText(outPath, Emit(m, r));
        string kind = m.Conditionals.Count > 0 ? "asm" : "gen";
        Console.WriteLine($"  {kind}  {m.Name,-16} -> {Rel(root, outPath)}  ({r.Sources.Count} src, {r.Headers.Count} hdr, {m.Features.Count} feat)");
    }
}

Console.WriteLine(problems == 0 ? "acsbuild: OK" : $"acsbuild: {problems} problem(s)");
return problems == 0 ? 0 : 1;

// ----- 収集 ------------------------------------------------------------------

static Resolved Collect(string dir, AcsModule m)
{
    string RelTo(string f) => Path.GetRelativePath(dir, f).Replace('\\', '/');
    var excl = new HashSet<string>(m.ExcludeFiles, StringComparer.OrdinalIgnoreCase);

    // 条件付きサブディレクトリは base 収集から除外する (条件側で追加するため)。
    var condSubdirs = m.Conditionals.Where(c => c.SubdirSources != null)
        .Select(c => c.SubdirSources!.TrimEnd('/').Replace('\\', '/') + "/").ToList();
    bool UnderCond(string rel) => condSubdirs.Any(s => rel.StartsWith(s, StringComparison.OrdinalIgnoreCase));

    List<string> Glob(string pattern) => Directory.EnumerateFiles(dir, pattern, SearchOption.AllDirectories)
        .Select(RelTo).Where(r => !excl.Contains(r) && !UnderCond(r)).ToList();

    var sources = Glob("*.cpp");
    var headers = Glob("*.h");
    foreach (var e in m.ExtraFiles)
    {
        if (e.EndsWith(".cpp", StringComparison.OrdinalIgnoreCase)) sources.Add(e);
        else if (e.EndsWith(".h", StringComparison.OrdinalIgnoreCase)) headers.Add(e);
    }
    sources.Sort(StringComparer.Ordinal);
    headers.Sort(StringComparer.Ordinal);

    var conds = new List<ResolvedCond>();
    foreach (var c in m.Conditionals)
    {
        var cs = new List<string>(c.Sources);
        if (c.SubdirSources != null)
        {
            string sub = Path.Combine(dir, c.SubdirSources);
            if (Directory.Exists(sub))
                cs.AddRange(Directory.EnumerateFiles(sub, "*.cpp", SearchOption.AllDirectories).Select(RelTo));
        }
        cs.Sort(StringComparer.Ordinal);
        var ch = new List<string>(c.Headers); ch.Sort(StringComparer.Ordinal);
        conds.Add(new ResolvedCond(c.Condition, cs, ch, c.Deps, c.LibsPublic, c.LibsPrivate));
    }
    return new Resolved(sources, headers, conds);
}

// ----- 出力 ------------------------------------------------------------------

static string Emit(AcsModule m, Resolved r)
{
    var sb = new StringBuilder();
    sb.Append("# ============================================================================\n");
    sb.Append($"# AUTO-GENERATED by acsbuild from {m.Name}.Build.cs — DO NOT EDIT BY HAND.\n");
    sb.Append($"# Edit src/{m.Name.ToLowerInvariant()}/{m.Name}.Build.cs, then run:\n");
    sb.Append("#   dotnet run --project tools/acsbuild -- gen\n");
    sb.Append("# ============================================================================\n");

    if (!string.IsNullOrWhiteSpace(m.Guard))
        sb.Append($"if(NOT {m.Guard})\n    return()\nendif()\n\n");

    if (!string.IsNullOrWhiteSpace(m.Preamble))
        sb.Append(m.Preamble!.Replace("\r\n", "\n").TrimEnd('\n')).Append("\n\n");

    if (m.Conditionals.Count == 0) EmitSimple(sb, m, r);
    else EmitAssembled(sb, m, r);

    foreach (var f in m.Features)
    {
        sb.Append('\n');
        sb.Append($"acs_module_feature(MODULE {m.Name} NAME {f.Name}\n");
        sb.Append($"    DEFINE {f.Define}\n");
        if (!string.IsNullOrEmpty(f.Description)) sb.Append($"    DESCRIPTION \"{f.Description}\"\n");
        sb.Append($"    DEFAULT {f.DefaultExpr ?? (f.Default ? "ON" : "OFF")})\n");
    }
    return sb.ToString();
}

static void EmitSimple(StringBuilder sb, AcsModule m, Resolved r)
{
    sb.Append("acs_module(\n");
    sb.Append($"    NAME    {m.Name}\n");
    sb.Append($"    TYPE    {m.Type}\n");
    AppendList(sb, "SOURCES", r.Sources);
    AppendList(sb, "HEADERS", r.Headers);
    AppendList(sb, "PUBLIC_DEPS", m.PublicDeps);
    AppendList(sb, "PRIVATE_DEPS", m.PrivateDeps);
    AppendList(sb, "LINK_PUBLIC", m.PublicLibs);
    AppendList(sb, "LINK_PRIVATE", m.PrivateLibs);
    sb.Append(")\n");
}

static void EmitAssembled(StringBuilder sb, AcsModule m, Resolved r)
{
    string v = "_acsgen_" + m.Name.ToLowerInvariant();
    bool useDeps = m.PublicDeps.Count > 0 || r.Conds.Any(c => c.Deps.Count > 0);
    bool usePrivDeps = m.PrivateDeps.Count > 0;
    bool useLpub = m.PublicLibs.Count > 0 || r.Conds.Any(c => c.LibsPublic.Count > 0);
    bool useLpriv = m.PrivateLibs.Count > 0 || r.Conds.Any(c => c.LibsPrivate.Count > 0);

    EmitSet(sb, $"{v}_sources", r.Sources);
    EmitSet(sb, $"{v}_headers", r.Headers);
    if (useDeps) EmitSet(sb, $"{v}_public_deps", m.PublicDeps);
    if (usePrivDeps) EmitSet(sb, $"{v}_private_deps", m.PrivateDeps);
    if (useLpub) EmitSet(sb, $"{v}_link_public", m.PublicLibs);
    if (useLpriv) EmitSet(sb, $"{v}_link_private", m.PrivateLibs);

    foreach (var c in r.Conds)
    {
        sb.Append($"if({c.Condition})\n");
        AppendListAppend(sb, $"{v}_sources", c.Sources);
        AppendListAppend(sb, $"{v}_headers", c.Headers);
        if (useDeps) AppendListAppend(sb, $"{v}_public_deps", c.Deps);
        if (useLpub) AppendListAppend(sb, $"{v}_link_public", c.LibsPublic);
        if (useLpriv) AppendListAppend(sb, $"{v}_link_private", c.LibsPrivate);
        sb.Append("endif()\n");
    }

    sb.Append("\nacs_module(\n");
    sb.Append($"    NAME    {m.Name}\n");
    sb.Append($"    TYPE    {m.Type}\n");
    sb.Append($"    SOURCES ${{{v}_sources}}\n");
    sb.Append($"    HEADERS ${{{v}_headers}}\n");
    if (useDeps) sb.Append($"    PUBLIC_DEPS ${{{v}_public_deps}}\n");
    if (usePrivDeps) sb.Append($"    PRIVATE_DEPS ${{{v}_private_deps}}\n");
    if (useLpub) sb.Append($"    LINK_PUBLIC ${{{v}_link_public}}\n");
    if (useLpriv) sb.Append($"    LINK_PRIVATE ${{{v}_link_private}}\n");
    sb.Append(")\n");
}

static void EmitSet(StringBuilder sb, string var, List<string> items)
{
    if (items.Count == 0) { sb.Append($"set({var})\n"); return; }
    sb.Append($"set({var}\n");
    foreach (var it in items) sb.Append($"    {it}\n");
    sb.Append(")\n");
}

static void AppendListAppend(StringBuilder sb, string var, List<string> items)
{
    if (items.Count == 0) return;
    sb.Append($"    list(APPEND {var}\n");
    foreach (var it in items) sb.Append($"        {it}\n");
    sb.Append("    )\n");
}

static void AppendList(StringBuilder sb, string keyword, List<string> items)
{
    if (items.Count == 0) return;
    sb.Append($"    {keyword}\n");
    foreach (var it in items) sb.Append($"        {it}\n");
}

// ----- 検証 (--check、単純形式のみ) ------------------------------------------

static (bool ok, string detail) Compare(string existingPath, AcsModule m, List<string> sources, List<string> headers)
{
    if (!File.Exists(existingPath)) return (false, "(no existing Module.cmake)");
    var buckets = ParseModuleCmake(File.ReadAllText(existingPath));
    var msgs = new List<string>();

    void Cmp(string key, IEnumerable<string> generated)
    {
        var have = buckets.TryGetValue(key, out var val) ? val : new List<string>();
        var a = new HashSet<string>(have, StringComparer.Ordinal);
        var b = new HashSet<string>(generated, StringComparer.Ordinal);
        if (!a.SetEquals(b))
        {
            var missing = b.Except(a).ToList();
            var extra = a.Except(b).ToList();
            var parts = new List<string>();
            if (missing.Count > 0) parts.Add("+[" + string.Join(",", missing) + "]");
            if (extra.Count > 0) parts.Add("-[" + string.Join(",", extra) + "]");
            msgs.Add($"{key} {string.Join(" ", parts)}");
        }
    }

    Cmp("SOURCES", sources);
    Cmp("HEADERS", headers);
    Cmp("PUBLIC_DEPS", m.PublicDeps);
    Cmp("PRIVATE_DEPS", m.PrivateDeps);
    Cmp("LINK_PUBLIC", m.PublicLibs);
    Cmp("LINK_PRIVATE", m.PrivateLibs);

    return msgs.Count == 0
        ? (true, $"{sources.Count} src, {headers.Count} hdr, {m.Features.Count} feat")
        : (false, string.Join(" | ", msgs));
}

static Dictionary<string, List<string>> ParseModuleCmake(string text)
{
    var result = new Dictionary<string, List<string>>();
    var stripped = string.Join(" ", text.Split('\n').Select(l =>
    {
        int h = l.IndexOf('#');
        return h >= 0 ? l[..h] : l;
    }));

    int call = stripped.IndexOf("acs_module(", StringComparison.Ordinal);
    if (call < 0) return result;
    int start = stripped.IndexOf('(', call);
    int depth = 0, end = -1;
    for (int p = start; p < stripped.Length; p++)
    {
        if (stripped[p] == '(') depth++;
        else if (stripped[p] == ')') { if (--depth == 0) { end = p; break; } }
    }
    if (end < 0) return result;

    var keywords = new HashSet<string>
    { "NAME", "TYPE", "SOURCES", "HEADERS", "PUBLIC_DEPS", "PRIVATE_DEPS", "LINK_PUBLIC", "LINK_PRIVATE" };
    string cur = "";
    foreach (var t in stripped.Substring(start + 1, end - start - 1)
                              .Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries))
    {
        if (keywords.Contains(t)) { cur = t; if (!result.ContainsKey(cur)) result[cur] = new List<string>(); }
        else if (cur.Length > 0) result[cur].Add(t);
    }
    return result;
}

// ----- 雑多 ------------------------------------------------------------------

static string Rel(string root, string path) => Path.GetRelativePath(root, path).Replace('\\', '/');

static string? GetArg(string[] a, string key)
{
    int i = Array.IndexOf(a, key);
    return (i >= 0 && i + 1 < a.Length) ? a[i + 1] : null;
}

static string? FindRoot(string[] a)
{
    string? explicitRoot = GetArg(a, "--root");
    if (explicitRoot is not null) return Path.GetFullPath(explicitRoot);
    var dir = new DirectoryInfo(Directory.GetCurrentDirectory());
    while (dir is not null)
    {
        if (Directory.Exists(Path.Combine(dir.FullName, "src")) &&
            Directory.Exists(Path.Combine(dir.FullName, "engine")))
            return dir.FullName;
        dir = dir.Parent;
    }
    return null;
}

record Resolved(List<string> Sources, List<string> Headers, List<ResolvedCond> Conds);
record ResolvedCond(string Condition, List<string> Sources, List<string> Headers,
                    List<string> Deps, List<string> LibsPublic, List<string> LibsPrivate);
