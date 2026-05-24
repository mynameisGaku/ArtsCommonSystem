// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — ModelViewer / ModelViewerPanel (Phase 21b 第一弾)
//
// 3D model の表示と asset 切替を行う **メインビューポート**。Phase 21a で投入
// された editor 共通基盤 (`editor_core::EditorPanel` / `EditorCamera` /
// `EditorWorkspace` / `AssetBrowser`) を最初に dogfood する panel として位置
// づけられ、後続の AnimCurveEditor / BehaviorTreeEditor / LevelEditor 等が
// 同様の継承形を参考にする想定。
//
// 役割:
//   ・3D model viewport の host (ImGui::Begin "Model Viewport" 1 window)
//   ・カレント asset path の保持 + 切替 (`LoadModelAsset` / `ClearModel`)
//   ・EditorCamera (3D orbit) を内包し、panel 内で操作 / Reset / Frame
//   ・Lighting (sun dir + sun color + IBL toggle + tonemap mode) パラメータの
//     設定 ImGui controls
//   ・Background color / Grid / Bone skeleton 表示の切替
//   ・AssetBrowser からのファイル選択通知 (`OnAssetSelected`) を受けて
//     mesh / model 拡張子なら自動 LoadModelAsset
//
// 役割分担 (Phase 21b ModelViewer 全体):
//   ・本 panel (ModelViewerPanel)         : 3D viewport + 表示パラメータ UI
//   ・ModelInspectorPanel (別エージェント): モデルのメタ情報 (vertex/index 数,
//                                            material list, bone count 等)
//   ・ModelMaterialPanel  (別エージェント): material slot 編集 (basecolor /
//                                            metallic / roughness 等)
//   ・ModelAnimationPanel (別エージェント): animation clip 切替 + 再生 / pause
//   ・Sample 31 ModelViewer demo          : 上記 4 panel を Workspace に組み立て
//   ・ModelViewportRenderer (将来)        : 実際の 3D 描画 (PbrShader + ShadowMap +
//                                            IBL + Tonemap) を持つ別クラス。本 panel
//                                            は描画パラメータの保管だけを担う。
//
// 使い方 (典型):
//   ModelViewerPanel viewer;
//   viewer.Init();
//   workspace.RegisterPanel(&viewer);  // EditorPanel として登録
//
//   // 毎フレーム TickAllPanels(dt) の中で OnFrameBegin + DrawUI が呼ばれる。
//
//   // AssetBrowser からファイル選択時:
//   workspace.BroadcastAssetSelected("models/hero.mdl");
//   // → ModelViewerPanel::OnAssetSelected が呼ばれ、自動 LoadModelAsset。
//
//   // 終了時:
//   workspace.UnregisterPanel(&viewer);
//   viewer.Shutdown();
//
// 設計選択 (Phase 21b ModelViewer 第一弾):
//   ・**EditorPanel 継承**: Phase 21a 共通基盤の最初の dogfood。Title / DrawUI /
//     OnInit / OnAssetSelected を override。OnInit では基底実装 (`Workspace()`
//     ポインタ保持) を必ず呼ぶ。
//   ・**3D viewport を「数値パラメータ + ImGui controls」だけにする**:
//     実際の 3D 描画 (PbrShader / ShadowMap / IBL) は Sample 31 側の
//     `ModelViewportRenderer` (将来クラス) が担当する。本 panel は
//     「Camera state + light params + background + toggle 群」の保管 + UI のみ。
//     こうしておけば panel 単体テストや、別の renderer (= raymarched preview /
//     OBJ thumbnail) への差し替えが容易。
//   ・**EditorCamera を内包 (値メンバ)**: 各 panel が独自 camera を持つ Unity
//     SceneView 風モデル。Camera() アクセサで参照を返し、Sample 側の renderer が
//     ViewMatrix / ProjectionMatrix を取り出して使う。
//   ・**asset path は wchar_t バッファ (kMaxPathChars = 512)**: AssetBrowser
//     と同じ規約。STL の std::wstring は使えないため、固定長で保持。
//     寿命管理を panel 側で完結させるため、コピー保存する (= AssetBrowser の
//     pointer 寿命 = 次の Refresh まで、には依存しない)。
//   ・**OnAssetSelected で拡張子フィルタ**: `.mdl` / `.fbx` / `.gltf` / `.glb` /
//     `.obj` が Mesh asset とみなされる (AssetBrowser::ClassifyByExtension の
//     Mesh 分類と一致)。それ以外の拡張子は無視 (= asset path は ASCII UTF-8 だが
//     panel 内では wchar_t に正規化して持つ)。
//   ・**ImGui controls の DrawUI 配置**: 単一 "Model Viewport" window 内に
//       - viewport 領域 (大半の面積、将来 renderer が描画 / 現状は dummy)
//       - 下部 control bar: Light dir / color / IBL / Tonemap / Background /
//         Grid / Bone skeleton toggle
//     を出す。各 widget の戻り値で「ユーザが変更したか」を判定し、外部に通知
//     する手段は将来 callback で追加予定 (Phase 21b では本 panel が値を保持
//     するだけ)。
//   ・**Tonemap mode は u32 enum 風 (0=ACES / 1=Reinhard / 2=Linear)**:
//     PbrShader / Tonemap の既存 enum (`render/PostProcess.h` 等) との連携は
//     Sample 31 側で行う (本 panel は u32 で受け流し)。
//   ・**Background color は Vec4** (RGBA): renderer の clear color 用。alpha は
//     通常 1.0 だが、screenshot 用 alpha 0 利用も将来想定。
//   ・**非コピー / 非ムーブ / 全 noexcept / STL 不使用 / `<string>` 禁止**:
//     ACS 規約。文字列は wchar_t 固定長バッファのみ。
//   ・**ImGui ヘッダは .cpp 限定**: 他 panel (HierarchyPanel / InspectorPanel /
//     ParticleEditorPanel) と同形。
//   ・**Phase 21b 第一弾 = 別 4 panel が並列で実装中**: ModelInspectorPanel /
//     ModelMaterialPanel / ModelAnimationPanel / Sample 31。本 panel から
//     forward decl で他 panel を受ける必要はない (Workspace 経由で疎結合)。
//
// 範囲外 (将来 / 別 panel):
//   ・PBR 描画 (PbrShader / ShadowMap / IBL) の実呼出 = ModelViewportRenderer (将来)
//   ・material slot 編集 = ModelMaterialPanel (別エージェント)
//   ・bone hierarchy 表示 = ModelInspectorPanel (別エージェント)
//   ・animation timeline = ModelAnimationPanel (別エージェント)
//   ・grid / gizmo の実描画 = EditorGizmo (Phase 21a 既存) を Sample 31 で組合せ
//   ・camera preset (Front / Top / Side) = EditorCamera 将来拡張 (ヘッダ末尾 TODO)
//   ・undo / redo = Phase 21b UndoStack 統合 (将来 OnUndo / OnRedo hook)
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "gameframework/tools/editor_core/EditorPanel.h"
#include "gameframework/tools/editor_core/EditorCamera.h"

