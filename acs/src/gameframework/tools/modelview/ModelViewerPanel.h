// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — ModelViewer / AModelViewerPanel
//
// 3D model の表示と asset 切替を行う **メインビューポート**。editor 共通基盤
// (`editor_core::AEditorPanel` / `CEditorCamera` / `CEditorWorkspace` /
// `CAssetBrowser`) を継承する panel。
//
// 役割:
//   ・3D model viewport の host (ImGui::Begin "Model Viewport" 1 window)
//   ・カレント asset path の保持 + 切替 (`LoadModelAsset` / `ClearModel`)
//   ・CEditorCamera (3D orbit) を内包し、panel 内で操作 / Reset / Frame
//   ・Lighting (sun dir + sun color + IBL toggle + tonemap mode) パラメータの
//     設定 ImGui controls
//   ・Background color / Grid / FBone skeleton 表示の切替
//   ・CAssetBrowser からのファイル選択通知 (`OnAssetSelected`) を受けて
//     mesh / model 拡張子なら自動 LoadModelAsset
//
// 役割分担 (ModelViewer 全体):
//   ・本 panel (AModelViewerPanel)         : 3D viewport + 表示パラメータ UI
//   ・AModelInspectorPanel                  : モデルのメタ情報 (vertex/index 数,
//                                            material list, bone count 等)
//   ・AModelMaterialPanel                   : material slot 編集 (basecolor /
//                                            metallic / roughness 等)
//   ・AModelAnimationPanel                  : animation clip 切替 + 再生 / pause
//   ・ModelViewportRenderer                 : 実際の 3D 描画 (CPbrShader + CShadowMap +
//                                            IBL + Tonemap) を持つ別クラス。本 panel
//                                            は描画パラメータの保管だけを担う。
//
// 使い方 (典型):
//   AModelViewerPanel viewer;
//   viewer.Init();
//   workspace.RegisterPanel(&viewer);  // AEditorPanel として登録
//
//   // 毎フレーム TickAllPanels(dt) の中で OnFrameBegin + DrawUI が呼ばれる。
//
//   // CAssetBrowser からファイル選択時:
//   workspace.BroadcastAssetSelected("models/hero.mdl");
//   // → AModelViewerPanel::OnAssetSelected が呼ばれ、自動 LoadModelAsset。
//
//   // 終了時:
//   workspace.UnregisterPanel(&viewer);
//   viewer.Shutdown();
//
// 設計選択 (ModelViewer):
//   ・**AEditorPanel 継承**: Title / DrawUI /
//     OnInit / OnAssetSelected を override。OnInit では基底実装 (`Workspace()`
//     ポインタ保持) を必ず呼ぶ。
//   ・**3D viewport を「数値パラメータ + ImGui controls」だけにする**:
//     実際の 3D 描画 (CPbrShader / CShadowMap / IBL) は
//     `ModelViewportRenderer` が担当する。本 panel は
//     「CCamera state + light params + background + toggle 群」の保管 + UI のみ。
//     こうしておけば panel 単体テストや、別の renderer (= raymarched preview /
//     OBJ thumbnail) への差し替えが容易。
//   ・**CEditorCamera を内包 (値メンバ)**: 各 panel が独自 camera を持つ Unity
//     SceneView 風モデル。Camera() アクセサで参照を返し、renderer が
//     ViewMatrix / ProjectionMatrix を取り出して使う。
//   ・**asset path は wchar_t バッファ (kMaxPathChars = 512)**: CAssetBrowser
//     と同じ規約。STL の std::wstring は使えないため、固定長で保持。
//     寿命管理を panel 側で完結させるため、コピー保存する (= CAssetBrowser の
//     pointer 寿命 = 次の Refresh まで、には依存しない)。
//   ・**OnAssetSelected で拡張子フィルタ**: `.mdl` / `.fbx` / `.gltf` / `.glb` /
//     `.obj` が Mesh asset とみなされる (CAssetBrowser::ClassifyByExtension の
//     Mesh 分類と一致)。それ以外の拡張子は無視 (= asset path は ASCII UTF-8 だが
//     panel 内では wchar_t に正規化して持つ)。
//   ・**ImGui controls の DrawUI 配置**: 単一 "Model Viewport" window 内に
//       - viewport 領域 (大半の面積、将来 renderer が描画 / 現状は dummy)
//       - 下部 control bar: Light dir / color / IBL / Tonemap / Background /
//         Grid / FBone skeleton toggle
//     を出す。各 widget の戻り値で「ユーザが変更したか」を判定する。本 panel は
//     値を保持するだけ。
//   ・**Tonemap mode は u32 enum 風 (0=ACES / 1=Reinhard / 2=Linear)**:
//     CPbrShader / Tonemap の既存 enum (`render/PostProcess.h` 等) との連携は
//     renderer 側で行う (本 panel は u32 で受け流し)。
//   ・**Background color は FVec4** (RGBA): renderer の clear color 用。alpha は
//     通常 1.0 だが、screenshot 用に alpha 0 もあり得る。
//   ・**非コピー / 非ムーブ / 全 noexcept / STL 不使用 / `<string>` 禁止**:
//     ACS 規約。文字列は wchar_t 固定長バッファのみ。
//   ・**ImGui ヘッダは .cpp 限定**: 他 panel (AHierarchyPanel / AInspectorPanel /
//     AParticleEditorPanel) と同形。
//   ・本 panel から forward decl で他 panel を受ける必要はない
//     (Workspace 経由で疎結合)。
//
// 範囲外 (別 panel):
//   ・PBR 描画 (CPbrShader / CShadowMap / IBL) の実呼出 = ModelViewportRenderer
//   ・material slot 編集 = AModelMaterialPanel
//   ・bone hierarchy 表示 = AModelInspectorPanel
//   ・animation timeline = AModelAnimationPanel
//   ・grid / gizmo の実描画 = CEditorGizmo
//   ・camera preset (Front / Top / Side) = CEditorCamera
//   ・undo / redo = CUndoStack 統合 (OnUndo / OnRedo hook)
#pragma once

