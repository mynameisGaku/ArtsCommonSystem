// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — editor_core / FEditorWorkspace (Phase 21a)
//
// 複数の `FEditorPanel` (ModelViewer / AnimCurveEditor / BehaviorTreeEditor /
// LevelEditor / SpriteAtlasEditor / FontEditor / CinematicsTimelineEditor 等) を
// 統括する **ワークスペース**。Phase 20 までは個別 panel が独立に存在していたが、
// Phase 21 で「panel 群を 1 つの editor アプリケーションとしてまとめて配線する」
// ための中央 hub として導入。
//
// 役割:
//   ・登録 panel のリスト管理 (PushBack / Find / Remove)
//   ・毎フレームの main loop coordination
//       (OnFrameBegin → DockSpace 描画 → DrawUI → MenuBar)
//   ・ImGui DockSpace の作成 + FWindow メニュー (panel toggle list)
//   ・レイアウト永続化 (ImGui ini + per-panel state を `.acslayout` 1 ファイル)
//   ・選択 / asset 選択イベントの全 panel への broadcast
//   ・FSelectionService (Phase 20) の保管点 (非所有)
//
// 使い方 (典型):
//   acs::game::editor_core::FEditorWorkspace ws;
//   ws.Init();
//
//   ws.SetSelectionService(&selection);   // Phase 20 FSelectionService を注入
//   ws.RegisterPanel(&hierarchy_panel);    // 各 panel は caller 所有
//   ws.RegisterPanel(&inspector_panel);
//   ws.RegisterPanel(&model_viewer);
//
//   // 必要なら起動時にレイアウト復元:
//   ws.LoadLayout(L"data/editor/last.acslayout");
//
//   // 毎フレーム:
//   ws.TickAllPanels(dt);
//
//   // 終了時:
//   ws.SaveLayout(L"data/editor/last.acslayout");
//   ws.Shutdown();
//
// 設計選択 (Pillar editor_core Phase 21a):
//   ・**panel は raw pointer の非所有保持**: caller が own する (= caller が
//     panel の lifetime を制御する) ことで、panel の動的生成 / scope-stack 配置
//     の両方を許容する。FParticleEditorPanel / FEditorToolbar と同形。
//   ・**`acs::TArray<FEditorPanel*>` で順序保持**: dispatch 順 / FWindow メニュー
//     の表示順 = 登録順。登録順以外のソートはしない (panel 自身に dependency
//     mask を持たせる Phase 21b 拡張で初めて並べ替えが入る予定)。
//   ・**`UnregisterPanel` は順序保存削除**: FWindow メニューの並びがフレーム間で
//     ぶれないよう、swap-remove ではなく shift 削除。FSelectionService の
//     RemoveAtSwap とは方針を変える (UI 表示順の体験を優先)。
//   ・**Title はリテラル文字列を期待**: `FEditorPanel::Title()` の規約 (リテラル /
//     静的領域) に依存する。`FindPanelByTitle` は strcmp 比較。
//   ・**ImGui::DockSpaceOverViewport**: Phase 21a では「メインビューポート全体に
//     central dock node を持つ」最小構成。central node 内で float window 動作
//     させたい panel は `SetDockTarget(false)` でヒントを出せるが、現状は
//     dock_target hint を DockSpace に強制反映する仕組みは持たない (Phase 21c 予定)。
//   ・**FWindow メニュー / Layout メニュー は `DrawMenuBar()` 内で MainMenuBar に
//     直接 push**: 派生コードからは TickAllPanels を呼ぶだけで自動描画される。
//     MenuBar の有無は `_enable_menu_bar` で制御可能 (= 既存 MainMenuBar に
//     共存させたい host は disable できる)。
//   ・**SaveLayout / LoadLayout のファイル形式**: 自前テキストフォーマット
//     `ACS_EDLAYOUT 1`。
//       1) ヘッダ行  : `ACS_EDLAYOUT <version>`
//       2) ImGui ini : `IMGUI_INI <byte_size>\n<raw ini bytes>\n`
//       3) panel state: `PANEL <title> <visible:0/1> <dock_target:0/1>\n` (1 行 1 panel)
//     ImGui ini は ImGui::SaveIniSettingsToMemory() で取得した raw 文字列を
//     生埋め込み。LoadLayout 側で同じく LoadIniSettingsFromMemory に渡す。
//     `EditorLayoutSerializer` が将来正式実装されたらここを置き換える予定。
//   ・**BroadcastSelectionChanged / BroadcastAssetSelected** は全 panel への
//     fan-out: 戻り値は無く、panel 側で必要に応じて自身に反映する。null safe。
//   ・**非コピー / 非ムーブ / 全 noexcept / STL 不使用**: ACS 規約。
//   ・**ImGui ヘッダは .cpp 側のみ include**: header からは imgui 依存を漏らさず、
//     FParticleEditorPanel / FInspectorPanel と同方針。
//
// 将来拡張余地 (Phase 21b 以降):
//   ・panel dependency 自動解決 (依存先 panel が無いと disable / 順序ソート)
//   ・shortcut key dispatcher (Ctrl+S → 全 panel の OnSaveLayout、Ctrl+Z → undo 等)
//   ・multi-window support (sub-viewport ごとに別 DockSpace)
//   ・panel template factory (run-time に panel 種別を選んで新規生成)
//   ・Workspace 階層 (= editor 内に「ツールパレット」サブ Workspace 等)
//
// 範囲外 (本クラスでは持たない):
//   ・panel の生成 / 破棄 (= caller 責務)
//   ・ImGui Context / GPU resource 管理 (= 外側の ImGuiBackend が持つ)
//   ・実 layout serializer (= Phase 21c の EditorLayoutSerializer 実装後に差替)
//   ・undo stack (= Phase 21b 予定)
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"

