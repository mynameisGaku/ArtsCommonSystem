// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — leveledit / FLevelEditorPanel (Phase 22)
//
// `acs::game::FTilemap` (multi-layer u16 FTileId grid) を **対話的に編集する
// tilemap painter panel**。Unity Tile Palette / Tiled / LDtk の「ペイント /
// 消し / 塗りつぶし / スポイト」 4 ブラシ + アクティブレイヤ切替 + tile id
// picker + grid show/hide + snap-to-grid を提供する。Phase 21a で確立された
// `editor_core::FEditorPanel` 基底に載せ、`FEditorWorkspace::RegisterPanel(&panel)`
// の 1 行で workspace に統合できる形にしている。
//
// 役割:
//   ・ImGui canvas 上に FTilemap を矩形描画 (= ImDrawList::AddRectFilled の
//     row-major スイープ、本物テクスチャは Phase 2 以降で差し替え)
//   ・上部 toolbar:
//       - Brush kind 選択 (Paint / Erase / Fill / Pick)
//       - Active layer dropdown (LayerCount 個の選択肢)
//       - Tile id picker (DragInt 0〜1023; Erase 時は disable)
//       - Show grid toggle
//       - Snap-to-grid toggle (= マウスホバー / クリック位置を tile 単位に
//         スナップ表示するか。ペイント自体は常に tile 単位)
//   ・中央 viewport: 2D camera (`FEditorCamera` Mode2D 内包) で pan / zoom
//   ・マウス操作:
//       - LMB drag (Paint)  : `FTilemap::SetTile(x, y, current_tile_id, layer)`
//       - LMB click (Erase) : `FTilemap::SetTile(x, y, FTileId{0}, layer)`
//       - LMB click (Fill)  : flood-fill (同じ tile id の連結成分を current_tile_id
//                              で塗替え)
//       - LMB click (Pick)  : クリック位置の tile id を current_tile_id にコピー
//   ・右側 inspector: 現在 selected tile coord (x, y, layer, current_tile_id)
//
// 役割分担:
//   ・本 panel は「**tilemap の編集 UI** だけ」を担当。実際の FTilemap データは
//     caller 所有 (= `SetTilemap(&tm)` で raw 参照を渡す、寿命は caller 責任)。
//     本 panel が tilemap を生成 / 破棄しない (FAnimCurveEditorPanel が
//     FAnimationCurve を non-owning で受けるのと同形)。
//   ・FTilemap の描画 (本物テクスチャ atlas) は **Phase 2 以降の責務**。Phase 22
//     範囲では「ImDrawList で色付き矩形を描く placeholder」のみ。tile id を
//     疑似乱数ハッシュで色に変換する `TileIdToColor` を内部実装する。
//   ・Save / Load tilemap は sample 34 側で stub menu を出す。本 panel 自身は
//     永続化 API を持たない (= JSON / binary / pak 等の選択を panel が知らない)。
//
// 使い方 (典型):
//   FLevelEditorPanel panel;
//   panel.Init();
//   workspace.RegisterPanel(&panel);   // FEditorPanel として登録
//
//   FTilemap map;
//   map.Init(32, 32, /*layer_count=*/2, /*tile_size=*/16.0f);
//   map.FillRect(0, 0, 31, 0, FTileId{1}, 0);  // 床
//   panel.SetTilemap(&map);
//
//   // 毎フレーム TickAllPanels(dt) の中で OnFrameBegin + DrawUI が呼ばれる。
//
//   // 終了時:
//   workspace.UnregisterPanel(&panel);
//   panel.Shutdown();
//
// 設計選択 (Phase 22 LevelEditor):
//   ・**FEditorPanel 継承**: Phase 21a 共通基盤の dogfood (sample 31 ModelViewer /
//     sample 32 AnimCurveEditor に続く 3 つ目)。Title = "Level Editor"、
//     DrawUI override。
//   ・**tilemap は raw pointer の非所有保持**: caller 所有 (FAnimCurveEditorPanel
//     が FAnimationCurve を non-owning で受けるのと同方針)。本 panel は tilemap
//     の寿命に関与せず、`_tilemap == nullptr` 時は "(No tilemap bound)" を表示。
//   ・**FEditorCamera Mode2D を内包**: 各 panel が独自 camera を持つ Unity
//     SceneView 風モデル (FModelViewerPanel と同形)。FCamera() アクセサで参照を
//     返し、外部 (sample 34) は HandleMouseInput / Tick を呼ぶ。本 panel 内部の
//     viewport drawing でも `_camera.State().zoom_2d` と `_camera.State().position`
//     を使って world → screen 変換する。
//   ・**EBrushKind は u8 enum**: ACS の E prefix enum 規約。Paint=塗り (drag 対応)、
//     Erase=消し (FTileId{0} 強制)、Fill=flood-fill (連結成分塗替)、Pick=スポイト
//     (クリック位置の tile id をコピー)。
//   ・**snap_to_grid は表示用フラグ**: tile 単位編集なのでペイント自体は常に
//     snap される。snap_to_grid=true の時は「マウスホバー位置の tile を強調表示」
//     (= grid line を太く描くなど) する見た目フラグとして使う。
//   ・**ImGui canvas は ImGui::InvisibleButton + GetWindowDrawList()**: AnimCurve
//     FEditorPanel と同パターン (ImGui Demo の Canvas example と同形)。
//   ・**flood-fill の上限 sentinel**: FTilemap 全 cell 数 (= 32*32=1024 程度)
//     を超える stack 深さは想定しないが、巨大マップでも暴走しないよう
//     `kFloodFillMaxCells` で打ち切り (= 64*64=4096)。
//   ・**Tile id 範囲は 0〜1023 (u16 だが UI 上で打ち切り)**: 仕様で指定された
//     範囲。本物 atlas が出来たら index 上限は atlas size に合わせて拡張する。
//     `kTileIdMax = 1023` を公開定数として出す。
//   ・**全 noexcept / 非コピー / 非ムーブ / STL 不使用 / `<string>` 禁止**:
//     ACS 規約。
//   ・**ImGui ヘッダは .cpp 限定**: 他 panel (FParticleEditorPanel /
//     FModelViewerPanel / FInspectorPanel) と同形。
//
// 範囲外 (Phase 22 では持たない、将来追加候補):
//   ・本物テクスチャ atlas でのタイル描画 (= 現状は色付き矩形 placeholder)
//   ・複数 tilemap の同時編集 / レイヤ重ね表示の z-order 操作
//   ・undo / redo 統合 (= Phase 21b FUndoStack 経由、将来 OnUndo / OnRedo hook)
//   ・tilemap のシリアライズ (= sample 34 menu で stub、本物は別 phase)
//   ・矩形選択 / コピー&ペースト / 移動 / 回転
//   ・auto-tiling (= Wang tiles / 4-bit neighbor mask)
//   ・collision layer の特別扱い (= 現状はただの layer)
//   ・タイル単位 flip / rotate (= FTileId 自体は純粋 id のまま、flag は別配列が必要)
#pragma once