namespace acs::game::editor_core {
// EditorWorkspace / AssetBrowser は EditorPanel.h から forward-decl で受ける。
// 本ヘッダで AssetBrowser.h を include しないことで、編集中ファイルの依存を
// 最小化する (= AssetBrowser のヘッダ変更で ModelViewerPanel 利用側が再ビルドを
// 強いられないようにする)。
class EditorWorkspace;
} // namespace acs::game::editor_core

namespace acs::game::modelview {

// ---------------------------------------------------------------------------
// ModelViewerPanel — 3D model viewport + 表示パラメータ UI
// ---------------------------------------------------------------------------
class ModelViewerPanel : public acs::game::editor_core::EditorPanel {
public:
    ModelViewerPanel() noexcept = default;
    ~ModelViewerPanel() noexcept override = default;

    // 非コピー・非ムーブ: 内部 EditorCamera (非コピー / 非ムーブ) + asset path
    // バッファの所有を曖昧にしない (ACS 規約 + 基底 EditorPanel 規約)。
    ModelViewerPanel(const ModelViewerPanel&)            = delete;
    ModelViewerPanel& operator=(const ModelViewerPanel&) = delete;
    ModelViewerPanel(ModelViewerPanel&&)                 = delete;
    ModelViewerPanel& operator=(ModelViewerPanel&&)      = delete;

