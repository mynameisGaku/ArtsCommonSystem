using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace AcsEditor;

/// <summary>プロジェクトの新規作成・オープン・最近使った一覧。フォルダ生成とテンプレート展開を担う。</summary>
public static class ProjectManager
{
    // マニフェスト JSON の (デ)シリアライズ用 DTO (Project の計算プロパティを書き出さないため分離)。
    private sealed class ManifestDto
    {
        public int    version       { get; set; } = 1;
        public string name          { get; set; } = "";
        public string engineVersion { get; set; } = "";
        public string template      { get; set; } = "blank";
        public string initialScene  { get; set; } = "Assets/main.acscene";
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
        Directory.CreateDirectory(Path.Combine(rootDir, "Assets"));
        Directory.CreateDirectory(Path.Combine(rootDir, "Source"));
        Directory.CreateDirectory(Path.Combine(rootDir, "Temp"));

        bool is2d = string.Equals(template, "2d", StringComparison.OrdinalIgnoreCase);

        // 初期シーン (エディタ表示用 ACSCENE)。
        File.WriteAllText(Path.Combine(rootDir, "Assets", "main.acscene"),
                          is2d ? Template2DScene : BlankScene, Utf8NoBom);

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
            InitialScene = "Assets/main.acscene",
            ProjectFilePath = Path.Combine(rootDir, name + ".acsproject"),
        };
        WriteManifest(proj);
        AddRecent(proj.ProjectFilePath);
        return proj;
    }

    /// <summary>.acsproject マニフェストを開いて Project を返す。</summary>
    public static Project Open(string acsprojectPath)
    {
        if (!File.Exists(acsprojectPath))
            throw new FileNotFoundException($"プロジェクトファイルが見つかりません: {acsprojectPath}");
        string json = File.ReadAllText(acsprojectPath, Encoding.UTF8);
        var dto = JsonSerializer.Deserialize<ManifestDto>(json)
                  ?? throw new InvalidDataException("マニフェストの解析に失敗しました。");
        var proj = new Project
        {
            Version = dto.version,
            Name = string.IsNullOrEmpty(dto.name) ? Path.GetFileNameWithoutExtension(acsprojectPath) : dto.name,
            EngineVersion = dto.engineVersion,
            Template = dto.template,
            InitialScene = string.IsNullOrEmpty(dto.initialScene) ? "Assets/main.acscene" : dto.initialScene,
            ProjectFilePath = Path.GetFullPath(acsprojectPath),
        };
        AddRecent(proj.ProjectFilePath);
        return proj;
    }

    private static void WriteManifest(Project p)
    {
        var dto = new ManifestDto
        {
            version = p.Version, name = p.Name, engineVersion = p.EngineVersion,
            template = p.Template, initialScene = p.InitialScene,
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

    public static readonly string[] BaseClassOptions = { "Empty", "FComponent2D", "FNode2D", "FScene2D" };

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

    /// <summary>クラス名を有効な C++ 識別子に整える。</summary>
    public static string ClassIdent(string name)
    {
        var sb = new StringBuilder();
        foreach (char c in name) sb.Append(char.IsLetterOrDigit(c) ? c : '_');
        if (sb.Length == 0 || char.IsDigit(sb[0])) sb.Insert(0, 'F');
        return sb.ToString();
    }

    /// <summary>新しいクラス/コンポーネントのソースをプロジェクトの Source に生成し、生成パスを返す。</summary>
    public static System.Collections.Generic.List<string> GenerateClass(Project p, string className, string baseClass)
    {
        string cls = ClassIdent(className);
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
                $"// {cls} — empty class generated by ACS Editor.\n" +
                $"class {cls}\n{{\n}};\n", Utf8NoBom);
            made.Add(hPath);
            return made;
        }

        // 基底ありクラス。ACS_CLASS / ACS_PROPERTY マーカーで宣言 → コードジェネレータが登録を生成。
        bool isComponent = baseClass == "FComponent2D";
        string baseQual = "acs::game::" + baseClass;
        var h = new StringBuilder();
        h.Append("#pragma once\n");
        h.Append("#include \"gameframework/GameFramework.h\"\n");
        h.Append("#include \"gameframework/Reflect.h\"\n");
        h.Append("#include \"gameframework/AcsClass.h\"\n");
        h.Append($"#include \"{apiHeader}\"\n\n");
        h.Append($"// {cls} — generated by ACS Editor (base: {baseClass}).\n");
        h.Append("ACS_CLASS()\n");
        h.Append($"class {api} {cls} : public {baseQual}\n{{\npublic:\n");
        if (isComponent)
        {
            h.Append($"    ACS_GAME_COMPONENT_KIND({cls})\n\n");
            h.Append("    // インスペクタに出したいプロパティは ACS_PROPERTY() を付ける。\n");
            h.Append("    ACS_PROPERTY() float value = 0.0f;\n\n");
            h.Append("    void OnUpdate(acs::f32 dt) noexcept override { (void)dt; }\n");
        }
        else if (baseClass == "FScene2D")
        {
            h.Append("    void OnReady() noexcept override\n    {\n        SetPixelsPerUnit(64.0f);\n        // TODO: build your scene here.\n    }\n");
        }
        else
        {
            h.Append("    // TODO: add members / behavior.\n");
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
        "COMP 1 FPrimitiveRenderer2D\n" +
        "COMP 1 FRigidBody2D\n" +
        "COMP 2 FPrimitiveRenderer2D\n" +
        "COMP 2 FRigidBody2D\n" +
        "COMP 3 FPrimitiveRenderer2D\n" +
        "COMP 3 FRigidBody2D\n" +
        "CPROP 1 1 0 0.000 0.000 0.000 0.000\n" +     // Ground bodyType = Static
        "CPROP 2 1 0 1.000 0.000 0.000 0.000\n" +     // Player bodyType = Dynamic
        "CPROP 3 1 0 0.000 0.000 0.000 0.000\n" +     // WallLeft bodyType = Static
        "SEL -1 0\n";

    // blank / 2d 共通: エディタで保存した main.acscene を読み込んで表示するスタンドアロン。
    // editor は world=pixel で扱うので PixelsPerUnit=1、読み込んだ境界にカメラを合わせる。
    private const string SceneLoaderSource =
        "// SPDX-License-Identifier: Apache-2.0\n" +
        "// Loads the scene authored in the ACS editor (main.acscene) so Build & Run shows\n" +
        "// exactly what you edited. Add game logic in OnTick.\n" +
        "#include \"gameframework/GameFramework.h\"\n" +
        "#include \"gameframework/SceneTextLoader.h\"\n" +
        "#include \"gameframework/Material2D.h\"\n" +
        "#include \"gameframework/RigidWorld2D.h\"\n" +
        "#include \"platform/InputCodes.h\"\n" +
        "#include \"render/Renderer.h\"\n" +
        "#include \"render/IRhiDevice.h\"\n" +
        "#include \"render/IRhiTexture.h\"\n\n" +
        "using namespace acs;\n" +
        "using namespace acs::game;\n\n" +
        "namespace {\n\n" +
        "class FMainScene final : public FScene2D\n" +
        "{\n" +
        "public:\n" +
        "    void OnReady() noexcept override\n" +
        "    {\n" +
        "        SetPixelsPerUnit(1.0f);\n" +
        "        GetGame().SetClearColor(0.07f, 0.08f, 0.10f);\n" +
        "        TArray<FRigidBodyRequest> bodies;\n" +
        "        m_Bounds = LoadAcsceneFile(\"main.acscene\", Root(), &m_Sprites, &bodies, &m_MatReqs);\n" +
        "        BuildSceneRigidBodies(m_World, bodies, m_PhysNodes, m_PhysBodies);\n" +
        "        if (m_Bounds.valid) Services().Camera().SetPosition(m_Bounds.Center());\n" +
        "    }\n\n" +
        "    void OnTick(f32 dt) noexcept override\n" +
        "    {\n" +
        "        m_MatTime += dt; SetMaterialClock(m_MatTime);   // animated material clock (Wave/HueShift)\n" +
        "        StepSceneRigidBodies(m_World, m_PhysNodes, m_PhysBodies, dt, FVec2{ 0.0f, 900.0f });\n" +
        "        if (Services().Input().IsPressed(ActionId(\"Quit\"))) GetGame().Quit();\n" +
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
        "    void OnDrawWorld(RenderContext& rc, FSpriteBatch& /*sb*/) noexcept override\n" +
        "    {\n" +
        "        if (m_SpritesLoaded) return;\n" +
        "        IRhiDevice* dev = rc.GetRenderer().Device();\n" +
        "        if (dev == nullptr) return;\n" +
        "        LoadSceneSprites(*dev, m_Sprites, m_Textures);\n" +
        "        LoadSceneMaterialTextures(*dev, m_MatReqs, m_MatTextures);   // PBR 法線マップ\n" +
        "        m_SpritesLoaded = true;\n" +
        "    }\n\n" +
        "private:\n" +
        "    FSceneBounds                    m_Bounds{};\n" +
        "    bool                            m_Framed = false;\n" +
        "    TArray<FSpriteRequest>          m_Sprites;\n" +
        "    TArray<TUniquePtr<IRhiTexture>> m_Textures;\n" +
        "    bool                            m_SpritesLoaded = false;\n" +
        "    FRigidWorld2D                   m_World;\n" +
        "    TArray<FNode2D*>                m_PhysNodes;\n" +
        "    TArray<u32>                     m_PhysBodies;\n" +
        "    f32                             m_MatTime = 0.0f;\n" +
        "    TArray<FMaterialTexRequest>     m_MatReqs;\n" +
        "    TArray<TUniquePtr<IRhiTexture>> m_MatTextures;\n" +
        "};\n\n" +
        "class FGameApp final : public FGame\n" +
        "{\n" +
        "protected:\n" +
        "    TUniquePtr<Scene> InitialScene() noexcept override { return MakeUnique<FMainScene>(); }\n" +
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
            "# Auto-generated by ACS Editor. Built as targets inside the ACS engine build\n" +
            "# (the editor passes -DACS_EXTERNAL_PROJECT_DIR=<this dir> and links ACS::GameFramework).\n" +
            "file(GLOB ACS_PROJ_SOURCES CONFIGURE_DEPENDS \"${CMAKE_CURRENT_SOURCE_DIR}/*.cpp\")\n\n" +
            "# スタンドアロン実行ファイル (Run 用)。\n" +
            $"add_executable({ident} WIN32 ${{ACS_PROJ_SOURCES}})\n" +
            $"acs_apply_compiler_options({ident})\n" +
            $"target_link_libraries({ident} PRIVATE ACS::GameFramework)\n" +
            $"target_include_directories({ident} PRIVATE \"${{ACS_SOURCE_ROOT}}\")\n" +
            $"set_target_properties({ident} PROPERTIES RUNTIME_OUTPUT_DIRECTORY \"{binDir}\")\n\n" +
            "# リフレクション DLL: ユーザー定義型をエディタへ公開する (editor が LoadLibrary する)。\n" +
            $"add_library({ident}_reflect SHARED ${{ACS_PROJ_SOURCES}} \"${{ACS_SOURCE_ROOT}}/editor_abi/GameReflectShim.cpp\")\n" +
            $"acs_apply_compiler_options({ident}_reflect)\n" +
            $"target_link_libraries({ident}_reflect PRIVATE ACS::GameFramework)\n" +
            $"target_include_directories({ident}_reflect PRIVATE \"${{ACS_SOURCE_ROOT}}\")\n" +
            "# <IDENT>_API でエクスポートするユーザークラスの DLL インターフェース警告を抑制 (リフレクション用途のため無害)。\n" +
            $"target_compile_options({ident}_reflect PRIVATE /wd4275 /wd4251)\n" +
            $"set_target_properties({ident}_reflect PROPERTIES RUNTIME_OUTPUT_DIRECTORY \"{binDir}\")\n";
    }
}