namespace acs::game::editor_core {

// 同 namespace の FEditorPanel は forward-decl のみで受ける。
// 本ヘッダから FEditorPanel.h を include しないことで、利用側 (sample / 上位
// editor アプリ) が「workspace と panel 群」を疎結合にビルド単位として
// 扱えるようにする (panel 派生クラスのヘッダ変更が workspace 自身の再ビルド
// 要否に影響しない)。
class FEditorPanel;

} // namespace acs::game::editor_core

namespace acs::game::inspector {
// Phase 20 で実装された FSelectionService。SetSelectionService / Get... API の
// 引数型として forward-decl のみで受ける。本 workspace は FSelectionService の
// API を直接呼ばない (= broadcast 時に panel 側へ参照を渡すだけ)。
class FSelectionService;
} // namespace acs::game::inspector

namespace acs::game::editor_core {

// ---------------------------------------------------------------------------
// FEditorWorkspace — 複数 FEditorPanel の統括 hub
// ---------------------------------------------------------------------------
class FEditorWorkspace {
public:
    FEditorWorkspace() noexcept = default;
    ~FEditorWorkspace() noexcept = default;

    // 非コピー・非ムーブ: 内部 TArray<FEditorPanel*> + FSelectionService 参照の
    // 所有 / 参照関係を曖昧にしない (ACS 規約)。
    FEditorWorkspace(const FEditorWorkspace&)            = delete;
    FEditorWorkspace& operator=(const FEditorWorkspace&) = delete;
    FEditorWorkspace(FEditorWorkspace&&)                 = delete;
    FEditorWorkspace& operator=(FEditorWorkspace&&)      = delete;

    // 初期化: panel list を空、FSelectionService を nullptr、設定値を default に。
    // 多重 Init 可 (= 完全リセットとして使える)。登録 panel は OnShutdown を
    // 呼ばずに破棄するので、Init 前に必ず Shutdown を済ませること
    // (= "クリアしたいなら Shutdown→Init")。
    void Init() noexcept;

