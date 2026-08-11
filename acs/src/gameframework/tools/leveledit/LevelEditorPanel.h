// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "gameframework/tools/editor_core/EditorPanel.h"
#include "gameframework/tools/editor_core/EditorCamera.h"

namespace acs::game {
// 編集対象の FTilemap は本ヘッダから forward-decl のみで受ける。
// `<gameframework/Tilemap.h>` を include しないことで、本 panel を利用する側が
// ヘッダ依存を最小化できる (= FTilemap 自体の変更で不要な再ビルドを避ける)。
class FTilemap;
} // namespace acs::game

namespace acs::game::leveledit {

/**
 * ペイントブラシの種類。
 *
 * @details
 * Paint=塗り (current_tile_id を SetTile、drag 対応)、Erase=消し
 * (FTileId{0} を SetTile、drag 対応)、Fill=flood-fill (クリック位置の連結成分を
 * current_tile_id で塗替、click 単発)、Pick=スポイト (クリック位置の tile id を
 * current_tile_id にコピー、click 単発)。
 */
enum class EBrushKind : u8 {
    /** 塗り。current_tile_id を SetTile する (drag で連続塗り対応)。 */
    Paint = 0,

    /** 消し。FTileId{0} を SetTile する (drag 対応)。 */
    Erase = 1,

    /** flood-fill。クリック位置の連結成分を current_tile_id で塗り替える (click 単発)。 */
    Fill  = 2,

    /** スポイト。クリック位置の tile id を current_tile_id にコピーする (click 単発)。 */
    Pick  = 3,
};

/**
 * FTilemap (multi-layer u16 FTileId grid) を対話的に編集する tilemap painter panel。
 *
 * @details
 * Paint / Erase / Fill / Pick の 4 ブラシ + アクティブレイヤ切替 + tile id picker +
 * grid show/hide + snap-to-grid を提供する。AEditorPanel 基底に載せ、Workspace へ 1 行
 * で統合できる。FTilemap データは caller 所有で raw 参照として受け取り、本 panel は
 * tilemap を生成・破棄しない。描画は ImDrawList で色付き矩形を積む placeholder
 * (本物テクスチャ atlas は未対応)。内部に 2D mode の CEditorCamera を内包し pan/zoom する。
 */
class ALevelEditorPanel : public acs::game::editor_core::AEditorPanel {
public:
    /** 空状態で構築する (内部 state は Init で初期化)。 */
    ALevelEditorPanel() noexcept = default;

    /** 破棄する (tilemap は caller 所有なので解放しない)。 */
    ~ALevelEditorPanel() noexcept override = default;

    /** コピー禁止 (基底 AEditorPanel と同規約、内部 CEditorCamera も非コピー)。 */
    ALevelEditorPanel(const ALevelEditorPanel&)            = delete;

    /** コピー代入も禁止。 */
    ALevelEditorPanel& operator=(const ALevelEditorPanel&) = delete;

    /** ムーブ禁止 (tilemap 参照・brush state の所有を曖昧にしないため)。 */
    ALevelEditorPanel(ALevelEditorPanel&&)                 = delete;

    /** ムーブ代入も禁止。 */
    ALevelEditorPanel& operator=(ALevelEditorPanel&&)      = delete;

    /**
     * 内部 state をデフォルトに初期化する。
     *
     * @details
     * CEditorCamera を 2D mode で Init し base ortho size を 512 に、tilemap 参照を
     * nullptr、brush=Paint、tile_id=1、active_layer=0、show_grid=true、snap_to_grid=true、
     * selected coord を unset にする。多重呼び出し可 (= 完全リセット)。
     */
    void Init() noexcept;

    /**
     * 内部 state を全解放する。
     *
     * @details
     * tilemap 参照を解除し CEditorCamera を Reset、selected coord をリセットする。
     * tilemap 自体は caller 所有なので本 panel は破棄しない。多重呼び出し可。
     */
    void Shutdown() noexcept;

    /**
     * 編集対象の FTilemap を raw 参照でセットする (nullptr で解除)。
     *
     * @details
     * 寿命は caller 責任 (本 panel は tilemap を所有しない)。セット直後は active_layer を
     * LayerCount() でクランプし、selected coord をリセットする (= 別 tilemap に切替えたら
     * 前 tilemap の coord は無意味)。
     * @param tm 編集対象の FTilemap (nullptr で解除)。
     */
    void SetTilemap(class FTilemap* tm) noexcept;

    /**
     * 現在編集対象の FTilemap を返す。
     *
     * @return 編集対象の FTilemap (未バインド時は nullptr)。
     */
    class FTilemap* CurrentTilemap() const noexcept;

    /**
     * 内部 CEditorCamera (Mode2D) への参照を返す。
     *
     * @details 呼出側が HandleMouseInput / Tick を呼ぶ。寿命は本 panel と同一。
     * @return 内部 CEditorCamera への参照。
     */
    acs::game::editor_core::CEditorCamera& Camera() noexcept;