#include "foundation/Types.h"
#include "gameframework/Forward.h"
#include "gameframework/tools/editor_core/EditorPanel.h"
#include "gameframework/tools/editor_core/EditorCamera.h"
#include "math/Vec.h"

namespace acs::game::modelview {

/**
 * 3D model viewport + 表示パラメータ UI を提供する panel (AEditorPanel 派生)。
 *
 * @details
 * 3D model の表示と asset 切替を行うメインビューポート。CEditorCamera (3D orbit) を
 * 内包し、Lighting (sun dir / color / IBL / tonemap)・Background・Grid・bone skeleton
 * 表示の数値パラメータと ImGui controls を保持する。実際の 3D 描画 (CPbrShader /
 * CShadowMap / IBL) は外部の ModelViewportRenderer が CurrentAssetPath() や各パラメータを
 * 見て担当し、本 panel は値の保管と UI のみを担う。CAssetBrowser からのファイル選択
 * (OnAssetSelected) を受けて mesh / model 拡張子なら自動 LoadModelAsset する。
 */
class AModelViewerPanel : public acs::game::editor_core::AEditorPanel {
public:
    /** 空のパネルを構築する (state は Init で確定)。 */
    AModelViewerPanel() noexcept = default;

    /** パネルを破棄する。 */
    ~AModelViewerPanel() noexcept override = default;

    /** コピー禁止 (内部 CEditorCamera + asset path バッファの所有を曖昧にしないため)。 */
    AModelViewerPanel(const AModelViewerPanel&)            = delete;

    /** コピー代入も禁止。 */
    AModelViewerPanel& operator=(const AModelViewerPanel&) = delete;

    /** ムーブ禁止 (ACS 規約 + 基底 AEditorPanel 規約)。 */
    AModelViewerPanel(AModelViewerPanel&&)                 = delete;

    /** ムーブ代入も禁止。 */
    AModelViewerPanel& operator=(AModelViewerPanel&&)      = delete;

    /**
     * パネルを初期化する。
     *
     * @details
     * 内部 state をデフォルトに戻し、CEditorCamera を 3D mode で Init、asset path を
     * 空にする。Workspace への登録は別途 CEditorWorkspace::RegisterPanel(&this) で行い
     * (= OnInit 経由で Workspace ポインタが保存される)、多重 Init 可 (= 完全リセット)。
     */
    void Init() noexcept;

