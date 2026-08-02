// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — editor_core / AEditorPanel
//
// 全エディタパネルの共通基底クラス。各 panel
// (AHierarchyPanel / AInspectorPanel / AEditorToolbar 等) と複数の editor 群
// (ModelViewer / AnimCurveEditor / BehaviorTreeEditor / LevelEditor /
//  SpriteAtlasEditor / FontEditor / CinematicsTimelineEditor) を統一的に
// 配線するため、ライフサイクル + 選択通知 + レイアウト永続化を共通化する。
//
// 使い方 (派生側の典型):
//   class AModelViewerPanel : public acs::game::editor_core::AEditorPanel {
//   public:
//       const char* Title() const noexcept override { return "Model Viewer"; }
//       void DrawUI() noexcept override {
//           if (!IsVisible()) return;
//           if (ImGui::Begin(Title(), &m_Visible)) {
//               // ... viewer 描画 ...
//           }
//           ImGui::End();
//       }
//       void OnAssetSelected(const char* asset_path) noexcept override {
//           // asset_path が .acs_model 拡張子なら load する等
//       }
//   };
//
//   // ホスト側 (CEditorWorkspace) からの呼び出し:
//   AModelViewerPanel viewer;
//   viewer.OnInit(workspace);
//   // 毎フレーム:
//   viewer.OnFrameBegin(dt);
//   viewer.DrawUI();
//   // 終了時:
//   viewer.OnShutdown();
//
// 設計選択 (Pillar editor_core):
//   ・**抽象基底 + デフォルト stub**: `Title()` と `DrawUI()` は純粋仮想 (= 各
//     panel に必須)。それ以外の hook (OnInit / OnFrameBegin / OnSelectionChanged
//     / OnAssetSelected / OnSaveLayout / OnLoadLayout) は no-op default 実装で、
//     必要な panel だけ override する形。
//   ・**ImGui::Begin/End は派生クラス側責務**: 基底で wrap する案も検討したが、
//     panel ごとに ImGuiWindowFlags / dock target / MenuBar の有無が異なるため、
//     強制 wrap せず派生側で完全制御させる方針 (Unity Editor の `EditorWindow`
//     と同じ責任分担)。`m_Visible` は派生側で `ImGui::Begin(Title(), &m_Visible)`
//     の close ボタンに直接渡せる public-ish state。
//   ・**CEditorWorkspace は forward-decl のみ**: ヘッダ依存を最小化。具体的な
//     workspace 型 (CSelectionService / CAssetBrowser / DockSpace 等の集約 hub) は
//     同 editor_core 配下に実装され、本基底は型を
//     知らなくても OnInit で参照を保存できればよい。Workspace ポインタは
//     non-owning (workspace の生存期間 ≧ panel の生存期間)。
//   ・**OnSelectionChanged / OnAssetSelected の二系統**:
//       - OnSelectionChanged: AScene 内の ANode 選択 (CSelectionService 経由)
//       - OnAssetSelected   : CAssetBrowser からのファイル選択 (asset path string)
//     2 つを独立 hook にしておくことで、ModelViewer のように "asset 系のみ反応"
//     する panel と、Inspector のように "node 系のみ反応" する panel が綺麗に
//     書き分けられる。
//   ・**OnSaveLayout / OnLoadLayout は stub**: FEditorLayoutSerializer の型は
//     forward-decl で受け、派生 panel が
//     window 位置 / size / dock state を後付けで永続化できる「予約点」として
//     用意。
//   ・**WantsFocus = false default**: 起動時に Focus を奪う panel は限定的
//     (例: SceneView を初期 Focus にしたい等) なので opt-in。
//   ・**非コピー / 非ムーブ**: panel は workspace と紐づく lifecycle を持つため
//     所有を曖昧にしない (ACS 規約)。
//   ・**全 noexcept / STL 不使用 / `<string>` 禁止**: ACS 規約。文字列は
//     `const char*` リテラル想定 (Title は静的文字列、asset_path は CAssetBrowser
//     所有バッファ)。
//   ・**ImGui ヘッダは含めない**: 派生クラスの .cpp で <imgui.h> を include する
//     パターン (AParticleEditorPanel / AInspectorPanel と同形)。
//
// 将来拡張余地:
//   ・`OnKeyShortcut(KeyCombo combo) noexcept` — panel ごとのキーバインド
//     (例: ModelViewer の F でフレーミング、AnimCurveEditor の Space で再生切替)。
//   ・`OnUndo() / OnRedo() noexcept` — panel 内 undo stack 統合
//     (Inspector の field 編集、CurveEditor のキー操作、LevelEditor の配置等を
//      Workspace 共通 undo stack に流す)。
//   ・`virtual u32 GetDependencyMask() const noexcept` — panel 間の依存を bit flag
//     で宣言 (例: ModelViewer は CAssetBrowser に依存)。Workspace が dependency 順に
//     OnFrameBegin / DrawUI を呼ぶ schedule を組むため。
//   ・`OnDockStateChanged()` — ImGui dock の attach/detach 通知。
//
// 範囲外 (本基底では持たない):
//   ・ImGui::Begin/End の自動 wrap (= 派生側責務)
//   ・panel ごとの ToolStrip / MenuBar (= 派生側で DrawUI 内で実装)
//   ・Localization (Title 文字列は当面英語固定)
//   ・viewport / render target の確保 (= ModelViewer 等の派生側で個別管理)
#pragma once

