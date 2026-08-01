// SPDX-License-Identifier: Apache-2.0
// GameFramework Tools — editor_core / CEditorWorkspace
//
// 複数の `AEditorPanel` (ModelViewer / AnimCurveEditor / BehaviorTreeEditor /
// LevelEditor / SpriteAtlasEditor / FontEditor / CinematicsTimelineEditor 等) を
// 統括する **ワークスペース**。panel 群を 1 つの editor アプリケーションとして
// まとめて配線するための中央 hub。
//
// 役割:
//   ・登録 panel のリスト管理 (PushBack / Find / Remove)
//   ・毎フレームの main loop coordination
//       (OnFrameBegin → DockSpace 描画 → DrawUI → MenuBar)
//   ・ImGui DockSpace の作成 + Window メニュー (panel toggle list)
//   ・レイアウト永続化 (ImGui ini + per-panel state を `.acslayout` 1 ファイル)
//   ・選択 / asset 選択イベントの全 panel への broadcast
//   ・CSelectionService の保管点 (非所有)
//
// 使い方 (典型):
//   acs::game::editor_core::CEditorWorkspace ws;
//   ws.Init();
//
//   ws.SetSelectionService(&selection);   // CSelectionService を注入
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
// 設計選択:
//   ・**panel は raw pointer の非所有保持**: caller が own する (= caller が
//     panel の lifetime を制御する) ことで、panel の動的生成 / scope-stack 配置
//     の両方を許容する。AParticleEditorPanel / AEditorToolbar と同形。
//   ・**`acs::TArray<AEditorPanel*>` で順序保持**: dispatch 順 / Window メニュー
//     の表示順 = 登録順。登録順以外のソートはしない。
//   ・**`UnregisterPanel` は順序保存削除**: Window メニューの並びがフレーム間で
//     ぶれないよう、swap-remove ではなく shift 削除。CSelectionService の
//     RemoveAtSwap とは方針を変える (UI 表示順の体験を優先)。
//   ・**Title はリテラル文字列を期待**: `AEditorPanel::Title()` の規約 (リテラル /
//     静的領域) に依存する。`FindPanelByTitle` は strcmp 比較。
//   ・**ImGui::DockSpaceOverViewport**: 「メインビューポート全体に central dock
//     node を持つ」最小構成。central node 内で float window 動作させたい panel は
//     `SetDockTarget(false)` でヒントを出せるが、現状は dock_target hint を
//     DockSpace に強制反映する仕組みは持たない。
//   ・**Window メニュー / Layout メニュー は `DrawMenuBar()` 内で MainMenuBar に
//     直接 push**: 派生コードからは TickAllPanels を呼ぶだけで自動描画される。
//     MenuBar の有無は `m_EnableMenuBar` で制御可能 (= 既存 MainMenuBar に
//     共存させたい host は disable できる)。
//   ・**SaveLayout / LoadLayout のファイル形式**: 自前テキストフォーマット
//     `ACS_EDLAYOUT 1`。
//       1) ヘッダ行  : `ACS_EDLAYOUT <version>`
//       2) ImGui ini : `IMGUI_INI <byte_size>\n<raw ini bytes>\n`
//       3) panel state: `PANEL <title> <visible:0/1> <dock_target:0/1>\n` (1 行 1 panel)
//     PANEL 行は右端 2 token を flag として解析するため、title 内部の ASCII space
//     (0x20) を保持できる。空 title、先頭/末尾 space、制御文字、非 ASCII は拒否する。
//     ImGui ini は ImGui::SaveIniSettingsToMemory() で取得した raw 文字列を
//     生埋め込み。LoadLayout 側で同じく LoadIniSettingsFromMemory に渡す。
//   ・**BroadcastSelectionChanged / BroadcastAssetSelected** は全 panel への
//     fan-out: 戻り値は無く、panel 側で必要に応じて自身に反映する。null safe。
//   ・**非コピー / 非ムーブ / 全 noexcept / STL 不使用**: ACS 規約。
//   ・**ImGui ヘッダは .cpp 側のみ include**: header からは imgui 依存を漏らさず、
//     AParticleEditorPanel / AInspectorPanel と同方針。
//
// 範囲外 (本クラスでは持たない):
//   ・panel の生成 / 破棄 (= caller 責務)
//   ・ImGui Context / GPU resource 管理 (= 外側の ImGuiBackend が持つ)
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "gameframework/Forward.h"