    /**
     * 内部 state を全解放する。
     *
     * @details
     * CEditorCamera は POD だが念のため Reset でデフォルト state にする。OnShutdown とは
     * 別物で、Workspace から外す前に panel 単体で reset したい場合の API。多重 Shutdown 可。
     */
    void Shutdown() noexcept;

    /**
     * asset path をコピーして内部バッファに保存する。
     *
     * @details
     * HasModel() が true に切替わる。実際の mesh 読込 / GPU upload は本 panel では行わず、
     * 外部の ModelViewportRenderer が CurrentAssetPath() を見て読み込む規約。
     * @param asset_path 保存する asset path (nullptr または空文字なら ClearModel() 相当)。
     */
    void LoadModelAsset(const wchar_t* asset_path) noexcept;

    /**
     * 内部 asset path を空文字に戻し HasModel() を false にする。
     *
     * @details
     * CEditorCamera 状態 / Lighting / Background は保持する (= モデルを切替えても視点と
     * 照明が維持される、Unity SceneView と同じ挙動)。
     */
    void ClearModel() noexcept;

    /**
     * model asset がロード対象としてセットされているかを返す。
     *
     * @return asset path が空文字でなければ true。実 GPU リソースの有無とは独立。
     */
    bool HasModel() const noexcept;

    /**
     * 現在の asset path を返す。
     *
     * @return asset path (wchar_t)。未設定 / Clear 後は nullptr ではなく L"" を返す。
     */
    const wchar_t* CurrentAssetPath() const noexcept;

    /**
     * 内部 CEditorCamera への参照を返す。
     *
     * @details
     * 呼出側 (renderer / panel 内 UI) が HandleMouseInput / Tick / ViewMatrix 等を呼ぶ。
     * 寿命は本 panel と同一。
     * @return 内部 CEditorCamera への参照。
     */
    acs::game::editor_core::CEditorCamera& Camera() noexcept;

    /**
     * sun light direction を設定する。
     *
     * @details
     * normalized 推奨だが panel 内では正規化しない (renderer 側が必要なら Normalize する)。
     * default = (0.3, -0.7, 0.6) (= 太陽がやや右上から差す典型的 3-quarter ライト)。
     * @param dir sun light direction。
     */
    void SetLightDirection(acs::FVec3 dir) noexcept;

    /**
     * sun light direction を返す。
     *
     * @return 設定済みの sun light direction (未正規化)。
     */
    acs::FVec3 LightDirection() const noexcept;

    /**
     * sun light color を設定する。
     *
     * @details
     * RGB / linear space / 通常 1.0 中心。HDR 強度を出したい場合は >1.0 を入れる
     * (renderer 側で乗算される想定)。default = white (1,1,1)。
     * @param color sun light color (RGB linear)。
     */
    void SetLightColor(acs::FVec3 color) noexcept;

    /**
     * sun light color を返す。
     *
     * @return 設定済みの sun light color (RGB linear)。
     */
    acs::FVec3 LightColor() const noexcept;

    /**
     * IBL (environment-based indirect lighting) を有効にするか設定する。
     *
     * @details 無効にすると renderer 側は env / irradiance / prefilter map をサンプルしない。default = true。
     * @param b true で IBL 有効。
     */
    void SetIblEnabled(bool b) noexcept;

    /**
     * IBL が有効かを返す。
     *
     * @return IBL 有効なら true。
     */
    bool IsIblEnabled() const noexcept;

    /**
     * tonemap mode を設定する。
     *
     * @details
     * 0=ACES (default) / 1=Reinhard / 2=Linear (= no tonemap)。u32 にして renderer 側 enum
     * (render/PostProcess.h の ETonemapMode) と疎結合にする。範囲外値は無視 (= 既存値を維持)。
     * @param mode tonemap mode (0..kToneMappingModeCount-1)。
     */
    void SetToneMappingMode(u32 mode) noexcept;

    /**
     * 現在の tonemap mode を返す。
     *
     * @return tonemap mode (0=ACES / 1=Reinhard / 2=Linear)。
     */
    u32  ToneMappingMode() const noexcept;

    /**
     * viewport background color を設定する。
     *
     * @details
     * RGBA / linear space。renderer 側で clear color として使う。alpha は通常 1.0 だが
     * screenshot 用 0 もあり得る。default = (0.15, 0.15, 0.18, 1.0) (= editor 風暗グレー)。
     * @param color background color (RGBA linear)。
     */
    void SetBackgroundColor(acs::FVec4 color) noexcept;

