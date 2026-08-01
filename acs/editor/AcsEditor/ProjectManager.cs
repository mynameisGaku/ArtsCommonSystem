using System;
using System.Buffers;
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
    private const int CurrentProjectManifestVersion = 1;

    internal readonly record struct NewProjectScenePlan(
        string Template,
        string InitialScene,
        string SceneText,
        SceneDocumentMode SourceMode,
        bool StartsOrthographic);

    // マニフェスト JSON の (デ)シリアライズ用 DTO (Project の計算プロパティを書き出さないため分離)。
    private sealed class ManifestDto
    {
        public int    version       { get; set; } = CurrentProjectManifestVersion;
        public string name          { get; set; } = "";
        public string engineVersion { get; set; } = "";
        public string template      { get; set; } = "blank";
        public string initialScene  { get; set; } = "Assets/main.acscene";
        public string canonicalSceneAssetId { get; set; } = "";
    }

    private static readonly JsonSerializerOptions JsonOpts = new() { WriteIndented = true };
    private static readonly JsonSerializerOptions ManifestReadJsonOpts = new()
    {
        PropertyNameCaseInsensitive = true,
    };
    private static readonly UTF8Encoding Utf8NoBom = new(false);

    /// <summary>
    /// 新規プロジェクトを &lt;parentDir&gt;/&lt;name&gt;/ に生成する。
    /// template は "3d" | "2d"。"blank" は旧ランチャー向けの 3D alias としてのみ受理する。
    /// </summary>
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

        NewProjectScenePlan scenePlan = PlanNewProjectScene(template);

        // New projects always author one ACS3D world. "2d" is only an initial
        // XY-Orthographic viewport preset; it is never a second payload format.
        string assetsDir = Path.Combine(rootDir, "Assets");
        string initialScenePath = SceneSourceFile.ResolveProjectSceneReference(
            rootDir,
            assetsDir,
            scenePlan.InitialScene,
            scenePlan.SourceMode);
        Directory.CreateDirectory(Path.Combine(rootDir, "Source"));
        Directory.CreateDirectory(Path.Combine(rootDir, "Temp"));
        SceneSourceFile.WriteProjectSceneAtomicText(
            initialScenePath,
            scenePlan.SceneText,
            rootDir,
            assetsDir,
            scenePlan.SourceMode);

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
            Template = scenePlan.Template,
            InitialScene = SceneSourceFile.NormalizeProjectSceneReference(
                rootDir,
                assetsDir,
                scenePlan.InitialScene,
                scenePlan.SourceMode),
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
        // Native ProjectSettings keeps a legacy .acscene default for old
        // manifests. New projects persist an explicit coherent override.
        SaveProjectSettings(
            proj,
            "[Game]\nDefaultScene=Assets/main.acs3d\n");
        AddRecent(proj.ProjectFilePath);
        return proj;
    }

    /// <summary>
    /// Produces the source contract for a new project without touching disk. "2d" selects an
    /// editor camera preset only; both templates author the same ACS3D document format.
    /// </summary>
    internal static NewProjectScenePlan PlanNewProjectScene(string? template)
    {
        string normalized = (template ?? "").Trim().ToLowerInvariant();
        if (normalized == "blank")
            normalized = "3d";
        if (normalized is not ("3d" or "2d"))
            throw new ArgumentException(
                "Project template must be '3d' or '2d'.",
                nameof(template));
        bool is2D = normalized == "2d";
        return new NewProjectScenePlan(
            normalized,
            "Assets/main.acs3d",
            is2D ? Template2DScene3D : BlankScene3D,
            SceneDocumentMode.ThreeD,
            StartsOrthographic: is2D);
    }

    /// <summary>.acsproject マニフェストを開いて Project を返す。</summary>
    public static Project Open(string acsprojectPath)
    {
        Project project = ReadManifest(acsprojectPath);
        // 復旧順序全体を一つのプロセス間リース内で固定する。Import/Reimport は
        // ペイロードとサイドカーの組を不完全な状態で残すことがあるため、シーン復旧が
        // 正式なサイドカーを走査する前に確定する。その後、シーン処理が起動参照を更新する。
        using (AssetMutationLock recoveryLease =
               AssetMutationLock.AcquireForRecovery(
                   project.AssetsDir,
                   "Recover project startup transactions"))
        {
            var assetDatabase = AssetDatabase.ForProject(project);
            AssetImportReconciliationResult importRecovery =
                AssetImportWorkflow.Reconcile(assetDatabase);
            ThrowIfAssetPublicationRecoveryIsAmbiguous(
                importRecovery,
                "Import");
            AssetImportReconciliationResult reimportRecovery =
                AssetReimportWorkflow.Reconcile(assetDatabase);
            ThrowIfAssetPublicationRecoveryIsAmbiguous(
                reimportRecovery,
                "Reimport");

            // Content Browser の移動と二つの起動参照ファイルは、一つのファイルシステム
            // トランザクションを共有できない。クラッシュ後に残った永続 intent を Project
            // オブジェクトの公開前に解決し、呼び出し元が古い参照を保持しないよう manifest を再読込する。
            ProjectSceneReferenceRecoveryResult sceneRecovery =
                ReconcileInitialScenePathFollow(project);
            if (sceneRecovery.Status is
                ProjectSceneReferenceRecoveryStatus.Deferred or
                ProjectSceneReferenceRecoveryStatus.LiveOperation)
            {
                throw new InvalidDataException(
                    "The project has an unresolved initial-scene move and remains fail-closed: " +
                    sceneRecovery.Message);
            }

            _ = BackfillCanonicalSceneAssetId(project, assetDatabase);
        }
        project = ReadManifest(acsprojectPath);
        try
        {
            // The manifest and ProjectSettings.ini jointly form the persistent
            // startup-scene record. Never publish a Project whose two references
            // disagree: MainWindow would otherwise load Game.DefaultScene and can
            // briefly present a stale scene before the intended document opens.
            ValidateInitialSceneReferenceFollow(project);
        }
        catch (InvalidDataException error)
        {
            throw new InvalidDataException(
                "The project startup-scene references are inconsistent. " +
                "The .acsproject initialScene and Game.DefaultScene values must identify " +
                "the same scene asset. " + error.Message,
                error);
        }
        AddRecent(project.ProjectFilePath);
        return project;
    }

    private static void ThrowIfAssetPublicationRecoveryIsAmbiguous(
        AssetImportReconciliationResult recovery,
        string operation)
    {
        if (recovery.PreservedTransactions == 0)
            return;
        string details = recovery.Warnings.Count == 0
            ? ""
            : " " + string.Join(" ", recovery.Warnings);
        throw new InvalidDataException(
            $"{operation} recovery preserved {recovery.PreservedTransactions} ambiguous " +
            "transaction(s); project open remains fail-closed." + details);
    }

    /// <summary>
    /// Reads and validates a project manifest without mutating the recent-project list.
    /// </summary>
    internal static Project ReadManifest(string acsprojectPath)
    {
        string requestedPath = Path.GetFullPath(acsprojectPath);
        string projectRoot = Path.GetDirectoryName(requestedPath)
            ?? throw new InvalidDataException(".acsproject path has no project directory.");
        SceneSourceFile.ValidateProjectRootDirectory(projectRoot);
        ReferenceFileSnapshot manifest = CaptureRequiredOrdinaryFile(
            requestedPath,
            MaxProjectManifestBytes,
            ".acsproject manifest",
            requireWritable: false);
        return ParseManifestSnapshot(manifest.Path, manifest.Bytes);
    }

    private static Project ParseManifestSnapshot(
        string projectFilePath,
        ReadOnlySpan<byte> manifestBytes)
    {
        projectFilePath = Path.GetFullPath(projectFilePath);
        string projectRoot = Path.GetDirectoryName(projectFilePath)
            ?? throw new InvalidDataException(
                ".acsproject path has no project directory.");
        SceneSourceFile.ValidateProjectRootDirectory(projectRoot);
        if (manifestBytes.StartsWith(new byte[] { 0xEF, 0xBB, 0xBF }))
            manifestBytes = manifestBytes[3..];
        string json;
        try
        {
            json = StrictUtf8NoBom.GetString(manifestBytes);
        }
        catch (DecoderFallbackException error)
        {
            throw new InvalidDataException(
                "The .acsproject manifest is not valid UTF-8.",
                error);
        }
        if (json.IndexOf('\0') >= 0)
            throw new InvalidDataException(
                "The .acsproject manifest contains an embedded NUL.");
        ValidateManifestJson(manifestBytes);
        ManifestDto dto;
        try
        {
            dto = JsonSerializer.Deserialize<ManifestDto>(
                      json,
                      ManifestReadJsonOpts)
                  ?? throw new InvalidDataException(
                      "The .acsproject manifest could not be parsed.");
        }
        catch (JsonException error)
        {
            throw new InvalidDataException(
                "The .acsproject manifest has an invalid field type or value.",
                error);
        }
        ValidateManifestDto(dto);
        string effectiveName = string.IsNullOrEmpty(dto.name)
            ? Path.GetFileNameWithoutExtension(projectFilePath)
            : dto.name;
        ValidateManifestText(effectiveName, "name", 256);
        var proj = new Project
        {
            Version = dto.version,
            Name = effectiveName,
            EngineVersion = dto.engineVersion ?? "",
            Template = dto.template ?? "blank",
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

    private static void ValidateManifestJson(ReadOnlySpan<byte> utf8)
    {
        JsonDocument document;
        try
        {
            document = JsonDocument.Parse(
                utf8.ToArray(),
                new JsonDocumentOptions
                {
                    AllowTrailingCommas = false,
                    CommentHandling = JsonCommentHandling.Disallow,
                    MaxDepth = 32,
                });
        }
        catch (JsonException error)
        {
            throw new InvalidDataException(
                "The .acsproject manifest is not valid JSON.",
                error);
        }

        using (document)
        {
            if (document.RootElement.ValueKind != JsonValueKind.Object)
            {
                throw new InvalidDataException(
                    "The .acsproject manifest root must be a JSON object.");
            }
            ValidateUniqueJsonProperties(document.RootElement);
        }
    }

    private static void ValidateUniqueJsonProperties(JsonElement element)
    {
        if (element.ValueKind == JsonValueKind.Object)
        {
            var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (JsonProperty property in element.EnumerateObject())
            {
                if (!names.Add(property.Name))
                {
                    throw new InvalidDataException(
                        "The .acsproject manifest contains a duplicate JSON property.");
                }
                ValidateUniqueJsonProperties(property.Value);
            }
            return;
        }

        if (element.ValueKind != JsonValueKind.Array)
            return;
        foreach (JsonElement item in element.EnumerateArray())
            ValidateUniqueJsonProperties(item);
    }

    private static void ValidateManifestDto(ManifestDto dto)
    {
        if (dto.version != CurrentProjectManifestVersion)
        {
            throw new InvalidDataException(
                $"Unsupported .acsproject manifest version {dto.version}. " +
                $"This editor supports version {CurrentProjectManifestVersion}.");
        }

        ValidateManifestText(dto.name, "name", 256);
        ValidateManifestText(dto.engineVersion, "engineVersion", 128);
        ValidateManifestText(dto.template, "template", 64);
        ValidateManifestText(dto.initialScene, "initialScene", 1024);

        string canonicalSceneAssetId = dto.canonicalSceneAssetId ?? "";
        if (canonicalSceneAssetId.Length != 0 &&
            (!Guid.TryParseExact(
                 canonicalSceneAssetId,
                 "N",
                 out Guid parsedAssetId) ||
             parsedAssetId == Guid.Empty))
        {
            throw new InvalidDataException(
                "The .acsproject canonicalSceneAssetId must be empty for a legacy project " +
                "or contain a non-zero 32-digit Asset GUID.");
        }
    }

    private static void ValidateManifestText(
        string? value,
        string field,
        int maximumCharacters)
    {
        if (value == null)
            return;
        if (value.Length > maximumCharacters)
        {
            throw new InvalidDataException(
                $"The .acsproject {field} exceeds {maximumCharacters} characters.");
        }
        int offset = 0;
        while (offset < value.Length)
        {
            OperationStatus status = Rune.DecodeFromUtf16(
                value.AsSpan(offset),
                out Rune rune,
                out int consumed);
            if (status != OperationStatus.Done || consumed <= 0)
            {
                throw new InvalidDataException(
                    $"The .acsproject {field} is not well-formed UTF-16.");
            }
            System.Globalization.UnicodeCategory category =
                Rune.GetUnicodeCategory(rune);
            if (category is
                System.Globalization.UnicodeCategory.Control or
                System.Globalization.UnicodeCategory.Format or
                System.Globalization.UnicodeCategory.LineSeparator or
                System.Globalization.UnicodeCategory.ParagraphSeparator)
            {
                throw new InvalidDataException(
                    $"The .acsproject {field} contains a control or formatting character.");
            }
            offset += consumed;
        }
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
        ValidateManifestDto(dto);
        byte[] serialized = StrictUtf8NoBom.GetBytes(
            JsonSerializer.Serialize(dto, JsonOpts) + Environment.NewLine);
        if (serialized.LongLength > MaxProjectManifestBytes)
        {
            throw new InvalidDataException(
                $"The .acsproject manifest exceeds {MaxProjectManifestBytes} bytes.");
        }

        SceneSourceFile.ValidateProjectRootDirectory(p.RootDir);
        ReferenceFileSnapshot snapshot = CaptureOptionalOrdinaryFile(
            p.ProjectFilePath,
            MaxProjectManifestBytes,
            ".acsproject manifest");
        string temporary = CreateSiblingTemporaryPath(snapshot.Path);
        try
        {
            WriteTemporaryBytes(temporary, serialized);
            PublishTemporary(temporary, snapshot.Path, snapshot);
        }
        finally
        {
            TryDeleteOrdinaryFile(temporary);
        }
    }

    // ===== 最近使ったプロジェクト (%APPDATA%/AcsEditor/recents.txt、新しい順) =====
    private static string RecentsFile =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "AcsEditor", "recents.txt");
    internal const int MaxRecentProjectListBytes = 512 * 1024;

    public static List<string> GetRecents()
    {
        return GetRecentPathsSnapshot()
            .Where(File.Exists)
            .ToList();
    }

    /// <summary>
    /// Reads the bounded recent-path file without probing each destination.
    /// The launcher performs availability checks on its filesystem worker so
    /// an offline UNC entry cannot block WPF's Dispatcher.
    /// </summary>
    internal static List<string> GetRecentPathsSnapshot()
    {
        try
        {
            if (!File.Exists(RecentsFile)) return new List<string>();
            ReferenceFileSnapshot snapshot = CaptureRequiredOrdinaryFile(
                RecentsFile,
                MaxRecentProjectListBytes,
                "recent project list",
                requireWritable: false);
            return ParseRecentPathsSnapshot(snapshot.Bytes);
        }
        catch { return new List<string>(); }
    }

    internal static List<string> ParseRecentPathsSnapshot(
        ReadOnlySpan<byte> bytes)
    {
        if (bytes.Length > MaxRecentProjectListBytes)
        {
            throw new InvalidDataException(
                $"recent project list exceeds {MaxRecentProjectListBytes} bytes.");
        }
        string source = StrictUtf8NoBom.GetString(bytes);
        if (source.Length != 0 && source[0] == '\uFEFF')
            source = source[1..];
        return source
            .Split(
                ["\r\n", "\n", "\r"],
                StringSplitOptions.RemoveEmptyEntries)
            .Where(line => !string.IsNullOrWhiteSpace(line))
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .Take(10)
            .ToList();
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

    public static readonly string[] BaseClassOptions = { "Empty", "AObject", "AComponent", "ANode", "FScene2D" };

    /// <summary>選択可能なエンジン基底 (これ以外の基底はユーザー型として扱う)。</summary>
    private static readonly HashSet<string> EngineBaseClasses = new() { "AObject", "AComponent", "ANode", "FScene2D" };

    /// <summary>既存の登録処理を保つobject基底。FScene2Dはscene専用登録へ分けるまで含める。</summary>
    private static readonly HashSet<string> RegisteredObjectBaseClasses = new() { "AObject", "AComponent", "ANode", "FScene2D" };

    /// <summary>baseClassからユーザー継承鎖を辿り、登録方針を決める既知rootを返す。</summary>
    /// <exception cref="InvalidOperationException">基底が不明または継承が循環している。</exception>
    private static string ResolveRootEngineBase(
        IReadOnlyDictionary<string, string> userClasses,
        string baseClass)
    {
        string cur = baseClass;
        var seen = new HashSet<string>();
        while (true)
        {
            if (EngineBaseClasses.Contains(cur)) return cur;
            if (string.Equals(cur, "Empty", StringComparison.OrdinalIgnoreCase)) return "Empty";
            if (!seen.Add(cur)) throw new InvalidOperationException($"基底classの継承が循環しています: {baseClass}");
            if (!userClasses.TryGetValue(cur, out var next))
                throw new InvalidOperationException($"基底classを解決できません: {baseClass}");
            cur = next;
        }
    }

    // 行頭の class [API] Name [final] [: public Base] { を捉える。
    private static readonly Regex UserClassRe = new(
        @"^[ \t]*class[ \t]+(?:[A-Za-z_]\w*_API[ \t]+)?([A-Za-z_]\w*)(?:[ \t]+final)?(?:[ \t]*:[ \t]*public[ \t]+([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*))?[ \t\r\n]*\{",
        RegexOptions.Compiled | RegexOptions.Multiline);

    /// <summary>class keywordの直前が空白を挟んだenum keywordかを返す。</summary>
    private static bool IsEnumClassSpecifier(string text, int classPosition)
    {
        int end = classPosition;
        while (end > 0 && char.IsWhiteSpace(text[end - 1])) --end;
        int start = end;
        while (start > 0 && (char.IsLetterOrDigit(text[start - 1]) || text[start - 1] == '_')) --start;
        return text.AsSpan(start, end - start).SequenceEqual("enum".AsSpan());
    }

    /// <summary>文字がC++数値リテラル内の桁区切りかを返す。</summary>
    private static bool IsNumericLiteralSeparator(string text, int position)
    {
        if (position <= 0 || position + 1 >= text.Length) return false;
        static bool IsTokenCharacter(char value) =>
            (value >= 'A' && value <= 'Z')
            || (value >= 'a' && value <= 'z')
            || (value >= '0' && value <= '9')
            || value is '_' or '\'';
        if (!IsTokenCharacter(text[position - 1]) || !IsTokenCharacter(text[position + 1])) return false;
        int start = position - 1;
        while (start > 0 && IsTokenCharacter(text[start - 1])) --start;
        return text[start] >= '0' && text[start] <= '9';
    }

    /// <summary>positionから始まるC++ raw文字列の終端を返す。</summary>
    private static bool TryFindCppRawStringEnd(string text, int position, out int end)
    {
        end = position;
        // raw文字列のR文字がある位置。
        int rawMarker = position;
        if (position + 3 < text.Length
            && text[position] == 'u'
            && text[position + 1] == '8'
            && text[position + 2] == 'R'
            && text[position + 3] == '"')
        {
            rawMarker = position + 2;
        }
        else if (position + 2 < text.Length
            && text[position] is 'u' or 'U' or 'L'
            && text[position + 1] == 'R'
            && text[position + 2] == '"')
        {
            rawMarker = position + 1;
        }
        else if (position + 1 >= text.Length
            || text[position] != 'R'
            || text[position + 1] != '"')
        {
            return false;
        }

        if (position > 0
            && (char.IsLetterOrDigit(text[position - 1]) || text[position - 1] == '_'))
        {
            return false;
        }

        // raw文字列delimiterの開始位置。
        int delimiterStart = rawMarker + 2;
        int open = text.IndexOf('(', delimiterStart);
        bool validDelimiter = open >= delimiterStart && open - delimiterStart <= 16;
        for (int delimiterPosition = delimiterStart;
            validDelimiter && delimiterPosition < open;
            ++delimiterPosition)
        {
            char delimiterCharacter = text[delimiterPosition];
            validDelimiter = !char.IsWhiteSpace(delimiterCharacter)
                && delimiterCharacter is not ('\\' or ')');
        }
        if (!validDelimiter) return false;

        string delimiter = text.Substring(delimiterStart, open - delimiterStart);
        string terminator = ")" + delimiter + "\"";
        int close = text.IndexOf(terminator, open + 1, StringComparison.Ordinal);
        end = close < 0 ? text.Length : close + terminator.Length;
        return true;
    }

    /// <summary>
    /// phase 2後のraw文字列を元source上の連続した終端まで対応付ける。
    /// raw内容では行結合が戻るため、終端探索だけは元sourceで行う。
    /// </summary>
    private static bool TryFindCppRawStringInPhase2(
        string phase2Text,
        IReadOnlyList<int> originalPositions,
        string originalText,
        int position,
        out int phase2End)
    {
        phase2End = position;
        // phase 2文字列内でraw prefixのRがある位置。
        int rawMarker = position;
        if (position + 3 < phase2Text.Length
            && phase2Text[position] == 'u'
            && phase2Text[position + 1] == '8'
            && phase2Text[position + 2] == 'R'
            && phase2Text[position + 3] == '"')
        {
            rawMarker = position + 2;
        }
        else if (position + 2 < phase2Text.Length
            && phase2Text[position] is 'u' or 'U' or 'L'
            && phase2Text[position + 1] == 'R'
            && phase2Text[position + 2] == '"')
        {
            rawMarker = position + 1;
        }
        else if (position + 1 >= phase2Text.Length
            || phase2Text[position] != 'R'
            || phase2Text[position + 1] != '"')
        {
            return false;
        }

        if (position > 0
            && (char.IsLetterOrDigit(phase2Text[position - 1]) || phase2Text[position - 1] == '_'))
        {
            return false;
        }

        // phase 2文字列内のdelimiter開始位置。
        int delimiterStart = rawMarker + 2;
        int open = phase2Text.IndexOf('(', delimiterStart);
        bool validDelimiter = open >= delimiterStart && open - delimiterStart <= 16;
        for (int delimiterPosition = delimiterStart;
            validDelimiter && delimiterPosition < open;
            ++delimiterPosition)
        {
            char delimiterCharacter = phase2Text[delimiterPosition];
            validDelimiter = !char.IsWhiteSpace(delimiterCharacter)
                && delimiterCharacter is not ('\\' or ')');
        }
        if (!validDelimiter) return false;

        string delimiter = phase2Text.Substring(delimiterStart, open - delimiterStart);
        string terminator = ")" + delimiter + "\"";
        // raw内容の開始位置に対応する元source位置。
        int originalOpen = originalPositions[open];
        int originalClose = originalText.IndexOf(
            terminator,
            originalOpen + 1,
            StringComparison.Ordinal);
        int originalEnd = originalClose < 0
            ? originalText.Length
            : originalClose + terminator.Length;
        phase2End = position;
        while (phase2End < originalPositions.Count
            && originalPositions[phase2End] < originalEnd) ++phase2End;
        return true;
    }

    /// <summary>C++のcommentと文字列を改行位置を保った空白へ置換する。</summary>
    private static string MaskCppCommentsAndLiterals(string text)
    {
        char[] masked = text.ToCharArray();

        // 指定範囲を空白化し、行頭判定に必要な改行だけを残す。
        void MaskRange(int start, int end)
        {
            for (int position = start; position < end; ++position)
            {
                if (masked[position] is not ('\r' or '\n')) masked[position] = ' ';
            }
        }

        for (int position = 0; position < text.Length;)
        {
            if (position + 1 < text.Length && text[position] == '/' && text[position + 1] == '/')
            {
                int end = position + 2;
                while (end < text.Length)
                {
                    int lineEnd = text.IndexOf('\n', end);
                    if (lineEnd < 0)
                    {
                        end = text.Length;
                        break;
                    }
                    int last = lineEnd - 1;
                    if (last >= end && text[last] == '\r') --last;
                    if (last < end || text[last] != '\\')
                    {
                        end = lineEnd;
                        break;
                    }
                    end = lineEnd + 1;
                }
                MaskRange(position, end);
                position = end;
                continue;
            }
            if (position + 1 < text.Length && text[position] == '/' && text[position + 1] == '*')
            {
                int close = text.IndexOf("*/", position + 2, StringComparison.Ordinal);
                int end = close < 0 ? text.Length : close + 2;
                MaskRange(position, end);
                position = end;
                continue;
            }
            if (TryFindCppRawStringEnd(text, position, out int rawEnd))
            {
                MaskRange(position, rawEnd);
                position = rawEnd;
                continue;
            }
            if (text[position] == '\'' && IsNumericLiteralSeparator(text, position))
            {
                ++position;
                continue;
            }
            if (text[position] is '"' or '\'')
            {
                char quote = text[position];
                int end = position + 1;
                while (end < text.Length)
                {
                    if (text[end] == '\\' && end + 1 < text.Length)
                    {
                        end += 2;
                        continue;
                    }
                    if (text[end++] == quote) break;
                }
                MaskRange(position, end);
                position = end;
                continue;
            }
            ++position;
        }
        return new string(masked);
    }

    /// <summary>行が空白と改行だけかを返す。</summary>
    private static bool IsBlankCppLine(string text, int lineStart, int lineEnd)
    {
        for (int position = lineStart; position < lineEnd; ++position)
        {
            if (!char.IsWhiteSpace(text[position])) return false;
        }
        return true;
    }

    /// <summary>行頭のpreprocessor directiveを名前と引数へ分ける。</summary>
    private static bool TryReadCppDirective(
        string text,
        int lineStart,
        int lineEnd,
        out int marker,
        out string directiveName,
        out string argument)
    {
        marker = lineStart;
        while (marker < lineEnd && text[marker] is ' ' or '\t') ++marker;
        if (marker >= lineEnd || text[marker] != '#')
        {
            directiveName = "";
            argument = "";
            return false;
        }

        // directive名の先頭位置。
        int nameStart = marker + 1;
        while (nameStart < lineEnd && char.IsWhiteSpace(text[nameStart])) ++nameStart;
        // directive名の終端位置。
        int nameEnd = nameStart;
        while (nameEnd < lineEnd
            && (char.IsLetterOrDigit(text[nameEnd]) || text[nameEnd] == '_')) ++nameEnd;
        directiveName = text.Substring(nameStart, nameEnd - nameStart);
        argument = text.Substring(nameEnd, lineEnd - nameEnd).Trim();
        return true;
    }

    /// <summary>C++識別子だけで構成された文字列かを返す。</summary>
    private static bool IsCppIdentifier(string value)
    {
        if (value.Length == 0 || !(char.IsLetter(value[0]) || value[0] == '_')) return false;
        for (int position = 1; position < value.Length; ++position)
        {
            if (!(char.IsLetterOrDigit(value[position]) || value[position] == '_')) return false;
        }
        return true;
    }

    /// <summary>
    /// file全体を囲む単純な#ifndef/define guardだけを透過対象として認識する。
    /// 条件分岐や置換値を持つguardは安全側で通常の条件付き領域として扱う。
    /// </summary>
    private static bool HasTransparentHeaderGuard(string text)
    {
        // 空行を除いた各行の範囲。
        var meaningfulLines = new List<(int Start, int End)>();
        for (int lineStart = 0; lineStart < text.Length;)
        {
            // 現在行の改行を含む終端位置。
            int lineBreak = text.IndexOf('\n', lineStart);
            int lineEnd = lineBreak < 0 ? text.Length : lineBreak + 1;
            if (!IsBlankCppLine(text, lineStart, lineEnd)) meaningfulLines.Add((lineStart, lineEnd));
            lineStart = lineEnd;
        }
        if (meaningfulLines.Count < 3) return false;

        (int firstStart, int firstEnd) = meaningfulLines[0];
        if (!TryReadCppDirective(
                text,
                firstStart,
                firstEnd,
                out _,
                out string firstName,
                out string guardName)
            || firstName != "ifndef"
            || !IsCppIdentifier(guardName))
        {
            return false;
        }

        (int secondStart, int secondEnd) = meaningfulLines[1];
        if (!TryReadCppDirective(
                text,
                secondStart,
                secondEnd,
                out _,
                out string secondName,
                out string definedName)
            || secondName != "define"
            || !string.Equals(definedName, guardName, StringComparison.Ordinal))
        {
            return false;
        }

        // 外側guardを含む現在の条件深さ。
        int conditionalDepth = 0;
        for (int lineIndex = 0; lineIndex < meaningfulLines.Count; ++lineIndex)
        {
            (int lineStart, int lineEnd) = meaningfulLines[lineIndex];
            if (!TryReadCppDirective(
                    text,
                    lineStart,
                    lineEnd,
                    out _,
                    out string directiveName,
                    out _))
            {
                if (conditionalDepth == 0) return false;
                continue;
            }

            if (directiveName is "if" or "ifdef" or "ifndef")
            {
                ++conditionalDepth;
                continue;
            }
            if (directiveName is "else" or "elif")
            {
                if (conditionalDepth == 1) return false;
                continue;
            }
            if (directiveName != "endif") continue;
            if (conditionalDepth == 0) return false;
            --conditionalDepth;
            if (conditionalDepth == 0 && lineIndex != meaningfulLines.Count - 1) return false;
        }
        return conditionalDepth == 0;
    }

    /// <summary>preprocessor directiveと条件付き領域を走査対象から除外する。</summary>
    private static string MaskCppPreprocessorRegions(string text)
    {
        char[] masked = text.ToCharArray();
        // file全体の単純なinclude guardだけは本文を走査する。
        int visibleConditionalDepth = HasTransparentHeaderGuard(text) ? 1 : 0;
        int conditionalDepth = 0;
        int lineStart = 0;
        while (lineStart < text.Length)
        {
            int lineBreak = text.IndexOf('\n', lineStart);
            int lineEnd = lineBreak < 0 ? text.Length : lineBreak + 1;
            bool directive = TryReadCppDirective(
                text,
                lineStart,
                lineEnd,
                out int marker,
                out string directiveName,
                out _);
            if (directive)
            {
                if (directiveName is "endif" && conditionalDepth > 0) --conditionalDepth;
                if (directiveName is "if" or "ifdef" or "ifndef") ++conditionalDepth;
            }

            if (directive || conditionalDepth > visibleConditionalDepth)
            {
                for (int position = lineStart; position < lineEnd; ++position)
                {
                    if (masked[position] is not ('\r' or '\n')) masked[position] = ' ';
                }
                if (directive) masked[marker] = '#';
            }
            lineStart = lineEnd;
        }
        return new string(masked);
    }

    private enum CppScanState
    {
        Code,
        LineComment,
        BlockComment,
        StringLiteral,
        CharacterLiteral,
    }

    /// <summary>
    /// C++ phase 2の行結合を行い、comment/string文脈で実raw文字列だけを空白化する。
    /// raw内容の行結合は元sourceの終端位置を使って取り消す。
    /// </summary>
    private static string SpliceCppLinesPreservingRawStrings(string text)
    {
        var phase2Builder = new StringBuilder(text.Length);
        // phase 2文字ごとの元source位置。
        var originalPositions = new List<int>(text.Length);
        for (int position = 0; position < text.Length;)
        {
            if (position + 1 < text.Length && text[position] == '\\' && text[position + 1] == '\n')
            {
                position += 2;
                continue;
            }
            if (position + 2 < text.Length
                && text[position] == '\\'
                && text[position + 1] == '\r'
                && text[position + 2] == '\n')
            {
                position += 3;
                continue;
            }
            phase2Builder.Append(text[position]);
            originalPositions.Add(position);
            ++position;
        }

        string phase2Text = phase2Builder.ToString();
        char[] protectedText = phase2Text.ToCharArray();
        CppScanState state = CppScanState.Code;
        for (int position = 0; position < phase2Text.Length;)
        {
            if (state == CppScanState.Code)
            {
                if (position + 1 < phase2Text.Length
                    && phase2Text[position] == '/'
                    && phase2Text[position + 1] == '/')
                {
                    state = CppScanState.LineComment;
                    position += 2;
                    continue;
                }
                if (position + 1 < phase2Text.Length
                    && phase2Text[position] == '/'
                    && phase2Text[position + 1] == '*')
                {
                    state = CppScanState.BlockComment;
                    position += 2;
                    continue;
                }
                if (TryFindCppRawStringInPhase2(
                    phase2Text,
                    originalPositions,
                    text,
                    position,
                    out int rawEnd))
                {
                    for (int maskedPosition = position; maskedPosition < rawEnd; ++maskedPosition)
                    {
                        if (protectedText[maskedPosition] is not ('\r' or '\n'))
                            protectedText[maskedPosition] = ' ';
                    }
                    position = rawEnd;
                    continue;
                }
                if (phase2Text[position] == '"')
                {
                    state = CppScanState.StringLiteral;
                    ++position;
                    continue;
                }
                if (phase2Text[position] == '\''
                    && !IsNumericLiteralSeparator(phase2Text, position))
                {
                    state = CppScanState.CharacterLiteral;
                    ++position;
                    continue;
                }
                ++position;
                continue;
            }

            if (state == CppScanState.LineComment)
            {
                if (phase2Text[position] == '\n') state = CppScanState.Code;
                ++position;
                continue;
            }

            if (state == CppScanState.BlockComment)
            {
                if (position + 1 < phase2Text.Length
                    && phase2Text[position] == '*'
                    && phase2Text[position + 1] == '/')
                {
                    state = CppScanState.Code;
                    position += 2;
                    continue;
                }
                ++position;
                continue;
            }

            char quote = state == CppScanState.StringLiteral ? '"' : '\'';
            if (phase2Text[position] == '\\' && position + 1 < phase2Text.Length)
            {
                position += 2;
                continue;
            }
            if (phase2Text[position] == quote) state = CppScanState.Code;
            ++position;
        }
        return new string(protectedText);
    }

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
            string code = MaskCppPreprocessorRegions(MaskCppCommentsAndLiterals(
                SpliceCppLinesPreservingRawStrings(text)));
            foreach (Match m in UserClassRe.Matches(code))
            {
                if (IsEnumClassSpecifier(code, m.Index)) continue;
                string name = m.Groups[1].Value;
                string bas  = m.Groups[2].Success ? m.Groups[2].Value : "Empty";
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
        "// SPDX-License-Identifier: Apache-2.0\n" +
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
        && stem[0] is 'A' or 'C' or 'E' or 'F' or 'I' or 'T'
        && ((stem[1] >= 'A' && stem[1] <= 'Z') || (stem[1] >= '0' && stem[1] <= '9'));

    public static string CppTypeIdent(string? name, char requiredPrefix)
    {
        if (requiredPrefix is not ('A' or 'C' or 'E' or 'F'))
            throw new ArgumentOutOfRangeException(nameof(requiredPrefix));

        string stem = CppPascalStem(name);
        bool prefixOnly = stem.Length == 1 && stem[0] is 'A' or 'C' or 'E' or 'F' or 'I' or 'T';
        if (stem.Length == 0 || prefixOnly)
        {
            stem = requiredPrefix switch
            {
                'A' => "GeneratedObject",
                'C' => "GeneratedClass",
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

    /// <summary>通常の機能class用の互換入口。生成型にはC prefixを保証する。</summary>
    public static string ClassIdent(string name) => CppTypeIdent(name, 'C');

    /// <summary>
    /// 表示名を enum class の列挙子に使える ASCII PascalCase 識別子へ変換する。
    /// 型用のA/C/E/F/I/T prefixが付いていた場合は、列挙子との混同を避けて除去する。
    /// </summary>
    public static string CppEnumeratorIdent(string? name)
    {
        string stem = CppPascalStem(name);
        bool prefixOnly = stem.Length == 1 && stem[0] is 'A' or 'C' or 'E' or 'F' or 'I' or 'T';
        if (stem.Length == 0 || prefixOnly) return "Value";
        if (HasKnownCppPrefix(stem)) stem = stem.Substring(1);
        return stem[0] >= '0' && stem[0] <= '9' ? "Value" + stem : stem;
    }

    /// <summary>新しいクラス/コンポーネントのソースをプロジェクトの Source に生成し、生成パスを返す。</summary>
    public static System.Collections.Generic.List<string> GenerateClass(Project p, string className, string baseClass)
    {
        // 全生成前にユーザーclass定義を一度だけ走査して重複を拒否する。
        var userClasses = new Dictionary<string, string>();
        foreach (var (name, bas) in ScanUserClasses(p))
        {
            if (!userClasses.TryAdd(name, bas))
                throw new InvalidOperationException($"基底class定義が重複しています: {name}");
        }
        // 選択された基底がエンジンに直接定義されているか。
        bool baseIsEngine = EngineBaseClasses.Contains(baseClass);
        // ユーザー継承を含めて到達した既知のroot基底。
        string rootEngine = baseIsEngine
            ? baseClass
            : ResolveRootEngineBase(userClasses, baseClass);
        // 空class以外では基底のincludeと継承宣言を生成する。
        bool hasBase = !string.Equals(baseClass, "Empty", StringComparison.OrdinalIgnoreCase);
        // ACSの登録対象としてマーカーを生成するか。
        bool registeredObject = RegisteredObjectBaseClasses.Contains(rootEngine);
        string cls = CppTypeIdent(className, registeredObject ? 'A' : 'C');
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

        if (!hasBase)
        {
            File.WriteAllText(hPath,
                "// SPDX-License-Identifier: Apache-2.0\n" +
                "#pragma once\n\n" +
                $"// {cls} — ACS Editor が生成した空クラス。\n" +
                $"class {cls}\n{{\n}};\n", Utf8NoBom);
            made.Add(hPath);
            return made;
        }

        // 基底ありclass。objectだけACS_CLASSを付け、通常の機能classは継承関係だけを生成する。
        // エンジン基底は定義元namespaceで修飾し、ユーザー基底はそのheaderを直接読む。
        string baseQual = baseClass == "AObject"
            ? "acs::AObject"
            : baseIsEngine ? "acs::game::" + baseClass : baseClass;
        bool isComponent  = rootEngine == "AComponent";   // 直/間接に AComponent 由来か
        var h = new StringBuilder();
        h.Append("// SPDX-License-Identifier: Apache-2.0\n");
        h.Append("#pragma once\n");
        if (registeredObject)
        {
            if (baseClass == "AObject") h.Append("#include \"memory/AObject.h\"\n");
            else h.Append("#include \"gameframework/GameFramework.h\"\n");
            h.Append("#include \"gameframework/Reflect.h\"\n");
            h.Append("#include \"gameframework/AcsClass.h\"\n");
        }
        h.Append($"#include \"{apiHeader}\"\n");
        if (!baseIsEngine) h.Append($"#include \"{baseClass}.h\"\n");   // ユーザー基底のヘッダ
        h.Append("\n");
        h.Append($"// {cls} — ACS Editor が生成 (基底: {baseClass})。\n");
        if (registeredObject) h.Append("ACS_CLASS()\n");
        h.Append($"class {api} {cls} : public {baseQual}\n{{\npublic:\n");
        if (!registeredObject)
        {
            h.Append("    // このclassが担う機能と、保持する状態のownerを明示して実装する。\n");
        }
        else if (isComponent)
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

    private const string BlankScene3D = "ACS3D v2\n";

    // The 2D starter is ordinary 3D geometry viewed through the XY-front
    // Orthographic preset. It can be rotated back into Perspective at any time.
    private const string Template2DScene3D =
        "ACS3D v2\n" +
        "N3D 1 -1 0 0.0000 220.0000 0.0000 0.0000 0.0000 0.0000 520.0000 14.0000 40.0000 0.2500 0.2800 0.3400 1.0000 Ground\n" +
        "N3D 2 -1 0 0.0000 -160.0000 0.0000 0.0000 0.0000 0.0000 48.0000 48.0000 48.0000 0.1500 0.8500 1.0000 1.0000 Player\n" +
        "N3D 3 -1 0 -260.0000 40.0000 0.0000 0.0000 0.0000 0.0000 20.0000 240.0000 40.0000 0.2200 0.2400 0.3000 1.0000 WallLeft\n" +
        "SEL3D -1\n";

    // blank / 2d 共通: エディタで保存した main.acscene を読み込んで表示するスタンドアロン。
    // editor は world=pixel で扱うので PixelsPerUnit=1、読み込んだ境界にカメラを合わせる。
    private const string SceneLoaderSource =
        "// SPDX-License-Identifier: Apache-2.0\n" +
        "// ACS_RUNTIME_CAPABILITY: LEGACY_SCENE3D=1\n" +
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

    /// <summary>
    /// Emits the stock standalone runtime only for the retained TEMP-only
    /// distribution audit. Ordinary projects own Source/Game.cpp and this
    /// helper must never be used as a repair or migration path.
    /// </summary>
    internal static void WriteGeneratedRuntimeSourceForDistributionAudit(
        Project project)
    {
        ArgumentNullException.ThrowIfNull(project);
        string source = Path.Combine(project.RootDir, "Source");
        Directory.CreateDirectory(source);
        File.WriteAllText(
            Path.Combine(source, "Game.cpp"),
            SceneLoaderSource,
            Utf8NoBom);
        File.WriteAllText(
            Path.Combine(source, "CMakeLists.txt"),
            CMakeTemplate(SanitizeIdent(project.Name), project.RootDir),
            Utf8NoBom);
        File.WriteAllText(
            Path.Combine(source, ApiHeaderName(project.Name)),
            ApiHeaderContent(SanitizeIdent(project.Name)),
            Utf8NoBom);
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