#include "foundation/Types.h"
#include "gameframework/tools/editor_core/EditorPanel.h"
#include "gameframework/tools/editor_core/EditorCamera.h"

namespace acs::game {
// 編集対象の FTilemap は本ヘッダから forward-decl のみで受ける。
// `<gameframework/FTilemap.h>` を include しないことで、本 panel を利用する側が
// ヘッダ依存を最小化できる (= FTilemap 自体の変更で不要な再ビルドを避ける)。
class FTilemap;
} // namespace acs::game

namespace acs::game::leveledit {

// ---------------------------------------------------------------------------
// EBrushKind — ペイントブラシの種類
// ---------------------------------------------------------------------------
// Paint=塗り (current_tile_id を SetTile、drag 対応)
// Erase=消し (FTileId{0} を SetTile、drag 対応)
// Fill =flood-fill (クリック位置の連結成分を current_tile_id で塗替、click 単発)
// Pick =スポイト (クリック位置の tile id を current_tile_id にコピー、click 単発)
// ---------------------------------------------------------------------------
enum class EBrushKind : u8 {
    Paint = 0,
    Erase = 1,
    Fill  = 2,
    Pick  = 3,
};

// ---------------------------------------------------------------------------
// FLevelEditorPanel — FTilemap (multi-layer u16 FTileId grid) 編集 UI panel
// ---------------------------------------------------------------------------
class FLevelEditorPanel : public acs::game::editor_core::FEditorPanel {
public:
    FLevelEditorPanel() noexcept = default;
    ~FLevelEditorPanel() noexcept override = default;

    // 非コピー / 非ムーブ: 基底 FEditorPanel と同規約 + 内部 FEditorCamera
    // (非コピー / 非ムーブ) + tilemap 参照 / brush state の所有を曖昧にしない
    // (ACS 規約)。
    FLevelEditorPanel(const FLevelEditorPanel&)            = delete;
    FLevelEditorPanel& operator=(const FLevelEditorPanel&) = delete;
    FLevelEditorPanel(FLevelEditorPanel&&)                 = delete;
    FLevelEditorPanel& operator=(FLevelEditorPanel&&)      = delete;

    // ----- 初期化 / 解放 ---------------------------------------------------

    // 内部 state をデフォルトに、FEditorCamera を 2D mode で Init、tilemap 参照を
    // nullptr、brush=Paint、tile_id=1、active_layer=0、show_grid=true、
    // snap_to_grid=true、selected coord を unset に。多重 Init 可 (= 完全リセット)。
    void Init() noexcept;

