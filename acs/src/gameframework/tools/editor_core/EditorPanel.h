// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — editor_core / FEditorPanel (Phase 21a)
//
// 全エディタパネルの共通基底クラス。Phase 20 で実装された各 panel
// (FHierarchyPanel / FInspectorPanel / FEditorToolbar 等) は独立クラスとして
// 作られていたが、Phase 21 以降で追加される複数の editor 群
// (ModelViewer / AnimCurveEditor / BehaviorTreeEditor / LevelEditor /
//  SpriteAtlasEditor / FontEditor / CinematicsTimelineEditor) を統一的に
// 配線するため、ライフサイクル + 選択通知 + レイアウト永続化を共通化する。
//
// 使い方 (派生側の典型):
//   class FModelViewerPanel : public acs::game::editor_core::FEditorPanel {
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
//   // ホスト側 (FEditorWorkspace) からの呼び出し:
//   FModelViewerPanel viewer;
//   viewer.OnInit(workspace);
//   // 毎フレーム:
//   viewer.OnFrameBegin(dt);
//   viewer.DrawUI();
//   // 終了時:
//   viewer.OnShutdown();
//
// 設計選択 (Pillar editor_core Phase 21a):
//   ・**抽象基底 + デフォルト stub**: `Title()` と `DrawUI()` は純粋仮想 (= 各
//     panel に必須)。それ以外の hook (OnInit / OnFrameBegin / OnSelectionChanged
//     / OnAssetSelected / OnSaveLayout / OnLoadLayout) は no-op default 実装で、
//     必要な panel だけ override する形。
//   ・**ImGui::Begin/End は派生クラス側責務**: 基底で wrap する案も検討したが、
//     panel ごとに ImGuiWindowFlags / dock target / MenuBar の有無が異なるため、
//     強制 wrap せず派生側で完全制御させる方針 (Unity Editor の `EditorWindow`
//     と同じ責任分担)。`m_Visible` は派生側で `ImGui::Begin(Title(), &m_Visible)`
//     の close ボタンに直接渡せる public-ish state。
//   ・**FEditorWorkspace は forward-decl のみ**: ヘッダ依存を最小化。具体的な
//     workspace 型 (FSelectionService / FAssetBrowser / DockSpace 等の集約 hub) は
//     Phase 21a 完了後に同 editor_core 配下に実装される予定で、本基底は型を
//     知らなくても OnInit で参照を保存できればよい。Workspace ポインタは
//     non-owning (workspace の生存期間 ≧ panel の生存期間)。
//   ・**OnSelectionChanged / OnAssetSelected の二系統**:
//       - OnSelectionChanged: Scene 内の Node 選択 (FSelectionService 経由)
//       - OnAssetSelected   : FAssetBrowser からのファイル選択 (asset path string)
//     2 つを独立 hook にしておくことで、ModelViewer のように "asset 系のみ反応"
//     する panel と、Inspector のように "node 系のみ反応" する panel が綺麗に
//     書き分けられる。
//   ・**OnSaveLayout / OnLoadLayout は stub**: Phase 21a 時点では
//     EditorLayoutSerializer 本体が未実装。型は forward-decl で受け、派生 panel が
//     window 位置 / size / dock state を後付けで永続化できる「予約点」として
//     用意。serializer 実装は Phase 21c で。
//   ・**WantsFocus = false default**: 起動時に Focus を奪う panel は限定的
//     (例: SceneView を初期 Focus にしたい等) なので opt-in。
//   ・**非コピー / 非ムーブ**: panel は workspace と紐づく lifecycle を持つため
//     所有を曖昧にしない (ACS 規約)。
//   ・**全 noexcept / STL 不使用 / `<string>` 禁止**: ACS 規約。文字列は
//     `const char*` リテラル想定 (Title は静的文字列、asset_path は FAssetBrowser
//     所有バッファ)。
//   ・**ImGui ヘッダは含めない**: 派生クラスの .cpp で <imgui.h> を include する
//     パターン (FParticleEditorPanel / FInspectorPanel と同形)。
//
// 将来拡張余地 (Phase 21b 以降):
//   ・`OnKeyShortcut(KeyCombo combo) noexcept` — panel ごとのキーバインド
//     (例: ModelViewer の F でフレーミング、AnimCurveEditor の Space で再生切替)。
//     KeyCombo 型は Phase 21b で input/ 配下に追加予定。
//   ・`OnUndo() / OnRedo() noexcept` — panel 内 undo stack 統合
//     (Inspector の field 編集、CurveEditor のキー操作、LevelEditor の配置等を
//      Workspace 共通 undo stack に流す)。
//   ・`virtual u32 GetDependencyMask() const noexcept` — panel 間の依存を bit flag
//     で宣言 (例: ModelViewer は FAssetBrowser に依存)。Workspace が dependency 順に
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

