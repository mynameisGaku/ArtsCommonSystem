using System.IO;

namespace AcsEditor;

/// <summary>
/// 開いている / 作成したプロジェクトのメタデータ。フォルダ規約:
///   &lt;RootDir&gt;/&lt;Name&gt;.acsproject   … マニフェスト (JSON)
///   &lt;RootDir&gt;/Assets/                  … 画像・音声・データ
///   &lt;RootDir&gt;/Source/                  … ゲームの C++ ソース
///   &lt;RootDir&gt;/Temp/                    … エディタキャッシュ・ビルド中間物
/// </summary>
public sealed class Project
{
    public int    Version        { get; set; } = 1;
    public string Name           { get; set; } = "";
    public string EngineVersion  { get; set; } = "";
    public string Template       { get; set; } = "3d"; // new: "3d" | "2d"; legacy manifests may retain "blank"
    public string InitialScene   { get; set; } = "Assets/main.acscene";   // RootDir 相対
    public string CanonicalSceneAssetId { get; set; } = ""; // Persistent Asset DB identity; path is legacy adapter.

    /// <summary>マニフェスト .acsproject の絶対パス (シリアライズ対象外)。</summary>
    public string ProjectFilePath { get; set; } = "";

    public string RootDir   => Path.GetDirectoryName(ProjectFilePath) ?? "";
    public string AssetsDir => Path.Combine(RootDir, "Assets");
    public string SourceDir => Path.Combine(RootDir, "Source");
    public string TempDir   => Path.Combine(RootDir, "Temp");

    /// <summary>初期シーンの絶対パス (相対パスのセパレータを OS 用に正規化)。</summary>
    public string InitialScenePath =>
        Path.GetFullPath(
            Path.Combine(
                RootDir,
                InitialScene.Replace('/', Path.DirectorySeparatorChar)));
}
