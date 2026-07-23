using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace AcsEditor;

/// <summary>プロジェクトの新規作成・オープン・最近使った一覧。フォルダ生成とテンプレート展開を担う。</summary>
public static partial class ProjectManager
{
    // マニフェスト JSON の (デ)シリアライズ用 DTO (Project の計算プロパティを書き出さないため分離)。
    private sealed class ManifestDto
    {
        public int    version       { get; set; } = 1;
        public string name          { get; set; } = "";
        public string engineVersion { get; set; } = "";
        public string template      { get; set; } = "blank";
        public string initialScene  { get; set; } = "Assets/main.acscene";
        public string canonicalSceneAssetId { get; set; } = "";
    }

    private static readonly JsonSerializerOptions JsonOpts = new() { WriteIndented = true };
    private static readonly UTF8Encoding Utf8NoBom = new(false);

    /// <summary>新規プロジェクトを &lt;parentDir&gt;/&lt;name&gt;/ に生成する。template は "blank" | "2d"。</summary>
    public static Project CreateNew(string name, string parentDir, string template)
    {
        if (string.IsNullOrWhiteSpace(name))      throw new ArgumentException("プロジェクト名が空です。");
        if (string.IsNullOrWhiteSpace(parentDir)) throw new ArgumentException("作成先フォルダが空です。");
        name = name.Trim();
        if (name.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)
            throw new ArgumentException("プロジェクト名に使用できない文字が含まれています。");

        string rootDir = Path.Combine(parentDir, name);
        if (Directory.Exists(rootDir) && Directory.EnumerateFileSystemEntries(rootDir).Any())
            throw new IOException($"フォルダが既に存在し空ではありません: {rootDir}");

        Directory.CreateDirectory(rootDir);
        SceneSourceFile.ValidateProjectRootDirectory(rootDir);
        Directory.CreateDirectory(Path.Combine(rootDir, "Assets"));

        bool is2d = string.Equals(template, "2d", StringComparison.OrdinalIgnoreCase);

        // 初期シーン (エディタ表示用 ACSCENE)。
        string assetsDir = Path.Combine(rootDir, "Assets");
        string initialScenePath = SceneSourceFile.ResolveProjectSceneReference(
            rootDir,
            assetsDir,
            "Assets/main.acscene",
            SceneDocumentMode.TwoD);
        Directory.CreateDirectory(Path.Combine(rootDir, "Source"));
        Directory.CreateDirectory(Path.Combine(rootDir, "Temp"));
        SceneSourceFile.WriteProjectSceneAtomicText(
            initialScenePath,
            is2d ? Template2DScene : BlankScene,
            rootDir,
            assetsDir,
            SceneDocumentMode.TwoD);

        // ゲームソース (スタンドアロン実行 = ACS_GAME_MAIN)。blank/2d とも «エディタで編集した
        // main.acscene を読み込む» 共通ソースを置く (Build & Run = 編集中シーンが立ち上がる)。
        File.WriteAllText(Path.Combine(rootDir, "Source", "Game.cpp"),
                          SceneLoaderSource, Utf8NoBom);
        // ビルド定義: エンジンが add_subdirectory で取り込み、ACS::GameFramework をリンクする。
        File.WriteAllText(Path.Combine(rootDir, "Source", "CMakeLists.txt"),
                          CMakeTemplate(SanitizeIdent(name), rootDir), Utf8NoBom);
        // プロジェクトの DLL エクスポートマクロ <IDENT>_API を定義するヘッダ。
        File.WriteAllText(Path.Combine(rootDir, "Source", ApiHeaderName(name)),
                          ApiHeaderContent(SanitizeIdent(name)), Utf8NoBom);

        var proj = new Project
        {
            Version = 1,
            Name = name,
            EngineVersion = EngineInterop.Version(),
            Template = is2d ? "2d" : "blank",
            InitialScene = SceneSourceFile.NormalizeProjectSceneReference(
                rootDir,
                assetsDir,
                "Assets/main.acscene",
                SceneDocumentMode.TwoD),
            ProjectFilePath = Path.Combine(rootDir, name + ".acsproject"),
        };
        var assetDatabase = AssetDatabase.ForProject(proj);
        assetDatabase.Refresh(verifyContent: true);
        if (!assetDatabase.TryGetByPath(initialScenePath, out AssetRecord? canonicalScene) ||
            canonicalScene == null)
        {
            throw new InvalidDataException(
                "The initial scene could not be assigned a persistent Asset ID.");
        }
        proj.CanonicalSceneAssetId = canonicalScene.AssetId;
        WriteManifest(proj);
        AddRecent(proj.ProjectFilePath);
        return proj;
    }

    /// <summary>.acsproject マニフェストを開いて Project を返す。</summary>
    public static Project Open(string acsprojectPath)
    {
        Project project = ReadManifest(acsprojectPath);
        AddRecent(project.ProjectFilePath);
        return project;
    }