    // ----- 初期化 / 解放 ---------------------------------------------------

    // 内部 state をデフォルトに、EditorCamera を 3D mode で Init、asset path を
    // 空に。Workspace への登録は別途 `EditorWorkspace::RegisterPanel(&this)` で
    // 行う (= OnInit が間接的に呼ばれる、その際 Workspace ポインタが保存される)。
    // 多重 Init 可 (= 完全リセット)。
    void Init() noexcept;

    // 内部 state を全解放。EditorCamera は POD のためそのまま放置で良いが、
    // 念のため Reset を呼んでデフォルト state にする。
    // OnShutdown とは別物 (= Workspace から外す前に panel 単体で reset したい
    // 場合の API)。多重 Shutdown 可。
    void Shutdown() noexcept;

    // ----- モデル切替 ------------------------------------------------------

    // wchar_t* path をコピーして内部バッファに保存する。`HasModel()` が true に
    // 切替わる。`asset_path == nullptr` または空文字なら `ClearModel()` 相当。
    // 実際の mesh 読込 / GPU upload は本 panel では行わず、外部 (Sample 31 の
    // ModelViewportRenderer) が `CurrentAssetPath()` を見て読み込む規約。
    void LoadModelAsset(const wchar_t* asset_path) noexcept;

    // 内部 asset path を空文字に戻し、`HasModel()` を false にする。
    // EditorCamera 状態 / Lighting / Background は保持 (= モデルを切替えても
    // 視点と照明が維持される、Unity SceneView と同じ挙動)。
    void ClearModel() noexcept;

    // 現在 model asset がロード対象としてセットされているか (= asset path が
    // 空文字でない)。実 GPU リソースの有無とは独立 (= 外部 renderer の責任範疇)。
    bool HasModel() const noexcept;

    // 現在の asset path (wchar_t)。未設定 / Clear 後は L"" を返す
    // (nullptr ではなく空文字を返すのは、呼び出し側が常に終端 wchar_t* として
    // 安全に扱えるようにするため)。
    const wchar_t* CurrentAssetPath() const noexcept;

    // ----- EditorCamera ----------------------------------------------------

    // 内部 EditorCamera への参照。呼出側 (renderer / panel 内 UI) が
    // `HandleMouseInput` / `Tick` / `ViewMatrix` 等を呼ぶ。
    // 寿命は本 panel の寿命と同一。
    acs::game::editor_core::EditorCamera& Camera() noexcept;

    // ----- Lighting --------------------------------------------------------

    // sun light direction (normalized 推奨だが panel 内では正規化しない =
    // renderer 側が必要なら Normalize する)。default = (0.3, -0.7, 0.6) (= 太陽が
    // やや右上から差す典型的 3-quarter ライト)。
    void SetLightDirection(acs::Vec3 dir) noexcept;
    acs::Vec3 LightDirection() const noexcept;

    // sun light color (RGB, linear space, 通常 1.0 中心)。default = white (1,1,1)。
    // HDR 強度を出したい場合は >1.0 を入れる (renderer 側で乗算される想定)。
    void SetLightColor(acs::Vec3 color) noexcept;
    acs::Vec3 LightColor() const noexcept;

    // IBL (environment-based indirect lighting) を有効にするか。
    // default = true (= sample 25 HelloIbl と同じ既定 ON)。
    // 無効にすると renderer 側は env / irradiance / prefilter map をサンプルしない。
    void SetIblEnabled(bool b) noexcept;
    bool IsIblEnabled() const noexcept;