    // 後片付け: 登録済み全 panel に OnShutdown を呼んでから list を解放する。
    // FSelectionService 参照も解除。多重 Shutdown 可。
    void Shutdown() noexcept;

    // ----- panel 登録 / 探索 ------------------------------------------------

    // panel を末尾に追加して OnInit を呼ぶ。`panel` の所有は caller。
    // 二重登録 (同一ポインタ) は no-op で弾く (= OnInit が複数回呼ばれない)。
    // nullptr 渡しは no-op。
    void RegisterPanel(FEditorPanel* panel) noexcept;

    // 登録解除して OnShutdown を呼ぶ。順序保存削除 (= shift)。
    // 未登録 / nullptr は no-op。
    void UnregisterPanel(FEditorPanel* panel) noexcept;

    // 現在の登録 panel 数。
    u32 PanelCount() const noexcept;

    // index 番目の panel ポインタ。範囲外は nullptr。
    FEditorPanel* GetPanelByIndex(u32 i) const noexcept;

    // Title が完全一致する panel を返す。なければ nullptr。
    // title が nullptr の場合は nullptr。比較は strcmp。
    FEditorPanel* FindPanelByTitle(const char* title) const noexcept;

    // Title が完全一致する panel の `_visible` を反転。未発見 / null は no-op。
    // 副作用として該当 panel の `SetVisible(!IsVisible())` を呼ぶ。
    void TogglePanelVisible(const char* title) noexcept;

    // ----- メインループ ------------------------------------------------------

    // 全 panel の 1 フレーム分を駆動: 順に
    //   1) 各 panel の OnFrameBegin(dt) を呼ぶ (visible / 非 visible を問わず)
    //   2) DockSpace を描画 (`_enable_dockspace` が true のとき)
    //   3) MenuBar を描画 (`_enable_menu_bar` が true のとき)
    //   4) 各 panel の DrawUI を順に呼ぶ
    // OnFrameBegin の例外的 早期 return は持たない (= visible flag は DrawUI 側
    // で判断する規約。OnFrameBegin はバックグラウンド処理含む可能性があるため)。
    void TickAllPanels(f32 dt) noexcept;

    // ImGui::DockSpaceOverViewport で main viewport にドック空間を出す。
    // `_no_docking_in_central_node` が true の場合は central node を split せず
    // 単一 viewport として扱う (= ImGuiDockNodeFlags_NoDockingInCentralNode 相当)。
    // TickAllPanels から自動呼出しされるが、自前 main loop に組み込みたい host は
    // 直接呼んで良い (多重描画はしない: ImGui 側で同名 DockSpace が二重生成
    // されないよう内部で一意 ID を使う)。
    //
    // 注意: ACS の ImGui は現状 master branch (= docking branch 未統合) のため、
    //       本関数は **`IMGUI_HAS_DOCK` 未定義時は no-op** になる。各 panel は
    //       通常の float window として並ぶ (= editor として動作はするが dock
    //       UI 統合は無し)。Phase 21c 以降で ImGui docking branch に切り替えた
    //       時点で自動的に有効化される。
    void DrawDockSpace() noexcept;

    // MainMenuBar 内に "FWindow" / "Layout" メニューを追加する。
    //   FWindow : 各登録 panel の visibility toggle (チェックボックス)
    //   Layout : Save / Load Default ボタン
    // 既存の FApplication-side MenuBar と共存させたい場合は host 側で先に
    // BeginMainMenuBar / EndMainMenuBar を呼ぶか、`SetEnableMenuBar(false)` で
    // 本 workspace 側を抑制すること。
    void DrawMenuBar() noexcept;

    // ----- レイアウト永続化 --------------------------------------------------

    // 現在の ImGui ini + 各 panel の visible / dock_target 状態を `.acslayout`
    // 形式テキストファイルに書き出す。既存ファイルは上書き。失敗時は silent
    // (ACS_LOG_WARN にログするが戻り値で通知はしない: 終了時呼び出しなど致命的
    // でないケースが多いため)。
    void SaveLayout(const wchar_t* file_path) noexcept;