namespace acs::game::editor_core {

/** `.acslayout` checked persistence の安定したエラー種別。 */
enum class EEditorWorkspacePersistenceError : u8 {
    None = 0,
    NullArgument,
    PathTooLong,
    InputTooLarge,
    EmbeddedNul,
    TooManyLines,
    LineTooLong,
    BadMagic,
    UnsupportedVersion,
    InvalidSyntax,
    DuplicateSection,
    DuplicatePanel,
    TooManyPanels,
    TitleTooLong,
    InvalidTitle,
    IniTooLarge,
    TruncatedIni,
    TrailingData,
    ImGuiContextMissing,
    AllocationFailure,
    FileNotFound,
    FileOpenFailed,
    FileSizeFailed,
    FileChanged,
    FileReadFailed,
    FileWriteFailed,
    FileFlushFailed,
    FileCloseFailed,
    AtomicReplaceFailed,
};

/** `.acslayout` checked load/save の結果。 */
struct FEditorWorkspacePersistenceResult {
    EEditorWorkspacePersistenceError error =
        EEditorWorkspacePersistenceError::None;
    u32 line = 0u;
    u32 panel_entries = 0u;
    u64 bytes_processed = 0u;
    u32 os_error = 0u;

    bool Succeeded() const noexcept {
        return error == EEditorWorkspacePersistenceError::None;
    }
    static const char* ErrorName(
        EEditorWorkspacePersistenceError error) noexcept;
};

/**
 * 複数の AEditorPanel を統括するワークスペース hub。
 *
 * @details
 * 登録 panel のリスト管理 (RegisterPanel / FindPanelByTitle / UnregisterPanel)、
 * 毎フレームの main loop coordination (OnFrameBegin → DockSpace → MenuBar →
 * DrawUI)、ImGui DockSpace と Window / Layout メニューの描画、レイアウトの
 * `.acslayout` 永続化、選択 / asset 選択イベントの全 panel への broadcast を担う。
 * panel は raw pointer の非所有保持で順序 = 登録順 = dispatch 順。CSelectionService
 * も非所有参照で保持する。所有 / 参照関係を曖昧にしないため非コピー・非ムーブ。
 */
class CEditorWorkspace {
public:
    /** 空状態で構築する (初期化は Init で行う)。 */
    CEditorWorkspace() noexcept = default;

    /** 破棄する (登録 panel は非所有なので解放しない)。 */
    ~CEditorWorkspace() noexcept = default;

    /** コピー禁止 (panel リストと CSelectionService 参照の所有関係を曖昧にしないため)。 */
    CEditorWorkspace(const CEditorWorkspace&)            = delete;

    /** コピー代入も禁止。 */
    CEditorWorkspace& operator=(const CEditorWorkspace&) = delete;