    // tonemap mode: 0=ACES (default) / 1=Reinhard / 2=Linear (= no tonemap)
    // u32 にして renderer 側 enum と疎結合 (Sample 31 の renderer が
    // `render/PostProcess.h` の ETonemapMode に変換)。
    // 範囲外値は無視 (= 既存値を維持) する規約。
    void SetToneMappingMode(u32 mode) noexcept;
    u32  ToneMappingMode() const noexcept;

    // ----- 背景色 ----------------------------------------------------------

    // viewport background color (RGBA, linear space)。default =
    // (0.15, 0.15, 0.18, 1.0) (= editor 風暗グレー)。renderer 側で
    // clear color として使う。alpha は通常 1.0 だが screenshot 用 0 もあり得る。
    void SetBackgroundColor(acs::Vec4 color) noexcept;
    acs::Vec4 BackgroundColor() const noexcept;

    // ----- 表示 toggle -----------------------------------------------------

    // grid (XZ 平面のチェッカー) を描くか。default = true。
    bool ShowGrid() const noexcept;
    void SetShowGrid(bool b) noexcept;

    // bone skeleton を line overlay で描くか。default = false (= モデルが
    // skeletal でない場合に視覚的ノイズになるため OFF が無難)。
    bool ShowBoneSkeleton() const noexcept;
    void SetShowBoneSkeleton(bool b) noexcept;

    // ----- EditorPanel override -------------------------------------------

    // window タイトル (ImGui::Begin の引数兼 ID)。固定リテラル。
    const char* Title() const noexcept override { return "Model Viewport"; }

    // Workspace への登録時に呼ばれる。基底実装で Workspace ポインタ保存、
    // 本クラスでは追加初期化 (EditorCamera を 3D mode で Init し直す保険) を行う。
    void OnInit(acs::game::editor_core::EditorWorkspace& workspace) noexcept override;

    // ImGui::Begin "Model Viewport" + viewport プレースホルダ + control bar 描画。
    // `IsVisible()` が false なら早期 return (= close ボタンで隠せる)。
    // 実 3D 描画は外部 renderer の責務 (本 panel は dummy area + 控えめな
    // overlay text のみ描く)。
    void DrawUI() noexcept override;

    // AssetBrowser からのファイル選択通知。拡張子が Mesh 相当
    // (.mdl/.fbx/.gltf/.glb/.obj) なら自動 LoadModelAsset。
    // それ以外 (texture / font / particle 等) は無視。
    void OnAssetSelected(const char* asset_path) noexcept override;

    // ----- 公開定数 --------------------------------------------------------

    // asset path 用バッファ長 (wchar_t 単位、終端含む)。AssetBrowser::kMaxPathChars
    // と同値にして相互運用しやすくする (= 同じ規約)。
    static constexpr u32 kMaxAssetPathChars = 512u;

    // ToneMappingMode の有効範囲 (= 0,1,2)。これを超える値が来たら無視する。
    static constexpr u32 kToneMappingModeCount = 3u;

private:
    // 3D viewport camera (orbit / pan / dolly)。Init() で Mode3D に初期化。
    acs::game::editor_core::EditorCamera _camera {};

    // 現在のモデル asset path (UTF-16)。未設定時は先頭が L'\0'。
    // コピー所有 (= AssetBrowser の文字列寿命に依存しない)。
    wchar_t _asset_path[kMaxAssetPathChars] = {};

    // ---- Lighting 状態 ----
    // 既定値はヘッダコメントの設計選択節と一致 (sun が右上から斜めに差す形)。
    acs::Vec3 _light_dir   {0.3f, -0.7f, 0.6f};   // direction (renderer 側で正規化)
    acs::Vec3 _light_color {1.0f, 1.0f, 1.0f};    // RGB linear
    bool      _ibl_enabled = true;
    u32       _tonemap_mode = 0u;                  // 0=ACES, 1=Reinhard, 2=Linear

    // ---- 背景 / 表示 toggle ----
    acs::Vec4 _bg_color    {0.15f, 0.15f, 0.18f, 1.0f};
    bool      _show_grid           = true;
    bool      _show_bone_skeleton  = false;
};

} // namespace acs::game::modelview