    // 内部 state を全解放 (= tilemap 参照を解除、FEditorCamera を Reset)。
    // tilemap 自体は caller 所有なので本 panel が破棄しない。多重 Shutdown 可。
    void Shutdown() noexcept;

    // ----- tilemap バインド ------------------------------------------------

    // 編集対象の FTilemap を raw 参照でセット (nullptr で解除)。
    // 寿命は caller 責任 (本 panel は tilemap を所有しない)。
    // セット直後は active_layer を 0 にクランプ、selected coord をリセット
    // (= 別 tilemap に切替えたら前 tilemap の coord は無意味)。
    void SetTilemap(class FTilemap* tm) noexcept;

    // 現在編集対象の FTilemap (未バインド時は nullptr)。
    class FTilemap* CurrentTilemap() const noexcept;

    // ----- FEditorCamera ----------------------------------------------------

    // 内部 FEditorCamera (Mode2D) への参照。呼出側 (sample 34) が
    // `HandleMouseInput` / `Tick` を呼ぶ。寿命は本 panel と同一。
    acs::game::editor_core::FEditorCamera& Camera() noexcept;

    // ----- ブラシ / レイヤ / tile id 設定 ----------------------------------

    EBrushKind CurrentBrush() const noexcept;
    void SetCurrentBrush(EBrushKind b) noexcept;

    // 現在ペイント中の tile id (Paint / Fill / Pick で使用、Erase では未使用)。
    // 上限は `kTileIdMax` (= 1023) でクランプ。
    u16 CurrentTileId() const noexcept;
    void SetCurrentTileId(u16 id) noexcept;

    // アクティブレイヤ番号 (`FTilemap::LayerCount()` 未満)。
    // tilemap 未バインド時はそのまま値を保持、バインド後に LayerCount で
    // クランプされる。
    u32 ActiveLayer() const noexcept;
    void SetActiveLayer(u32 layer) noexcept;

    // grid 表示 toggle (= ImDrawList で grid line を描くか)。
    bool ShowGrid() const noexcept;
    void SetShowGrid(bool b) noexcept;

    // snap-to-grid 表示 toggle (= ホバー時のハイライト矩形を tile 単位に
    // 揃えるか。ペイント自体は tile 単位なのでこのフラグは表示用)。
    bool SnapToGrid() const noexcept;
    void SetSnapToGrid(bool b) noexcept;

    // ----- FEditorPanel override -------------------------------------------

    // window タイトル (ImGui::Begin の引数兼 ID)。固定リテラル。
    const char* Title() const noexcept override { return "Level Editor"; }

    // Workspace への登録時に呼ばれる。基底実装で Workspace ポインタ保存、
    // 本クラスでは追加初期化 (FEditorCamera を 2D mode で Init し直す保険) を行う。
    void OnInit(acs::game::editor_core::FEditorWorkspace& workspace) noexcept override;

    // ImGui::Begin "Level Editor" + Toolbar + Canvas + Inspector を描画。
    // tilemap 未バインドなら "(No tilemap bound)" を表示してセクションは出さない。
    void DrawUI() noexcept override;

    // ----- 公開定数 --------------------------------------------------------

    // tile id picker (UI) の上限。仕様で 0〜1023 を要求 (本物 atlas 整備までの
    // 暫定値)。`SetCurrentTileId` でクランプに使う。
    static constexpr u16 kTileIdMax = 1023u;

    // flood-fill (Brush=Fill) の最大処理セル数。FTilemap 全 cell を超える深さの
    // 暴走を防ぐための上限。32*32 想定なら 1024 で十分、巨大マップでも 4096 で
    // 打ち切り (= タイル数が 4096 を超える領域は塗り切れない、警告も出さない簡易実装)。
    static constexpr u32 kFloodFillMaxCells = 4096u;

    // "未選択" を表す sentinel coord (= u32 max)。selected_x / selected_y が
    // この値の時は inspector が "(none)" を出す。
    static constexpr u32 kNoCoord = 0xFFFFFFFFu;

private:
    // 2D viewport camera (pan / zoom)。Init() で Mode2D に初期化。
    acs::game::editor_core::FEditorCamera _camera {};

    // 編集対象 FTilemap (caller 所有、本 panel は非所有)。
    class FTilemap* _tilemap = nullptr;

    // ---- ブラシ / レイヤ / tile id ----
    EBrushKind _brush          = EBrushKind::Paint;
    u16        _current_tile_id = 1u;            // 1 にしておくと初手 Paint が「空でない tile」を置く
    u32        _active_layer    = 0u;

    // ---- 表示 toggle ----
    bool       _show_grid       = true;
    bool       _snap_to_grid    = true;

    // ---- inspector 用「最後にクリック / ホバーした tile coord」----
    u32        _selected_x      = kNoCoord;
    u32        _selected_y      = kNoCoord;
};

} // namespace acs::game::leveledit