#include "foundation/Types.h"
#include "gameframework/Forward.h"

namespace acs::game::editor_core {

class FEditorLayoutSerializer;

} // namespace acs::game::editor_core

namespace acs::game::editor_core {

/**
 * 全エディタパネルの抽象基底クラス。
 *
 * @details
 * 各 panel (AHierarchyPanel / AInspectorPanel / AEditorToolbar 等) や複数の editor
 * 群を統一的に配線するため、ライフサイクル + 選択通知 + レイアウト永続化を共通化する。
 * Title() と DrawUI() は純粋仮想で各 panel 必須、それ以外の hook は no-op default を
 * 持ち必要な panel だけ override する。ImGui::Begin/End の wrap は行わず派生側責務とし、
 * Workspace ポインタは OnInit で受けて non-owning で保持する。
 */
class AEditorPanel {
public:
    /** 空状態で構築する (表示 ON、dock target OFF、Workspace 未設定)。 */
    AEditorPanel() noexcept = default;

    /** 派生クラスを正しく破棄するための仮想デストラクタ。 */
    virtual ~AEditorPanel() noexcept = default;

    /** コピー禁止 (panel は Workspace と固有の lifecycle を持つ unique 存在)。 */
    AEditorPanel(const AEditorPanel&)            = delete;

    /** コピー代入も禁止。 */
    AEditorPanel& operator=(const AEditorPanel&) = delete;

    /** ムーブ禁止 (panel は Workspace と固有の lifecycle を持つ unique 存在)。 */
    AEditorPanel(AEditorPanel&&)                 = delete;

    /** ムーブ代入も禁止。 */
    AEditorPanel& operator=(AEditorPanel&&)      = delete;

    /**
     * ImGui::Begin に渡す window タイトルを返す (純粋仮想)。
     *
     * @details
     * リテラル / 静的領域文字列を返すこと (本基底はコピー所有しない)。同じ
     * CEditorWorkspace 内で重複しない一意な名前が望ましい (ImGui の window id 衝突回避)。
     * @return window タイトル文字列。
     */
    virtual const char* Title() const noexcept = 0;

    /**
     * メインの ImGui 描画を行う (純粋仮想)。
     *
     * @details
     * ImGui::Begin(Title(), ...) / ImGui::End() は派生クラス側責務で、基底は wrap しない。
     * IsVisible() が false のとき早期 return するかは派生側判断 (典型は冒頭で
     * if (!IsVisible()) return;)。
     */
    virtual void DrawUI() noexcept = 0;

    /**
     * Workspace への登録時に 1 度呼ばれる初期化フック。
     *
     * @details
     * workspace への参照を内部に保存し、以降の hook 内から Workspace() で取り出せる
     * ようにする (基底実装)。派生クラスで追加初期化が必要なら override + 冒頭で
     * AEditorPanel::OnInit(ws) を呼ぶ。
     * @param workspace 登録先の editor workspace (参照を non-owning で保持)。
     */
    virtual void OnInit(CEditorWorkspace& workspace) noexcept;