    /** ムーブ禁止。 */
    CEditorWorkspace(CEditorWorkspace&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CEditorWorkspace& operator=(CEditorWorkspace&&)      = delete;

    /**
     * panel list を空、CSelectionService を nullptr、設定値を default にして初期化する。
     *
     * @details
     * 多重 Init 可 (完全リセットとして使える)。登録 panel に OnShutdown を呼ばずに
     * list を空にするので、Init 前に必ず Shutdown を済ませること。
     */
    void Init() noexcept;

    /**
     * 登録済み全 panel に OnShutdown を呼んでから list を解放する。
     *
     * @details
     * CSelectionService 参照も解除する。動作フラグ類は再 Init で同じ host 設定を
     * 引き継げるよう意図的にリセットしない。多重 Shutdown 可。
     */
    void Shutdown() noexcept;

    /**
     * panel を末尾に追加して OnInit を呼ぶ。
     *
     * @details
     * 所有は caller。二重登録 (同一ポインタ) / nullptr / Title() == nullptr は no-op。
     * kMaxPanels 到達後も silent no-op。
     * @param panel 登録する panel (非所有)。
     */
    void RegisterPanel(AEditorPanel* panel) noexcept;

    /**
     * panel を登録解除して OnShutdown を呼ぶ。
     *
     * @details 順序保存削除 (shift)。未登録 / nullptr は no-op。
     * @param panel 登録解除する panel。
     */
    void UnregisterPanel(AEditorPanel* panel) noexcept;

    /**
     * 現在の登録 panel 数を返す。
     *
     * @return 登録 panel 数。
     */
    u32 PanelCount() const noexcept;

    /**
     * index 番目の panel ポインタを返す。
     *
     * @param i panel のインデックス。
     * @return i 番目の panel (範囲外なら nullptr)。
     */
    AEditorPanel* GetPanelByIndex(u32 i) const noexcept;

    /**
     * Title が完全一致する panel を返す。
     *
     * @details 比較は strcmp。
     * @param title 探す panel の Title (nullptr なら nullptr)。
     * @return 一致した panel (なければ nullptr)。
     */
    AEditorPanel* FindPanelByTitle(const char* title) const noexcept;

    /**
     * Title が完全一致する panel の可視状態を反転する。
     *
     * @details 該当 panel の SetVisible(!IsVisible()) を呼ぶ。未発見 / null は no-op。
     * @param title 反転対象の panel の Title。
     */
    void TogglePanelVisible(const char* title) noexcept;

    /**
     * 全 panel の 1 フレーム分を駆動する。
     *
     * @details
     * 順に 1) 各 panel の OnFrameBegin(dt) (visible を問わず全 panel)、
     * 2) DockSpace 描画 (m_EnableDockspace が true のとき)、
     * 3) MenuBar 描画 (m_EnableMenuBar が true のとき)、
     * 4) 各 panel の DrawUI を呼ぶ。visibility は DrawUI 側で判断する規約。
     * @param dt 前フレームからの経過秒。
     */
    void TickAllPanels(f32 dt) noexcept;

    /**
     * main viewport に ImGui DockSpace を出す。
     *
     * @details
     * m_NoDockingInCentralNode が true なら central node の docking を無効化する。
     * 多重呼び出しは ImGui 側で安全。ACS の ImGui は現状 master branch (docking
     * branch 未統合) のため IMGUI_HAS_DOCK 未定義時は no-op になり、各 panel は通常の
     * float window として並ぶ (docking branch 切替時に自動有効化)。
     */
    void DrawDockSpace() noexcept;

    /**
     * MainMenuBar 内に "Window" / "Layout" メニューを追加描画する。
     *
     * @details
     * Window は各登録 panel の visibility toggle、Layout は Save / Load Default
     * ボタンを持つ。既存 host が MainMenuBar を持つ場合は SetEnableMenuBar(false) で
     * 本 workspace 側を抑制する。
     */
    void DrawMenuBar() noexcept;

    /**
     * 現在のレイアウトを `.acslayout` テキストファイルへ書き出す。
     *
     * @details
     * ImGui ini + 各 panel の visible / dock_target 状態を保存する。既存ファイルは
     * 上書き。失敗時は ACS_LOG_WARN のみで戻り値通知はしない (終了時呼び出しなど
     * 致命的でないケースが多いため)。file_path が nullptr なら no-op。
     * @param file_path 書き出し先のファイルパス。
     */
    void SaveLayout(const wchar_t* file_path) noexcept;

    /** SaveLayout の checked atomic 版。 */
    FEditorWorkspacePersistenceResult TrySaveLayout(
        const wchar_t* file_path) noexcept;

    /**
     * SaveLayout で書き出した `.acslayout` を復元する。
     *
     * @details version 不一致 / 構文エラー / 未登録 panel title は安全に skip。失敗時も silent。
     * @param file_path 読み込み元のファイルパス。
     */
    void LoadLayout(const wchar_t* file_path) noexcept;

    /** LoadLayout の checked transaction 版。 */
    FEditorWorkspacePersistenceResult TryLoadLayout(
        const wchar_t* file_path) noexcept;

    /**
     * 長さ付き `.acslayout` を全検証し、最後に ImGui/panel 状態だけを更新する。
     * 登録 panel 配列と selection service pointer は変更しない。PANEL title は内部
     * ASCII space のみ許可し、空・先頭/末尾 space・制御文字・非 ASCII を拒否する。
     */
    FEditorWorkspacePersistenceResult TryParseLayoutText(
        const char* text, usize text_size) noexcept;

    /**
     * CSelectionService 参照を登録 / 解除する。
     *
     * @details
     * caller 所有 (non-owning)。注入されると BroadcastSelectionChanged 経由で各 panel の
     * OnSelectionChanged の引数として渡される。CSelectionService 側の callback 購読は
     * panel が独自に行う (本 workspace は購読しない)。
     * @param svc 登録する CSelectionService (nullptr で解除)。
     */
    void SetSelectionService(inspector::CSelectionService* svc) noexcept;

    /**
     * 現在登録されている CSelectionService を返す。
     *
     * @return 登録済み CSelectionService (未注入時は nullptr)。
     */
    inspector::CSelectionService* GetSelectionService() const noexcept;

    /**
     * 全 panel に OnSelectionChanged を呼ぶ。
     *
     * @details CSelectionService 未注入時は no-op (callback シグネチャ上、参照が必要なため)。
     */
    void BroadcastSelectionChanged() noexcept;

    /**
     * 全 panel に OnAssetSelected を呼ぶ。
     *
     * @details
     * asset_path は本 workspace では保持せず呼び出しごとに pass-through する
     * (CAssetBrowser 所有想定)。nullptr 渡しは "選択解除" として全 panel に伝播する。
     * @param asset_path 選択された asset のパス (nullptr で選択解除)。
     */
    void BroadcastAssetSelected(const char* asset_path) noexcept;

    /**
     * DockSpace の central node 内 docking 無効化フラグを設定する。
     *
     * @param b true で central node を「メインビュー」専用として固定する (既定 false)。
     */
    void SetNoDockingInCentralNode(bool b) noexcept { m_NoDockingInCentralNode = b; }

    /**
     * central node 内 docking 無効化フラグを返す。
     *
     * @return 無効化中なら true。
     */
    bool NoDockingInCentralNode() const noexcept { return m_NoDockingInCentralNode; }

    /**
     * DockSpace の自動描画フラグを設定する。
     *
     * @param b false で自動描画を抑制する (既存 host が独自 DockSpace を持つ場合)。
     */
    void SetEnableDockSpace(bool b) noexcept { m_EnableDockspace = b; }

    /**
     * DockSpace 自動描画が有効かを返す。
     *
     * @return 有効なら true。
     */
    bool IsDockSpaceEnabled() const noexcept { return m_EnableDockspace; }

    /**
     * MainMenuBar の自動描画フラグを設定する。
     *
     * @param b false で自動描画を抑制する (既存 host が MainMenuBar を持つ場合)。
     */
    void SetEnableMenuBar(bool b) noexcept { m_EnableMenuBar = b; }

    /**
     * MainMenuBar 自動描画が有効かを返す。
     *
     * @return 有効なら true。
     */
    bool IsMenuBarEnabled() const noexcept { return m_EnableMenuBar; }

    /** `.acslayout` ファイル先頭の magic 文字列 (LoadLayout 側で同値を期待)。 */
    static constexpr const char* kLayoutMagic   = "ACS_EDLAYOUT";

    /** `.acslayout` ファイルフォーマットの現行バージョン。 */
    static constexpr u32         kLayoutVersion = 1u;

    /** 同時登録可能 panel 数の上限 (overflow ガード、到達後は silent no-op)。 */
    static constexpr u32         kMaxPanels     = 32u;

    static constexpr usize kMaxLayoutBytes = 4u * 1024u * 1024u;
    static constexpr usize kMaxIniBytes = 2u * 1024u * 1024u;
    static constexpr usize kMaxLayoutLineBytes = 255u;
    static constexpr u32 kMaxLayoutLines = 4096u;
    static constexpr usize kMaxPanelTitleBytes = 127u;
    static constexpr usize kMaxPersistencePathChars = 1023u;

private:
    /**
     * panel をポインタ完全一致で探索する。
     *
     * @param panel 探す panel (nullptr なら kInvalidIndex)。
     * @return 見つかったインデックス、ヒットしなければ kInvalidIndex。
     */
    i32 FindPanelIndex(const AEditorPanel* panel) const noexcept;

    /** FindPanelIndex が未ヒット時に返す番兵値。 */
    static constexpr i32 kInvalidIndex = -1;

    /** 登録 panel 群 (raw pointer / 非所有、順序 = 登録順 = dispatch 順)。 */
    TArray<AEditorPanel*>          m_Panels;

    /** CSelectionService (non-owning、未注入時 nullptr)。 */
    inspector::CSelectionService* m_SelectionService          = nullptr;

    /** central node 内 docking 無効化フラグ。 */
    bool                         m_NoDockingInCentralNode = false;

    /** DockSpace 自動描画フラグ。 */
    bool                         m_EnableDockspace           = true;

    /** MainMenuBar 自動描画フラグ。 */
    bool                         m_EnableMenuBar            = true;
};

} // namespace acs::game::editor_core