namespace acs::game::editor_core {

// 別 panel / 同 editor_core 配下で実装される hub クラス群の forward-decl。
// 本ヘッダから実体ヘッダを include しないことで「panel の派生クラスを書く側」が
// editor_core の他コンポーネントを意識せず override だけに集中できる。
class FEditorWorkspace;
class EditorLayoutSerializer;

} // namespace acs::game::editor_core

namespace acs::game::inspector {
// Phase 20 で実装された FSelectionService。OnSelectionChanged の引数型として
// forward-decl のみで受ける。本基底は FSelectionService の API を直接呼ばない
// (派生側が selection.CurrentSelection() 等を呼ぶ)。
class FSelectionService;
} // namespace acs::game::inspector

namespace acs::game::editor_core {

// ---------------------------------------------------------------------------
// FEditorPanel — 全エディタパネルの抽象基底
// ---------------------------------------------------------------------------
class FEditorPanel {
public:
    FEditorPanel() noexcept = default;
    virtual ~FEditorPanel() noexcept = default;

    // 非コピー・非ムーブ: panel は workspace と固有の lifecycle を持ち、
    // 同一インスタンスが Workspace に登録される (= unique 存在) ため
    // 所有関係を曖昧にしない (ACS 規約 + 他 panel と同形)。
    FEditorPanel(const FEditorPanel&)            = delete;
    FEditorPanel& operator=(const FEditorPanel&) = delete;
    FEditorPanel(FEditorPanel&&)                 = delete;
    FEditorPanel& operator=(FEditorPanel&&)      = delete;

    // ----- 必須 override (純粋仮想) -----------------------------------------

    // ImGui::Begin に渡す window タイトル。リテラル / 静的領域文字列を返すこと
    // (本基底はコピー所有しない)。同じ FEditorWorkspace 内で重複しない一意な名前
    // が望ましい (ImGui の window id 衝突回避のため)。
    virtual const char* Title() const noexcept = 0;

    // メインの ImGui 描画。`ImGui::Begin(Title(), ...)` / `ImGui::End()` は
    // 派生クラス側責務 (基底は wrap しない)。`IsVisible()` が false の場合に
    // 早期 return するかは派生側判断 (典型は冒頭で `if (!IsVisible()) return;`)。
    virtual void DrawUI() noexcept = 0;

    // ----- 任意 override (デフォルト no-op) ---------------------------------

    // Workspace への登録時に 1 度呼ばれる。`workspace` への参照を内部に保存し、
    // 以降の hook 内から `Workspace()` 経由で取り出せるようにする (基底実装)。
    // 派生クラスで追加初期化が必要なら override + 基底呼び出しの順で書く:
    //   void OnInit(FEditorWorkspace& ws) noexcept override {
    //       FEditorPanel::OnInit(ws);
    //       // ... 追加初期化 ...
    //   }
    virtual void OnInit(FEditorWorkspace& workspace) noexcept;

    // Workspace からの登録解除時 / editor shutdown 時に 1 度呼ばれる。
    // 派生クラスは GPU リソース解放やコールバック解除をここで行う。
    // 多重呼び出し可 (= 二重 Shutdown は派生側で no-op に書くこと)。
    virtual void OnShutdown() noexcept {}