    /**
     * Workspace からの登録解除時 / editor shutdown 時に呼ばれる後始末フック。
     *
     * @details
     * 派生クラスは GPU リソース解放やコールバック解除をここで行う。多重呼び出し可
     * (二重 Shutdown は派生側で no-op に書くこと)。
     */
    virtual void OnShutdown() noexcept {}

    /**
     * フレーム開始時 (UI 描画より前) に呼ばれるフック。
     *
     * @details input 取得 / state 更新 / 非同期タスクのポーリング等、ImGui 描画前に済ませたい処理をここで行う。
     * @param dt スケール後の経過秒。
     */
    virtual void OnFrameBegin(f32 /*dt*/) noexcept {}

    /**
     * AScene 内 ANode の選択が変わったときに呼ばれるフック。
     *
     * @details
     * selection から CurrentSelection() 等を取り出して反映する。CSelectionService の
     * lifecycle 管理は Workspace 側責務。
     * @param selection 現在の選択状態を保持する選択サービス。
     */
    virtual void OnSelectionChanged(inspector::CSelectionService& /*selection*/) noexcept {}

    /**
     * CAssetBrowser からファイルが選択された時に呼ばれるフック。
     *
     * @details
     * asset_path は CAssetBrowser が所有する文字列で、保持したい場合は固定長 char
     * バッファ等を派生クラスで持つこと (STL / <string> 不使用)。nullptr は「選択解除」と解釈する。
     * @param asset_path 選択された asset のパス (nullptr で選択解除)。
     */
    virtual void OnAssetSelected(const char* /*asset_path*/) noexcept {}

    /**
     * 起動時 / dock layout reset 時に Workspace が問い合わせる Focus 要求。
     *
     * @details true を返した panel は ImGui::SetWindowFocus 相当が一度だけ呼ばれる (典型: SceneView)。
     * @return Focus を要求するなら true (既定 false)。
     */
    virtual bool WantsFocus() const noexcept { return false; }

    /**
     * panel 独自の追加レイアウト情報をシリアライズするフック。
     *
     * @details
     * 内部 splitter 比率 / カラム幅 / 表示モード等を out に書く。ImGui の window 位置 /
     * size / dock state は ImGui 自体が ini file に保存するため対象外。
     * @param out レイアウト情報の書き出し先シリアライザ。
     */
    virtual void OnSaveLayout(FEditorLayoutSerializer& /*out*/) noexcept {}

    /**
     * OnSaveLayout で書き出した情報を復元するフック。
     *
     * @details フォーマット互換が無い場合は安全に no-op に倒すこと (例外を投げず古いレイアウトは黙って捨てる)。
     * @param in レイアウト情報の読み込み元シリアライザ。
     */
    virtual void OnLoadLayout(FEditorLayoutSerializer& /*in*/) noexcept {}

    /**
     * panel が表示状態かを返す。
     *
     * @return 表示中なら true。
     */
    bool IsVisible() const noexcept { return m_Visible; }

    /**
     * panel の表示状態を設定する (close ボタン or プログラム的 hide)。
     *
     * @param b 表示するなら true。
     */
    void SetVisible(bool b) noexcept { m_Visible = b; }

    /**
     * panel が dock target にできるかを返す。
     *
     * @return dock target 可なら true。
     */
    bool IsDockTarget() const noexcept { return m_DockedTarget; }

    /**
     * panel を dock target にできるかを設定する。
     *
     * @details Workspace 側で dock 対象 panel を絞るためのヒント (例: ステータスバーは false、ビューやインスペクタは true)。
     * @param b dock target 可とするなら true。
     */
    void SetDockTarget(bool b) noexcept { m_DockedTarget = b; }

    /**
     * OnInit で保存された Workspace ポインタを返す。
     *
     * @return Workspace へのポインタ (OnInit 前 / OnShutdown 後は nullptr、non-owning)。
     */
    CEditorWorkspace* Workspace() const noexcept { return m_Workspace; }

protected:
    /** panel 表示 toggle (派生から ImGui::Begin の close ボタンに直接バインド可能)。 */
    bool m_Visible = true;

    /** dock target 可否 (派生がコンストラクタや OnInit で設定する想定)。 */
    bool m_DockedTarget = false;

private:
    /** OnInit で保存される Workspace 参照 (non-owning)。 */
    CEditorWorkspace* m_Workspace = nullptr;
};

} // namespace acs::game::editor_core