    /// <summary>
    /// Reads and validates a project manifest without mutating the recent-project list.
    /// </summary>
    internal static Project ReadManifest(string acsprojectPath)
    {
        string projectFilePath = Path.GetFullPath(acsprojectPath);
        if (!File.Exists(projectFilePath))
            throw new FileNotFoundException($"プロジェクトファイルが見つかりません: {projectFilePath}");
        FileAttributes projectFileAttributes = File.GetAttributes(projectFilePath);
        if ((projectFileAttributes & FileAttributes.Directory) != 0)
            throw new InvalidDataException(
                $".acsproject path must be an ordinary file: {projectFilePath}");
        if ((projectFileAttributes & FileAttributes.ReparsePoint) != 0)
            throw new InvalidDataException(
                $".acsproject file cannot be a reparse point: {projectFilePath}");
        string projectRoot = Path.GetDirectoryName(projectFilePath)
            ?? throw new InvalidDataException(".acsproject path has no project directory.");
        SceneSourceFile.ValidateProjectRootDirectory(projectRoot);
        string json = File.ReadAllText(projectFilePath, Encoding.UTF8);
        var dto = JsonSerializer.Deserialize<ManifestDto>(json)
                  ?? throw new InvalidDataException("マニフェストの解析に失敗しました。");
        var proj = new Project
        {
            Version = dto.version,
            Name = string.IsNullOrEmpty(dto.name) ? Path.GetFileNameWithoutExtension(projectFilePath) : dto.name,
            EngineVersion = dto.engineVersion,
            Template = dto.template,
            InitialScene = string.IsNullOrEmpty(dto.initialScene) ? "Assets/main.acscene" : dto.initialScene,
            CanonicalSceneAssetId = dto.canonicalSceneAssetId ?? "",
            ProjectFilePath = projectFilePath,
        };
        proj.InitialScene = SceneSourceFile.NormalizeProjectSceneReference(
            proj.RootDir,
            proj.AssetsDir,
            proj.InitialScene);
        return proj;
    }

    private static void WriteManifest(Project p)
    {
        p.InitialScene = SceneSourceFile.NormalizeProjectSceneReference(
            p.RootDir,
            p.AssetsDir,
            p.InitialScene);
        var dto = new ManifestDto
        {
            version = p.Version, name = p.Name, engineVersion = p.EngineVersion,
            template = p.Template, initialScene = p.InitialScene,
            canonicalSceneAssetId = p.CanonicalSceneAssetId,
        };
        File.WriteAllText(p.ProjectFilePath, JsonSerializer.Serialize(dto, JsonOpts), Utf8NoBom);
    }