    // フレーム開始時 (= UI 描画より前) に呼ばれる。input 取得 / state 更新 /
    // 非同期タスクのポーリング等、ImGui 描画前に済ませたい処理をここで。
    // `dt` はスケール後 dt (秒)。
    virtual void OnFrameBegin(f32 /*dt*/) noexcept {}

    // Scene 内 Node の選択が変わったときに呼ばれる (Phase 20 FSelectionService
    // との統合)。selection から `CurrentSelection()` 等を取り出して反映する。
    // FSelectionService の lifecycle 管理は Workspace 側責務。
    virtual void OnSelectionChanged(inspector::FSelectionService& /*selection*/) noexcept {}

    // FAssetBrowser からファイルが選択された時の通知 (Phase 21 別 panel で実装予定)。
    // `asset_path` は FAssetBrowser が所有する文字列 (本 panel ではコピー禁止 = STL
    // 不使用 + `<string>` 禁止のため、保持したい場合は ACS 内の固定長 char バッファ
    // または FilePath 型を派生クラスで持つこと)。nullptr が来た場合は "選択解除"
    // と解釈する。
    virtual void OnAssetSelected(const char* /*asset_path*/) noexcept {}

    // 起動時 / dock layout reset 時に Workspace が問い合わせる "Focus 要求"。
    // true を返した panel は ImGui::SetWindowFocus 相当が一度だけ呼ばれる。
    // 典型: SceneView を起動直後の active 操作対象にしたい場合。
    virtual bool WantsFocus() const noexcept { return false; }

    // ----- レイアウト永続化 (Phase 21c 本実装、現状は予約点) -----------------

    // panel ごとの追加レイアウト情報 (例: 内部 splitter 比率 / カラム幅 /
    // 表示モード) を `out` にシリアライズする。ImGui の window 位置 / size /
    // dock state は ImGui 自体が ini file に保存するため、ここでは「panel
    // 独自の表示設定」のみを対象とする。
    virtual void OnSaveLayout(EditorLayoutSerializer& /*out*/) noexcept {}

    // OnSaveLayout で書き出した情報を復元する。フォーマット互換が無い場合は
    // 安全に no-op に倒すこと (= 例外は投げない / 古いレイアウトは黙って捨てる)。
    virtual void OnLoadLayout(EditorLayoutSerializer& /*in*/) noexcept {}

    // ----- 状態アクセサ -----------------------------------------------------

    // panel 表示状態 (close ボタン or プログラム的 hide)。
    bool IsVisible() const noexcept { return m_Visible; }
    void SetVisible(bool b) noexcept { m_Visible = b; }

    // dock target にできるか。Workspace 側で dock 対象 panel を絞るためのヒント。
    // 典型: ステータスバーやツールバーは dock target にしない、ビューや
    // インスペクタは dock target にする、等。
    bool IsDockTarget() const noexcept { return m_DockedTarget; }
    void SetDockTarget(bool b) noexcept { m_DockedTarget = b; }

    // OnInit で保存された Workspace ポインタ。OnInit 前 / OnShutdown 後は
    // nullptr。non-owning (workspace の生存期間は呼び出し側責務)。
    FEditorWorkspace* Workspace() const noexcept { return m_Workspace; }

protected:
    // panel 表示 toggle。派生クラスから `ImGui::Begin(Title(), &m_Visible)` の
    // close ボタンに直接バインドできるよう protected 公開。
    bool m_Visible = true;

    // dock target 可否。派生クラスがコンストラクタや OnInit で設定する想定。
    // 例: ステータスバーなら false、ビューやインスペクタなら true。
    bool m_DockedTarget = false;

private:
    // OnInit で保存される Workspace 参照 (non-owning)。
    // OnShutdown で nullptr に戻すかは基底責務 (= OnShutdown は no-op default の
    // ため、派生側で必要なら明示的に解除すること)。
    FEditorWorkspace* m_Workspace = nullptr;
};

} // namespace acs::game::editor_core