    /**
     * viewport background color を返す。
     *
     * @return 設定済みの background color (RGBA linear)。
     */
    acs::FVec4 BackgroundColor() const noexcept;

    /**
     * grid (XZ 平面のチェッカー) を描くかを返す。
     *
     * @return grid を描くなら true (default = true)。
     */
    bool ShowGrid() const noexcept;

    /**
     * grid を描くかを設定する。
     *
     * @param b true で grid 表示。
     */
    void SetShowGrid(bool b) noexcept;

    /**
     * bone skeleton を line overlay で描くかを返す。
     *
     * @return bone skeleton を描くなら true (default = false)。
     */
    bool ShowBoneSkeleton() const noexcept;

    /**
     * bone skeleton を line overlay で描くかを設定する。
     *
     * @details モデルが skeletal でない場合に視覚的ノイズになるため OFF が無難。
     * @param b true で bone skeleton 表示。
     */
    void SetShowBoneSkeleton(bool b) noexcept;

    /**
     * window タイトルを返す (ImGui::Begin の引数兼 ID)。
     *
     * @return 固定リテラル "Model Viewport"。
     */
    const char* Title() const noexcept override { return "Model Viewport"; }

    /**
     * Workspace への登録時に呼ばれる初期化フック。
     *
     * @details 基底実装で Workspace ポインタを保存し、本クラスでは CEditorCamera を 3D mode で Init し直す保険を行う。
     * @param workspace 登録先の CEditorWorkspace。
     */
    void OnInit(acs::game::editor_core::CEditorWorkspace& workspace) noexcept override;

    /**
     * ImGui window を描画する (viewport プレースホルダ + control bar)。
     *
     * @details
     * IsVisible() が false なら早期 return (= close ボタンで隠せる)。実 3D 描画は外部
     * renderer の責務で、本 panel は dummy area + 控えめな overlay text のみ描く。
     */
    void DrawUI() noexcept override;

    /**
     * CAssetBrowser からのファイル選択通知フック。
     *
     * @details
     * 拡張子が Mesh 相当 (.mdl/.fbx/.gltf/.glb/.obj) なら自動 LoadModelAsset し、それ以外
     * (texture / font / particle 等) は無視する。
     * @param asset_path 選択された asset path (UTF-8)。
     */
    void OnAssetSelected(const char* asset_path) noexcept override;

    /**
     * asset path 用バッファ長 (wchar_t 単位、終端含む)。
     *
     * @details CAssetBrowser::kMaxPathChars と同値にして相互運用しやすくする (= 同じ規約)。
     */
    static constexpr u32 kMaxAssetPathChars = 512u;

    /** ToneMappingMode の有効範囲 (= 0,1,2)。これを超える値が来たら無視する。 */
    static constexpr u32 kToneMappingModeCount = 3u;

private:
    /** 3D viewport camera (orbit / pan / dolly)。Init() で Mode3D に初期化。 */
    acs::game::editor_core::CEditorCamera m_Camera {};

    /** 現在のモデル asset path (UTF-16)。未設定時は先頭が L'\0'、コピー所有。 */
    wchar_t m_AssetPath[kMaxAssetPathChars] = {};

    /** sun light direction (renderer 側で正規化)。 */
    acs::FVec3 m_LightDir   {0.3f, -0.7f, 0.6f};

    /** sun light color (RGB linear)。 */
    acs::FVec3 m_LightColor {1.0f, 1.0f, 1.0f};

    /** IBL 有効フラグ。 */
    bool      m_IblEnabled = true;

    /** tonemap mode (0=ACES, 1=Reinhard, 2=Linear)。 */
    u32       m_TonemapMode = 0u;

    /** viewport background color (RGBA linear)。 */
    acs::FVec4 m_BgColor    {0.15f, 0.15f, 0.18f, 1.0f};

    /** grid 表示フラグ。 */
    bool      m_ShowGrid           = true;

    /** bone skeleton 表示フラグ。 */
    bool      m_ShowBoneSkeleton  = false;
};

using FModelViewerPanel = AModelViewerPanel;

} // namespace acs::game::modelview