    // SaveLayout で書き出した `.acslayout` を復元する。version 不一致 / 構文エラー
    // / 未登録 panel title は安全に skip する。失敗時も silent。
    void LoadLayout(const wchar_t* file_path) noexcept;

    // ----- FSelectionService 連携 (Phase 20) ---------------------------------

    // FSelectionService 参照を登録 / 解除 (nullptr で解除)。caller 所有 (non-owning)。
    // 注入されると BroadcastSelectionChanged 経由で各 panel の OnSelectionChanged
    // が呼ばれた際の引数として渡される。FSelectionService 側の callback 購読は
    // panel が独自に行う (本 workspace は購読しない)。
    void SetSelectionService(inspector::FSelectionService* svc) noexcept;

    // 現在登録されている FSelectionService。未注入時は nullptr。
    inspector::FSelectionService* GetSelectionService() const noexcept;

    // 全 panel に OnSelectionChanged を呼ぶ。FSelectionService 未注入時は no-op
    // (panel 側の callback シグネチャ上、FSelectionService 参照が必要なため)。
    void BroadcastSelectionChanged() noexcept;

    // 全 panel に OnAssetSelected を呼ぶ。`asset_path` は本 workspace では
    // 保持せず、呼び出しごとに pass-through するだけ (= FAssetBrowser 所有想定)。
    // nullptr 渡しは "選択解除" として全 panel に伝播 (= panel 側の規約に従う)。
    void BroadcastAssetSelected(const char* asset_path) noexcept;

    // ----- 動作フラグ --------------------------------------------------------
    // DockSpace の central node 内 docking を無効化する (= central node を
    // 「メインビュー」専用として固定したい時に true)。default false。
    void SetNoDockingInCentralNode(bool b) noexcept { _no_docking_in_central_node = b; }
    bool NoDockingInCentralNode() const noexcept { return _no_docking_in_central_node; }

    // DockSpace 自動描画の on/off。既存 host が独自 DockSpace を持つ場合は false。
    void SetEnableDockSpace(bool b) noexcept { _enable_dockspace = b; }
    bool IsDockSpaceEnabled() const noexcept { return _enable_dockspace; }

    // MainMenuBar 自動描画の on/off。既存 host が MainMenuBar を持つなら false。
    void SetEnableMenuBar(bool b) noexcept { _enable_menu_bar = b; }
    bool IsMenuBarEnabled() const noexcept { return _enable_menu_bar; }

    // ----- 公開定数 ----------------------------------------------------------
    // SaveLayout の magic / version。LoadLayout 側で同値を期待。
    static constexpr const char* kLayoutMagic   = "ACS_EDLAYOUT";
    static constexpr u32         kLayoutVersion = 1u;

    // 同時登録可能 panel 数の上限 (overflow ガード)。実用上 32 で十分。
    // 上限到達後の RegisterPanel は silent no-op。
    static constexpr u32         kMaxPanels     = 32u;

private:
    // panel index 探索ヘルパ (== ポインタ完全一致 / nullptr は false 返却)。
    // ヒットしない場合は `kInvalidIndex` を返す。
    i32 FindPanelIndex(const FEditorPanel* panel) const noexcept;

    static constexpr i32 kInvalidIndex = -1;

    // 登録 panel 群 (raw pointer / 非所有)。順序 = 登録順 = dispatch 順。
    TArray<FEditorPanel*>          _panels;

    // FSelectionService (non-owning)。未注入時 nullptr。
    inspector::FSelectionService* _selection_service          = nullptr;

    // 動作フラグ群 (詳細は setter のコメント参照)。
    bool                         _no_docking_in_central_node = false;
    bool                         _enable_dockspace           = true;
    bool                         _enable_menu_bar            = true;
};

} // namespace acs::game::editor_core