    // ===== 最近使ったプロジェクト (%APPDATA%/AcsEditor/recents.txt、新しい順) =====
    private static string RecentsFile =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "AcsEditor", "recents.txt");

    public static List<string> GetRecents()
    {
        try
        {
            if (!File.Exists(RecentsFile)) return new List<string>();
            return File.ReadAllLines(RecentsFile)
                       .Where(l => !string.IsNullOrWhiteSpace(l) && File.Exists(l))
                       .Distinct(StringComparer.OrdinalIgnoreCase)
                       .Take(10).ToList();
        }
        catch { return new List<string>(); }
    }

    public static void AddRecent(string projectFilePath)
    {
        try
        {
            var list = new List<string> { projectFilePath };
            list.AddRange(GetRecents());
            var dedup = list.Distinct(StringComparer.OrdinalIgnoreCase).Take(10);
            Directory.CreateDirectory(Path.GetDirectoryName(RecentsFile)!);
            File.WriteAllLines(RecentsFile, dedup, Utf8NoBom);
        }
        catch { /* recents は失敗しても致命的でない */ }
    }

    // ===== テンプレート文字列 =====

    /// <summary>プロジェクト名を CMake ターゲット名 (= exe 名) に使える識別子へ正規化する。</summary>
    public static string SanitizeIdent(string name)
    {
        var sb = new StringBuilder();
        foreach (char c in name) sb.Append(char.IsLetterOrDigit(c) ? c : '_');
        if (sb.Length == 0 || char.IsDigit(sb[0])) sb.Insert(0, '_');
        return sb.ToString();
    }

    // ===== クラス/ソース生成 (基底選択。Empty=空クラス、それ以外は <IDENT>_API エクスポート) =====

    public static readonly string[] BaseClassOptions = { "Empty", "AComponent", "ANode", "FScene2D" };

    /// <summary>選択可能なエンジン基底 (これ以外の基底はユーザー型として扱う)。</summary>
    private static readonly HashSet<string> EngineBaseClasses = new() { "AComponent", "ANode", "FScene2D" };

    /// <summary>baseClass からユーザー継承鎖を辿り、行き着くエンジン基底名を返す (見つからなければ baseClass)。</summary>
    private static string ResolveRootEngineBase(Project p, string baseClass)
    {
        var map = new Dictionary<string, string>();
        foreach (var (name, bas) in ScanUserClasses(p)) map[name] = bas;   // 同名は後勝ち
        string cur = baseClass;
        var seen = new HashSet<string>();
        while (!EngineBaseClasses.Contains(cur) && seen.Add(cur) && map.TryGetValue(cur, out var b)) cur = b;
        return cur;
    }

    // class [API] Name : public Base { を捉える (基底ピッカーの階層構築用)。
    private static readonly Regex UserClassRe = new(
        @"class\s+(?:\w+\s+)?(\w+)\s*:\s*public\s+([\w:]+)\s*\{", RegexOptions.Compiled);

    /// <summary>
    /// プロジェクトの Source/*.h を走査し、ユーザークラスの (名前, 基底名) を返す。
    /// 基底名の名前空間修飾 (acs::game:: 等) は除去する。基底ピッカーの階層表示に使う。
    /// </summary>
    public static List<(string Name, string Base)> ScanUserClasses(Project p)
    {
        var list = new List<(string, string)>();
        string dir = p.SourceDir;
        if (!Directory.Exists(dir)) return list;
        foreach (string h in Directory.GetFiles(dir, "*.h"))
        {
            string text;
            try { text = File.ReadAllText(h); } catch { continue; }
            foreach (Match m in UserClassRe.Matches(text))
            {
                string name = m.Groups[1].Value;
                string bas  = m.Groups[2].Value;
                int c = bas.LastIndexOf("::", StringComparison.Ordinal);
                if (c >= 0) bas = bas.Substring(c + 2);
                if (name != bas) list.Add((name, bas));
            }
        }
        return list;
    }

    private static string ApiHeaderName(string projName) => SanitizeIdent(projName) + "API.h";
    private static string ApiMacro(string ident) => ident.ToUpperInvariant() + "_API";

    private static string ApiHeaderContent(string ident) =>
        "#pragma once\n" +
        $"// {ApiMacro(ident)}: このプロジェクト DLL のエクスポート/インポートマクロ。\n" +
        "// リフレクション DLL (<ident>_reflect) のビルド時は dllexport、それ以外は空。\n" +
        "#if defined(_WIN32)\n" +
        $"#  if defined({ident}_reflect_EXPORTS)\n" +
        $"#    define {ApiMacro(ident)} __declspec(dllexport)\n" +
        "#  else\n" +
        $"#    define {ApiMacro(ident)}\n" +
        "#  endif\n" +
        "#else\n" +
        $"#  define {ApiMacro(ident)}\n" +
        "#endif\n";

    /// <summary>
    /// 表示名を prefix 規約に従う ASCII C++ 型識別子へ変換する。
    /// 表示名そのものは変更せず、生成するファイル名・型名だけに使用する。
    /// </summary>
    private static string CppPascalStem(string? name)
    {
        var sb = new StringBuilder();
        bool capitalizeNext = true;
        foreach (char c in name ?? "")
        {
            bool asciiLetter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
            bool asciiDigit = c >= '0' && c <= '9';
            if (!asciiLetter && !asciiDigit)
            {
                capitalizeNext = true;
                continue;
            }

            char normalized = capitalizeNext && c >= 'a' && c <= 'z'
                ? char.ToUpperInvariant(c)
                : c;
            sb.Append(normalized);
            capitalizeNext = false;
        }
        return sb.ToString();
    }

    private static bool HasKnownCppPrefix(string stem) =>
        stem.Length >= 2
        && stem[0] is 'A' or 'E' or 'F' or 'I' or 'T'
        && ((stem[1] >= 'A' && stem[1] <= 'Z') || (stem[1] >= '0' && stem[1] <= '9'));

    public static string CppTypeIdent(string? name, char requiredPrefix)
    {
        if (requiredPrefix is not ('A' or 'E' or 'F'))
            throw new ArgumentOutOfRangeException(nameof(requiredPrefix));

        string stem = CppPascalStem(name);
        bool prefixOnly = stem.Length == 1 && stem[0] is 'A' or 'E' or 'F' or 'I' or 'T';
        if (stem.Length == 0 || prefixOnly)
        {
            stem = requiredPrefix switch
            {
                'A' => "GeneratedObject",
                'E' => "GeneratedEnum",
                _ => "GeneratedType",
            };
        }

        if (HasKnownCppPrefix(stem))
        {
            if (stem[0] == requiredPrefix) return stem;
            stem = stem.Substring(1);
        }
        return requiredPrefix + stem;
    }

    /// <summary>通常 class/struct 用の互換入口。生成型には F prefix を保証する。</summary>
    public static string ClassIdent(string name) => CppTypeIdent(name, 'F');

    /// <summary>
    /// 表示名を enum class の列挙子に使える ASCII PascalCase 識別子へ変換する。
    /// 型用の A/E/F/I/T prefix が付いていた場合は、列挙子との混同を避けて除去する。
    /// </summary>
    public static string CppEnumeratorIdent(string? name)
    {
        string stem = CppPascalStem(name);
        bool prefixOnly = stem.Length == 1 && stem[0] is 'A' or 'E' or 'F' or 'I' or 'T';
        if (stem.Length == 0 || prefixOnly) return "Value";
        if (HasKnownCppPrefix(stem)) stem = stem.Substring(1);
        return stem[0] >= '0' && stem[0] <= '9' ? "Value" + stem : stem;
    }

    /// <summary>新しいクラス/コンポーネントのソースをプロジェクトの Source に生成し、生成パスを返す。</summary>
    public static System.Collections.Generic.List<string> GenerateClass(Project p, string className, string baseClass)
    {
        bool baseIsEngine = EngineBaseClasses.Contains(baseClass);
        string rootEngine = baseIsEngine ? baseClass : ResolveRootEngineBase(p, baseClass);
        bool objectManaged = rootEngine is "AComponent" or "ANode" or "FObject"
            || (rootEngine.Length >= 2 && rootEngine[0] == 'A' && char.IsUpper(rootEngine[1]));
        string cls = CppTypeIdent(className, objectManaged ? 'A' : 'F');
        string ident = SanitizeIdent(p.Name);
        string api = ApiMacro(ident);
        string apiHeader = ApiHeaderName(p.Name);
        string dir = p.SourceDir;
        var made = new System.Collections.Generic.List<string>();

        Directory.CreateDirectory(dir);
        // API ヘッダが無ければ作る (旧プロジェクト互換)。
        string apiHeaderPath = Path.Combine(dir, apiHeader);
        if (!File.Exists(apiHeaderPath)) File.WriteAllText(apiHeaderPath, ApiHeaderContent(ident), Utf8NoBom);

        string hPath = Path.Combine(dir, cls + ".h");
        if (File.Exists(hPath)) throw new IOException($"既に存在します: {cls}.h");

        if (string.Equals(baseClass, "Empty", StringComparison.OrdinalIgnoreCase))
        {
            File.WriteAllText(hPath,
                "#pragma once\n\n" +
                $"// {cls} — ACS Editor が生成した空クラス。\n" +
                $"class {cls}\n{{\n}};\n", Utf8NoBom);
            made.Add(hPath);
            return made;
        }

        // 基底ありクラス。ACS_CLASS / ACS_PROPERTY マーカーで宣言 → コードジェネレータが登録を生成。
        // 基底がエンジン型なら acs::game:: 修飾、ユーザー型ならそのまま + 基底ヘッダを include。
        string baseQual   = baseIsEngine ? "acs::game::" + baseClass : baseClass;
        bool isComponent  = rootEngine == "AComponent";   // 直/間接に AComponent 由来か
        var h = new StringBuilder();
        h.Append("#pragma once\n");
        h.Append("#include \"gameframework/GameFramework.h\"\n");
        h.Append("#include \"gameframework/Reflect.h\"\n");
        h.Append("#include \"gameframework/AcsClass.h\"\n");
        h.Append($"#include \"{apiHeader}\"\n");
        if (!baseIsEngine) h.Append($"#include \"{baseClass}.h\"\n");   // ユーザー基底のヘッダ
        h.Append("\n");
        h.Append($"// {cls} — ACS Editor が生成 (基底: {baseClass})。\n");
        h.Append("ACS_CLASS()\n");
        h.Append($"class {api} {cls} : public {baseQual}\n{{\npublic:\n");
        if (isComponent)
        {
            h.Append($"    ACS_GAME_COMPONENT_KIND({cls})\n\n");
            h.Append("    // 派生型固有の Kind/ReflectName を宣言し、型別取得と反射登録を一致させる。\n");
            h.Append("    ACS_PROPERTY() float value = 0.0f;\n\n");
            h.Append("    void OnUpdate(acs::f32 dt) noexcept override { (void)dt; }\n");
        }
        else if (rootEngine == "FScene2D")
        {
            h.Append("    void OnReady() noexcept override\n    {\n        SetPixelsPerUnit(64.0f);\n        // TODO: ここでシーンを構築する。\n    }\n");
        }
        else
        {
            h.Append("    // TODO: メンバや振る舞いを追加する。\n");
        }
        h.Append("};\n");
        File.WriteAllText(hPath, h.ToString(), Utf8NoBom);
        made.Add(hPath);
        return made;
    }

    private const string BlankScene = "ACSCENE v1\n0\n";

    // 床 (Static) + プレイヤー (Dynamic) + 左壁 (Static)。エディタで Play すると落下・衝突する。
    private const string Template2DScene =
        "ACSCENE v1\n" +
        "3\n" +
        "1 -1 0.0000 220.0000 0.0000 7.0000 0.4000 48.00 0.250 0.280 0.340 1.000 Ground\n" +
        "2 -1 0.0000 -160.0000 0.0000 1.0000 1.0000 48.00 0.150 0.850 1.000 1.000 Player\n" +
        "3 -1 -260.0000 40.0000 0.0000 1.2000 3.0000 48.00 0.220 0.240 0.300 1.000 WallLeft\n" +
        "COMP 1 APrimitiveRenderer2D\n" +
        "COMP 1 ARigidBody2D\n" +
        "COMP 2 APrimitiveRenderer2D\n" +
        "COMP 2 ARigidBody2D\n" +
        "COMP 3 APrimitiveRenderer2D\n" +
        "COMP 3 ARigidBody2D\n" +
        "CPROP 1 1 0 0.000 0.000 0.000 0.000\n" +     // Ground bodyType = Static
        "CPROP 2 1 0 1.000 0.000 0.000 0.000\n" +     // Player bodyType = Dynamic
        "CPROP 3 1 0 0.000 0.000 0.000 0.000\n" +     // WallLeft bodyType = Static
        "SEL -1 0\n";

    // blank / 2d 共通: エディタで保存した main.acscene を読み込んで表示するスタンドアロン。
    // editor は world=pixel で扱うので PixelsPerUnit=1、読み込んだ境界にカメラを合わせる。
    private const string SceneLoaderSource =
        "// SPDX-License-Identifier: Apache-2.0\n" +
        "// ACS Editor で作成した main.acscene を読み、Build & Run に編集結果を反映する。\n" +
        "// ゲームロジックは OnTick に追加する。\n" +
        "#include \"gameframework/GameFramework.h\"\n" +
        "#include \"gameframework/SceneTextLoader.h\"\n" +
        "#include \"gameframework/Material2D.h\"\n" +
        "#include \"gameframework/RigidWorld2D.h\"\n" +
        "#include \"assetpack/AcpakGameBridge.h\"\n" +
        "#include \"foundation/Platform.h\"\n" +
        "#include \"platform/InputCodes.h\"\n" +
        "#include \"render/Renderer.h\"\n" +
        "#include \"render/IRhiDevice.h\"\n" +
        "#include \"render/IRhiTexture.h\"\n" +
        "#include <cstddef>\n" +
        "#include <cstdio>\n" +
        "#include <cstring>\n\n" +
        "using namespace acs;\n" +
        "using namespace acs::game;\n\n" +
        "namespace {\n\n" +
        "enum class EPackProbe : u8\n" +
        "{\n" +
        "    Missing,\n" +
        "    Found,\n" +
        "    Invalid,\n" +
        "};\n\n" +
        "constexpr std::size_t kPackPathUtf8Capacity = 131072;\n\n" +
        "EPackProbe ProbeExecutableGamePack(char* outPath, std::size_t outCapacity) noexcept\n" +
        "{\n" +
        "    if (outPath == nullptr || outCapacity == 0) return EPackProbe::Invalid;\n" +
        "    outPath[0] = '\\0';\n" +
        "#if defined(_WIN32)\n" +
        "    wchar_t modulePath[32768]{};\n" +
        "    constexpr DWORD moduleCapacity = static_cast<DWORD>(sizeof(modulePath) / sizeof(modulePath[0]));\n" +
        "    const DWORD moduleLength = ::GetModuleFileNameW(nullptr, modulePath, moduleCapacity);\n" +
        "    if (moduleLength == 0 || moduleLength >= moduleCapacity) return EPackProbe::Invalid;\n\n" +
        "    std::size_t directoryLength = moduleLength;\n" +
        "    while (directoryLength > 0 && modulePath[directoryLength - 1] != L'\\\\' && modulePath[directoryLength - 1] != L'/')\n" +
        "        --directoryLength;\n" +
        "    if (directoryLength == 0) return EPackProbe::Invalid;\n\n" +
        "    constexpr wchar_t packName[] = L\"game.acpak\";\n" +
        "    constexpr std::size_t packNameLength = (sizeof(packName) / sizeof(packName[0])) - 1;\n" +
        "    if (directoryLength + packNameLength + 1 > (sizeof(modulePath) / sizeof(modulePath[0])))\n" +
        "        return EPackProbe::Invalid;\n" +
        "    std::memcpy(modulePath + directoryLength, packName, sizeof(packName));\n\n" +
        "    const DWORD attributes = ::GetFileAttributesW(modulePath);\n" +
        "    if (attributes == INVALID_FILE_ATTRIBUTES)\n" +
        "    {\n" +
        "        const DWORD error = ::GetLastError();\n" +
        "        return (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)\n" +
        "            ? EPackProbe::Missing\n" +
        "            : EPackProbe::Invalid;\n" +
        "    }\n" +
        "    if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)\n" +
        "        return EPackProbe::Invalid;\n\n" +
        "    const int required = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, modulePath, -1, nullptr, 0, nullptr, nullptr);\n" +
        "    if (required <= 0 || static_cast<std::size_t>(required) > outCapacity) return EPackProbe::Invalid;\n" +
        "    const int written = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, modulePath, -1, outPath, required, nullptr, nullptr);\n" +
        "    return written == required ? EPackProbe::Found : EPackProbe::Invalid;\n" +
        "#else\n" +
        "    constexpr char packName[] = \"game.acpak\";\n" +
        "    if (sizeof(packName) > outCapacity) return EPackProbe::Invalid;\n" +
        "    std::memcpy(outPath, packName, sizeof(packName));\n" +
        "    std::FILE* probe = std::fopen(outPath, \"rb\");\n" +
        "    if (probe == nullptr) return EPackProbe::Missing;\n" +
        "    std::fclose(probe);\n" +
        "    return EPackProbe::Found;\n" +
        "#endif\n" +
        "}\n\n" +
        "enum class EBootstrapSceneKind : u8\n" +
        "{\n" +
        "    Invalid,\n" +
        "    Legacy2D,\n" +
        "    Legacy3D,\n" +
        "};\n\n" +
        "EBootstrapSceneKind DetectBootstrapScene(const u8* bytes, u64 size) noexcept\n" +
        "{\n" +
        "    if (bytes == nullptr) return EBootstrapSceneKind::Invalid;\n" +
        "    constexpr char scene2d[] = \"ACSCENE v1\";\n" +
        "    constexpr char scene3d[] = \"ACS3D v2\";\n" +
        "    const auto exactHeader = [bytes, size](const char* header, u64 length) noexcept\n" +
        "    {\n" +
        "        if (size <= length || std::memcmp(bytes, header, static_cast<std::size_t>(length)) != 0) return false;\n" +
        "        return bytes[length] == '\\n' || (bytes[length] == '\\r' && size > length + 1 && bytes[length + 1] == '\\n');\n" +
        "    };\n" +
        "    if (exactHeader(scene2d, sizeof(scene2d) - 1)) return EBootstrapSceneKind::Legacy2D;\n" +
        "    if (exactHeader(scene3d, sizeof(scene3d) - 1)) return EBootstrapSceneKind::Legacy3D;\n" +
        "    return EBootstrapSceneKind::Invalid;\n" +
        "}\n\n" +
        "EBootstrapSceneKind ProbeCanonicalBootstrap() noexcept\n" +
        "{\n" +
        "    char packPath[kPackPathUtf8Capacity]{};\n" +
        "    const EPackProbe packProbe = ProbeExecutableGamePack(packPath, sizeof(packPath));\n" +
        "    if (packProbe == EPackProbe::Invalid) return EBootstrapSceneKind::Invalid;\n" +
        "    if (packProbe == EPackProbe::Found)\n" +
        "    {\n" +
        "        assetpack::FAcpakGameReader pack;\n" +
        "        if (pack.Mount(packPath).IsErr()) return EBootstrapSceneKind::Invalid;\n" +
        "        const auto fileSize = pack.FileSize(\"main.acscene\");\n" +
        "        if (fileSize.IsErr() || fileSize.Value() == 0 || fileSize.Value() > kScene3DSerializeMaxInputBytes)\n" +
        "            return EBootstrapSceneKind::Invalid;\n" +
        "        TArray<u8> bytes;\n" +
        "        if (!bytes.TryResize(static_cast<usize>(fileSize.Value())) ||\n" +
        "            pack.ReadFile(\"main.acscene\", bytes.Data(), fileSize.Value()).IsErr())\n" +
        "            return EBootstrapSceneKind::Invalid;\n" +
        "        return DetectBootstrapScene(bytes.Data(), fileSize.Value());\n" +
        "    }\n" +
        "    std::FILE* file = std::fopen(\"main.acscene\", \"rb\");\n" +
        "    if (file == nullptr) return EBootstrapSceneKind::Invalid;\n" +
        "    u8 bytes[32]{};\n" +
        "    const u64 read = static_cast<u64>(std::fread(bytes, 1, sizeof(bytes), file));\n" +
        "    std::fclose(file);\n" +
        "    return DetectBootstrapScene(bytes, read);\n" +
        "}\n\n" +
        "class FMainScene2D final : public FScene2D\n" +
        "{\n" +
        "public:\n" +
        "    void OnReady() noexcept override\n" +
        "    {\n" +
        "        SetPixelsPerUnit(1.0f);\n" +
        "        GetGame().SetClearColor(0.07f, 0.08f, 0.10f);\n" +
        "        TArray<FRigidBodyRequest> bodies;\n" +
        "        FSceneTextLoadResult loaded;\n" +
        "        char packPath[kPackPathUtf8Capacity]{};\n" +
        "        const EPackProbe packProbe = ProbeExecutableGamePack(packPath, sizeof(packPath));\n" +
        "        if (packProbe == EPackProbe::Invalid)\n" +
        "        {\n" +
        "            std::fprintf(stderr, \"ACS package path is invalid or inaccessible; refusing loose fallback.\\n\");\n" +
        "            GetGame().Quit();\n" +
        "            return;\n" +
        "        }\n" +
        "        if (packProbe == EPackProbe::Found)\n" +
        "        {\n" +
        "            const auto mounted = m_Pack.Mount(packPath);\n" +
        "            if (mounted.IsErr())\n" +
        "            {\n" +
        "                std::fprintf(stderr, \"ACS package mount failed; refusing loose fallback.\\n\");\n" +
        "                GetGame().Quit();\n" +
        "                return;\n" +
        "            }\n" +
        "            m_UsePack = true;\n" +
        "            loaded = TryLoadAcsceneAssetPack(m_Pack, \"main.acscene\", Root(), &m_Sprites, &bodies, &m_MatReqs);\n" +
        "        }\n" +
        "        else\n" +
        "        {\n" +
        "            loaded = TryLoadAcsceneFile(\"main.acscene\", Root(), &m_Sprites, &bodies, &m_MatReqs);\n" +
        "        }\n" +
        "        if (!loaded.Succeeded())\n" +
        "        {\n" +
        "            std::fprintf(stderr, \"ACS scene load failed: %s\\n\", SceneTextLoadErrorName(loaded.Error));\n" +
        "            GetGame().Quit();\n" +
        "            return;\n" +
        "        }\n" +
        "        m_Bounds = loaded.Bounds;\n" +
        "        BuildSceneRigidBodies(m_World, bodies, m_PhysNodes, m_PhysBodies);\n" +
        "        if (m_Bounds.valid) Services().Camera().SetPosition(m_Bounds.Center());\n" +
        "    }\n\n" +
        "    void OnTick(f32 dt) noexcept override\n" +
        "    {\n" +
        "        m_MatTime += dt; SetMaterialClock(m_MatTime);   // Wave/HueShift 用のアニメーション時刻\n" +
        "        StepSceneRigidBodies(m_World, m_PhysNodes, m_PhysBodies, dt, FVec2{ 0.0f, 900.0f });\n" +
        "        if (Services().Input().IsPressed(FActionId(\"Quit\"))) GetGame().Quit();\n" +
        "        if (!m_Framed && m_Bounds.valid && ScreenWidth() > 0)\n" +
        "        {\n" +
        "            const f32 sw = static_cast<f32>(ScreenWidth());\n" +
        "            const f32 sh = static_cast<f32>(ScreenHeight());\n" +
        "            const FVec2 sz = m_Bounds.Size();\n" +
        "            const f32 zx = (sz.x > 1.0f) ? sw / (sz.x * 1.25f) : 1.0f;\n" +
        "            const f32 zy = (sz.y > 1.0f) ? sh / (sz.y * 1.25f) : 1.0f;\n" +
        "            f32 z = zx < zy ? zx : zy;\n" +
        "            if (z <= 0.0f) z = 1.0f;\n" +
        "            Services().Camera().SetZoom(z);\n" +
        "            Services().Camera().SetPosition(m_Bounds.Center());\n" +
        "            m_Framed = true;\n" +
        "        }\n" +
        "    }\n\n" +
        "    // device が要るスプライトのテクスチャは初回描画時に遅延ロードする。\n" +
        "    void OnDrawWorld(FRenderContext& rc, FSpriteBatch& /*sb*/) noexcept override\n" +
        "    {\n" +
        "        if (m_SpritesLoaded) return;\n" +
        "        IRhiDevice* dev = rc.GetRenderer().Device();\n" +
        "        if (dev == nullptr) return;\n" +
        "        if (m_UsePack)\n" +
        "        {\n" +
        "            LoadSceneSpritesFromAssetPack(*dev, m_Sprites, m_Pack, m_Textures);\n" +
        "            LoadSceneMaterialTexturesFromAssetPack(*dev, m_MatReqs, m_Pack, m_MatTextures);\n" +
        "        }\n" +
        "        else\n" +
        "        {\n" +
        "            LoadSceneSprites(*dev, m_Sprites, m_Textures);\n" +
        "            LoadSceneMaterialTextures(*dev, m_MatReqs, m_MatTextures);\n" +
        "        }\n" +
        "        m_SpritesLoaded = true;\n" +
        "    }\n\n" +
        "private:\n" +
        "    FSceneBounds                    m_Bounds{};\n" +
        "    bool                            m_Framed = false;\n" +
        "    assetpack::FAcpakGameReader     m_Pack;\n" +
        "    bool                            m_UsePack = false;\n" +
        "    TArray<FSpriteRequest>          m_Sprites;\n" +
        "    TArray<TUniquePtr<IRhiTexture>> m_Textures;\n" +
        "    bool                            m_SpritesLoaded = false;\n" +
        "    FRigidWorld2D                   m_World;\n" +
        "    TArray<ANode*>                m_PhysNodes;\n" +
        "    TArray<u32>                     m_PhysBodies;\n" +
        "    f32                             m_MatTime = 0.0f;\n" +
        "    TArray<FMaterialTexRequest>     m_MatReqs;\n" +
        "    TArray<TUniquePtr<IRhiTexture>> m_MatTextures;\n" +
        "};\n\n" +
        "class FMainScene3D final : public FLegacyScene3DAdapter\n" +
        "{\n" +
        "public:\n" +
        "    void OnEnter() noexcept override\n" +
        "    {\n" +
        "        char packPath[kPackPathUtf8Capacity]{};\n" +
        "        const EPackProbe packProbe = ProbeExecutableGamePack(packPath, sizeof(packPath));\n" +
        "        if (packProbe == EPackProbe::Invalid)\n" +
        "        {\n" +
        "            std::fprintf(stderr, \"ACS package path is invalid or inaccessible; refusing loose fallback.\\n\");\n" +
        "            GetGame().Quit();\n" +
        "            return;\n" +
        "        }\n" +
        "        FScene3DLoadResult loaded;\n" +
        "        if (packProbe == EPackProbe::Found)\n" +
        "        {\n" +
        "            if (m_Pack.Mount(packPath).IsErr())\n" +
        "            {\n" +
        "                std::fprintf(stderr, \"ACS package mount failed; refusing loose fallback.\\n\");\n" +
        "                GetGame().Quit();\n" +
        "                return;\n" +
        "            }\n" +
        "            loaded = LoadAssetPack(m_Pack, \"main.acscene\");\n" +
        "        }\n" +
        "        else\n" +
        "        {\n" +
        "            loaded = LoadFile(\"main.acscene\");\n" +
        "        }\n" +
        "        if (!loaded.Succeeded())\n" +
        "        {\n" +
        "            std::fprintf(stderr, \"ACS legacy .acs3d source load failed: %s\\n\", Scene3DSerializeErrorName(loaded.Error));\n" +
        "            GetGame().Quit();\n" +
        "            return;\n" +
        "        }\n" +
        "        FLegacyScene3DAdapter::OnEnter();\n" +
        "    }\n\n" +
        "    void OnExit() noexcept override\n" +
        "    {\n" +
        "        FLegacyScene3DAdapter::OnExit();\n" +
        "        m_Pack.Unmount();\n" +
        "    }\n\n" +
        "private:\n" +
        "    assetpack::FAcpakGameReader m_Pack;\n" +
        "};\n\n" +
        "class FInvalidBootstrapScene final : public FScene\n" +
        "{\n" +
        "public:\n" +
        "    void OnEnter() noexcept override\n" +
        "    {\n" +
        "        std::fprintf(stderr, \"ACS canonical scene bootstrap is missing, corrupt, or uses an unsupported header.\\n\");\n" +
        "        GetGame().Quit();\n" +
        "    }\n" +
        "};\n\n" +
        "class FGameApp final : public FGame\n" +
        "{\n" +
        "protected:\n" +
        "    TUniquePtr<FScene> InitialScene() noexcept override\n" +
        "    {\n" +
        "        switch (ProbeCanonicalBootstrap())\n" +
        "        {\n" +
        "        case EBootstrapSceneKind::Legacy2D: return MakeUnique<FMainScene2D>();\n" +
        "        case EBootstrapSceneKind::Legacy3D: return MakeUnique<FMainScene3D>();\n" +
        "        default: return MakeUnique<FInvalidBootstrapScene>();\n" +
        "        }\n" +
        "    }\n" +
        "};\n\n" +
        "} // namespace\n\n" +
        "ACS_GAME_MAIN(FGameApp)\n";

    /// <summary>プロジェクトのビルド定義 (Source/CMakeLists.txt) を現在のテンプレートへ更新する。
    /// 旧バージョンのエディタで作られたプロジェクトの陳腐化した CMakeLists (例: ACS_ENGINE_DIR を
    /// 要求する古いスタブ) を自己修復する。内容が一致していれば書き込まない (不要な再 configure を回避)。
    /// 書き換えたら true を返す (呼び出し側は再 configure を強制すべき)。</summary>
    public static bool EnsureBuildFiles(Project project)
    {
        try
        {
            string cmakePath = Path.Combine(project.RootDir, "Source", "CMakeLists.txt");
            string want = CMakeTemplate(SanitizeIdent(project.Name), project.RootDir);
            if (File.Exists(cmakePath) && File.ReadAllText(cmakePath) == want) return false;
            Directory.CreateDirectory(Path.GetDirectoryName(cmakePath)!);
            File.WriteAllText(cmakePath, want, Utf8NoBom);
            return true;
        }
        catch { return false; }   // 書けなくてもビルドは試みる
    }

    // エンジンが ACS_EXTERNAL_PROJECT_DIR=<Source> 経由で add_subdirectory する前提。
    // サンプルと同じく add_executable + ACS::GameFramework だけで済む。出力はプロジェクトの Binaries へ。
    private static string CMakeTemplate(string ident, string rootDir)
    {
        string binDir = (Path.Combine(rootDir, "Binaries")).Replace('\\', '/');
        return
            "# ACS Editor による自動生成。ACS engine build 内の target として構築する。\n" +
            "# editor は -DACS_EXTERNAL_PROJECT_DIR=<this dir> を渡し ACS::GameFramework を link する。\n" +
            "file(GLOB ACS_PROJ_SOURCES CONFIGURE_DEPENDS \"${CMAKE_CURRENT_SOURCE_DIR}/*.cpp\")\n\n" +
            "# スタンドアロン実行ファイル (Run 用)。\n" +
            $"add_executable({ident} WIN32 ${{ACS_PROJ_SOURCES}})\n" +
            $"acs_apply_compiler_options({ident})\n" +
            $"target_link_libraries({ident} PRIVATE ACS::GameFramework ACS::AssetPack)\n" +
            $"target_include_directories({ident} PRIVATE \"${{ACS_SOURCE_ROOT}}\")\n" +
            $"set_target_properties({ident} PROPERTIES RUNTIME_OUTPUT_DIRECTORY \"{binDir}\")\n\n" +
            "# リフレクション DLL: ユーザー定義型をエディタへ公開する (editor が LoadLibrary する)。\n" +
            $"add_library({ident}_reflect SHARED ${{ACS_PROJ_SOURCES}} \"${{ACS_SOURCE_ROOT}}/editor_abi/GameReflectShim.cpp\")\n" +
            $"acs_apply_compiler_options({ident}_reflect)\n" +
            $"target_link_libraries({ident}_reflect PRIVATE ACS::GameFramework ACS::AssetPack)\n" +
            $"target_include_directories({ident}_reflect PRIVATE \"${{ACS_SOURCE_ROOT}}\")\n" +
            "# <IDENT>_API でエクスポートするユーザークラスの DLL インターフェース警告を抑制 (リフレクション用途のため無害)。\n" +
            $"target_compile_options({ident}_reflect PRIVATE /wd4275 /wd4251)\n" +
            $"set_target_properties({ident}_reflect PROPERTIES RUNTIME_OUTPUT_DIRECTORY \"{binDir}\")\n";
    }
}