    /**
     * 現在のブラシ種別を返す。
     *
     * @return 現在の EBrushKind。
     */
    EBrushKind CurrentBrush() const noexcept;

    /**
     * ブラシ種別を設定する。
     *
     * @param b 設定するブラシ種別。
     */
    void SetCurrentBrush(EBrushKind b) noexcept;

    /**
     * 現在ペイント中の tile id を返す。
     *
     * @details Paint / Fill / Pick で使用 (Erase では未使用)。
     * @return 現在の tile id。
     */
    u16 CurrentTileId() const noexcept;

    /**
     * ペイントする tile id を設定する。
     *
     * @details kTileIdMax (= 1023) を超える値はクランプされる。
     * @param id 設定する tile id。
     */
    void SetCurrentTileId(u16 id) noexcept;

    /**
     * アクティブレイヤ番号を返す。
     *
     * @return アクティブレイヤ番号 (FTilemap::LayerCount() 未満)。
     */
    u32 ActiveLayer() const noexcept;

    /**
     * アクティブレイヤ番号を設定する。
     *
     * @details
     * tilemap バインド時は LayerCount() でクランプ、未バインド時はそのまま保持し
     * 後の SetTilemap でクランプされる。
     * @param layer 設定するレイヤ番号。
     */
    void SetActiveLayer(u32 layer) noexcept;

    /**
     * grid 表示フラグを返す。
     *
     * @return grid を描画するなら true。
     */
    bool ShowGrid() const noexcept;

    /**
     * grid 表示フラグを設定する。
     *
     * @param b true なら ImDrawList で grid line を描く。
     */
    void SetShowGrid(bool b) noexcept;

    /**
     * snap-to-grid 表示フラグを返す。
     *
     * @return snap-to-grid 表示なら true。
     */
    bool SnapToGrid() const noexcept;

    /**
     * snap-to-grid 表示フラグを設定する。
     *
     * @details ホバー時のハイライト矩形を tile 単位に揃える表示用フラグ (ペイント自体は常に tile 単位)。
     * @param b true なら snap 表示する。
     */
    void SetSnapToGrid(bool b) noexcept;

    /**
     * window タイトルを返す (ImGui::Begin の引数兼 ID)。
     *
     * @return 固定リテラル "Level Editor"。
     */
    const char* Title() const noexcept override { return "Level Editor"; }

    /**
     * Workspace への登録時に呼ばれる初期化フック。
     *
     * @details
     * 基底実装で Workspace ポインタを保存したあと、CEditorCamera を 2D mode で
     * 初期化し直す (Init() 未呼出でも登録だけで動くようにする保険)。
     * @param workspace 登録先のエディタワークスペース。
     */
    void OnInit(acs::game::editor_core::CEditorWorkspace& workspace) noexcept override;

    /**
     * Toolbar + viewport canvas + inspector を ImGui で描画する。
     *
     * @details
     * ImGui::Begin "Level Editor" 内に Toolbar (Brush / Layer / Tile ID / Show Grid /
     * Snap)、viewport canvas (tilemap 矩形描画 + ホバー強調 + クリック/drag ペイント)、
     * inspector を出す。tilemap 未バインドなら "(No tilemap bound)" を表示して終了する。
     */
    void DrawUI() noexcept override;

    /** tile id picker (UI) の上限 (本物 atlas 整備までの暫定値、SetCurrentTileId でクランプに使う)。 */
    static constexpr u16 kTileIdMax = 1023u;

    /** flood-fill (Brush=Fill) の最大処理セル数 (巨大マップでの暴走を防ぐ上限)。 */
    static constexpr u32 kFloodFillMaxCells = 4096u;

    /** "未選択" を表す sentinel coord (= u32 max)。inspector が "(none)" を出す判定に使う。 */
    static constexpr u32 kNoCoord = 0xFFFFFFFFu;

private:
    /** 2D viewport camera (pan / zoom)。Init() で Mode2D に初期化する。 */
    acs::game::editor_core::CEditorCamera m_Camera {};

    /** 編集対象 FTilemap (caller 所有、本 panel は非所有)。 */
    class FTilemap* m_Tilemap = nullptr;

    /** 現在のブラシ種別。 */
    EBrushKind m_Brush          = EBrushKind::Paint;

    /** 現在ペイント中の tile id (初期値 1 で初手 Paint が空でない tile を置く)。 */
    u16        m_CurrentTileId = 1u;

    /** アクティブレイヤ番号。 */
    u32        m_ActiveLayer    = 0u;

    /** grid 表示フラグ。 */
    bool       m_ShowGrid       = true;

    /** snap-to-grid 表示フラグ。 */
    bool       m_SnapToGrid    = true;

    /** inspector 用「最後にクリック / ホバーした tile」の X 座標 (未選択は kNoCoord)。 */
    u32        m_SelectedX      = kNoCoord;

    /** inspector 用「最後にクリック / ホバーした tile」の Y 座標 (未選択は kNoCoord)。 */
    u32        m_SelectedY      = kNoCoord;
};

using FLevelEditorPanel = ALevelEditorPanel;

} // namespace acs::game::leveledit
