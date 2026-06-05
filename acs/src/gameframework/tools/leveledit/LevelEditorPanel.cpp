// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — FLevelEditorPanel 実装
//
// 仕様の意図は FLevelEditorPanel.h を参照。本ファイルでは:
//   ・FEditorPanel 基底 hook (OnInit / DrawUI) の override
//   ・SetTilemap / brush / layer / tile id / show grid / snap-to-grid のアクセサ
//   ・ImGui canvas (InvisibleButton + ImDrawList) ベースの tilemap 描画 +
//     マウス → world → tile 座標変換 + 4 ブラシ (Paint/Erase/Fill/Pick) の処理
//   ・flood-fill (Fill ブラシ) を反復スタック (= TArray<u32>) で実装
//   ・tile id を疑似乱数ハッシュで色化 (placeholder)
// を実装する。すべて noexcept、STL 不使用、ImGui 依存はこの .cpp に閉じる。

#include "gameframework/tools/leveledit/LevelEditorPanel.h"

#include "gameframework/Tilemap.h"
#include "gameframework/tools/editor_core/EditorWorkspace.h"

#include "container/Array.h"

#include <imgui.h>

#include <cstdio>   // std::snprintf (combo label 整形)

namespace acs::game::leveledit {

namespace {

/**
 * ブラシ種別を表示ラベルに変換する。
 *
 * @param b 変換するブラシ種別。
 * @return 対応するラベル文字列 (範囲外は "(unknown)")。
 */
const char* BrushLabel(EBrushKind b) noexcept {
    switch (b) {
        case EBrushKind::Paint: return "Paint";
        case EBrushKind::Erase: return "Erase";
        case EBrushKind::Fill:  return "Fill";
        case EBrushKind::Pick:  return "Pick";
        default:                return "(unknown)";
    }
}

/**
 * tile id を疑似乱数ハッシュで RGBA 色に変換する placeholder。
 *
 * @details
 * 本物 atlas が出来るまでの暫定描画用。0 (= 空) はほぼ透明な暗灰色で「下地が
 * 見える」感を出し、0 以外は splitmix32 風ハッシュで色相を散らす。各 channel は
 * 全黒近くを避けるため最低輝度 64 を保証する。
 * @param id 変換する tile id。
 * @return ImGui 用の RGBA 色 (ImU32)。
 */
ImU32 TileIdToColor(u16 id) noexcept {
    if (id == 0u) {
        // 空 tile: 暗灰半透明 (背景の checkerboard を意識させる)
        return IM_COL32(50, 50, 55, 120);
    }
    // 適当な splitmix32 風ハッシュ → R/G/B 8bit
    u32 h = static_cast<u32>(id) * 2654435761u;   // Knuth's multiplicative hash
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    // ID が大きく違っても色が極端に近くならないよう、各 channel を別 byte から
    // 取りつつ「最低輝度 64」を保証する (= 全黒近くを避ける、可読性のため)。
    const u8 r = static_cast<u8>(64u + ((h        & 0xFFu) * 192u / 255u));
    const u8 g = static_cast<u8>(64u + (((h >> 8) & 0xFFu) * 192u / 255u));
    const u8 b = static_cast<u8>(64u + (((h >> 16) & 0xFFu) * 192u / 255u));
    return IM_COL32(r, g, b, 255);
}

/**
 * world 座標を canvas 上の screen 座標 (ImVec2) に変換する。
 *
 * @details
 * 2D ortho 投影を pixels_per_world でスケールした素朴形。world Y は +Y 上、
 * screen Y は +Y 下なので符号を反転する。
 * @param wx world X 座標。
 * @param wy world Y 座標。
 * @param cam_x カメラ中心の world X。
 * @param cam_y カメラ中心の world Y。
 * @param pixels_per_world world 1 単位あたりの screen ピクセル数。
 * @param canvas_center canvas 中央の screen 座標。
 * @return 変換後の screen 座標。
 */
inline ImVec2 WorldToScreen(f32 wx, f32 wy,
                            f32 cam_x, f32 cam_y, f32 pixels_per_world,
                            ImVec2 canvas_center) noexcept {
    // world Y は +Y 上、screen Y は +Y 下 → 符号反転。
    return ImVec2(canvas_center.x + (wx - cam_x) * pixels_per_world,
                  canvas_center.y - (wy - cam_y) * pixels_per_world);
}

/**
 * screen 座標を world 座標に逆変換する。
 *
 * @details pixels_per_world が極小 (< 0.0001) のときはカメラ中心をそのまま返す。
 * @param sx screen X 座標。
 * @param sy screen Y 座標。
 * @param cam_x カメラ中心の world X。
 * @param cam_y カメラ中心の world Y。
 * @param pixels_per_world world 1 単位あたりの screen ピクセル数。
 * @param canvas_center canvas 中央の screen 座標。
 * @param out_wx 変換後の world X を受け取る出力。
 * @param out_wy 変換後の world Y を受け取る出力。
 */
inline void ScreenToWorld(f32 sx, f32 sy,
                          f32 cam_x, f32 cam_y, f32 pixels_per_world,
                          ImVec2 canvas_center,
                          f32& out_wx, f32& out_wy) noexcept {
    if (pixels_per_world < 0.0001f) {
        out_wx = cam_x;
        out_wy = cam_y;
        return;
    }
    out_wx = cam_x + (sx - canvas_center.x) / pixels_per_world;
    out_wy = cam_y - (sy - canvas_center.y) / pixels_per_world;
}

/**
 * 4-neighbor flood-fill で連結成分を塗り替える。
 *
 * @details
 * 開始点 (sx, sy) の tile id と同じ連結成分を new_id で塗り替える。
 * new_id == start_id なら no-op。再帰だと巨大マップで stack overflow するため
 * 明示スタック (TArray<u32>) を使い、max_cells を超えたら途中で打ち切る。
 * @param map 対象の FTilemap。
 * @param layer 塗り替えるレイヤ番号。
 * @param sx 開始 tile の X 座標。
 * @param sy 開始 tile の Y 座標。
 * @param new_id 塗り替え後の tile id。
 * @param max_cells 処理セル数の上限 (暴走防止の打ち切り値)。
 */
void FloodFill(class FTilemap& map, u32 layer,
               u32 sx, u32 sy, u16 new_id, u32 max_cells) noexcept {
    const u32 w = map.Width();
    const u32 h = map.Height();
    if (sx >= w || sy >= h) return;
    const u16 start_id = map.GetTile(sx, sy, layer).value;
    if (start_id == new_id) return;   // 同じ id なら何もしない

    // 反復スタック (TArray<u32>): 各要素は y * w + x の cell index。
    // 再帰だと巨大マップで stack overflow するため明示スタックを使う。
    TArray<u32> stack;
    stack.Reserve(64u);
    stack.PushBack(sy * w + sx);

    u32 processed = 0u;
    while (stack.Size() > 0u && processed < max_cells) {
        const u32 idx = stack[stack.Size() - 1u];
        stack.PopBack();

        const u32 cx = idx % w;
        const u32 cy = idx / w;

        // 既に塗ってあるかチェック (= スタックに同 cell が複数積まれた case)
        if (map.GetTile(cx, cy, layer).value != start_id) continue;

        map.SetTile(cx, cy, FTileId{new_id}, layer);
        ++processed;

        // 4-neighbor を push (範囲チェックは GetTile/SetTile 側でも弾かれるが、
        // スタックに無駄な要素を積まないようここで境界チェック)。
        if (cx + 1u < w && map.GetTile(cx + 1u, cy, layer).value == start_id)
            stack.PushBack(cy * w + (cx + 1u));
        if (cx > 0u       && map.GetTile(cx - 1u, cy, layer).value == start_id)
            stack.PushBack(cy * w + (cx - 1u));
        if (cy + 1u < h && map.GetTile(cx, cy + 1u, layer).value == start_id)
            stack.PushBack((cy + 1u) * w + cx);
        if (cy > 0u       && map.GetTile(cx, cy - 1u, layer).value == start_id)
            stack.PushBack((cy - 1u) * w + cx);
    }
}

} // anonymous namespace

/** 内部 state をデフォルトに初期化する (FEditorCamera を 2D mode で Init)。 */
void FLevelEditorPanel::Init() noexcept {
    // FEditorCamera を 2D mode で完全初期化 (= position=origin, zoom=1.0)。
    m_Camera.Init(acs::game::editor_core::EEditorCameraMode::Mode2D);
    // tilemap 1 セル = 16 world unit が典型 (FTilemap.h default) なので、
    // base ortho size を「32 セル分 = 512」相当にしておくと初期表示で 32x32
    // マップがちょうど画面に収まる。
    m_Camera.SetBaseOrthoSize(512.0f);

    m_Tilemap          = nullptr;
    m_Brush            = EBrushKind::Paint;
    m_CurrentTileId  = 1u;
    m_ActiveLayer     = 0u;
    m_ShowGrid        = true;
    m_SnapToGrid     = true;
    m_SelectedX       = kNoCoord;
    m_SelectedY       = kNoCoord;

    // 表示 / dock ヒントの初期値。viewport 系 panel は dock 中央に置きたい
    // ことが多いので m_DockedTarget = true (= Workspace 側へのヒント)。
    m_Visible       = true;
    m_DockedTarget = true;
}

/** 内部 state を全解放する (tilemap 参照解除 + FEditorCamera Reset)。 */
void FLevelEditorPanel::Shutdown() noexcept {
    // FEditorCamera は POD だが明示 Reset で確定状態にする。
    m_Camera.Reset();
    m_Tilemap     = nullptr;
    m_SelectedX  = kNoCoord;
    m_SelectedY  = kNoCoord;
}

/** 編集対象の FTilemap を raw 参照でセットし、active_layer と selection を整える。 */
void FLevelEditorPanel::SetTilemap(class FTilemap* tm) noexcept {
    m_Tilemap = tm;
    // active_layer を LayerCount() でクランプ (= 別 tilemap に切替えたら
    // 前 tilemap の layer 番号は無意味)。
    if (m_Tilemap != nullptr) {
        const u32 lc = m_Tilemap->LayerCount();
        if (lc == 0u) {
            m_ActiveLayer = 0u;
        } else if (m_ActiveLayer >= lc) {
            m_ActiveLayer = lc - 1u;
        }
    }
    // selection もリセット
    m_SelectedX = kNoCoord;
    m_SelectedY = kNoCoord;
}

/** 現在編集対象の FTilemap を返す (未バインド時は nullptr)。 */
class FTilemap* FLevelEditorPanel::CurrentTilemap() const noexcept {
    return m_Tilemap;
}

/** 内部 FEditorCamera (Mode2D) への参照を返す。 */
acs::game::editor_core::FEditorCamera& FLevelEditorPanel::Camera() noexcept {
    return m_Camera;
}

/** 現在のブラシ種別を返す。 */
EBrushKind FLevelEditorPanel::CurrentBrush() const noexcept {
    return m_Brush;
}

/** ブラシ種別を設定する。 */
void FLevelEditorPanel::SetCurrentBrush(EBrushKind b) noexcept {
    m_Brush = b;
}

/** 現在ペイント中の tile id を返す。 */
u16 FLevelEditorPanel::CurrentTileId() const noexcept {
    return m_CurrentTileId;
}

/** ペイントする tile id を設定する (kTileIdMax でクランプ)。 */
void FLevelEditorPanel::SetCurrentTileId(u16 id) noexcept {
    if (id > kTileIdMax) id = kTileIdMax;
    m_CurrentTileId = id;
}

/** アクティブレイヤ番号を返す。 */
u32 FLevelEditorPanel::ActiveLayer() const noexcept {
    return m_ActiveLayer;
}

/** アクティブレイヤ番号を設定する (tilemap バインド時は LayerCount でクランプ)。 */
void FLevelEditorPanel::SetActiveLayer(u32 layer) noexcept {
    // tilemap がある場合は LayerCount でクランプ、無い場合はそのまま受け入れる
    // (後で SetTilemap でクランプされる)。
    if (m_Tilemap != nullptr) {
        const u32 lc = m_Tilemap->LayerCount();
        if (lc == 0u) {
            m_ActiveLayer = 0u;
            return;
        }
        if (layer >= lc) layer = lc - 1u;
    }
    m_ActiveLayer = layer;
}

/** grid 表示フラグを返す。 */
bool FLevelEditorPanel::ShowGrid() const noexcept {
    return m_ShowGrid;
}

/** grid 表示フラグを設定する。 */
void FLevelEditorPanel::SetShowGrid(bool b) noexcept {
    m_ShowGrid = b;
}

/** snap-to-grid 表示フラグを返す。 */
bool FLevelEditorPanel::SnapToGrid() const noexcept {
    return m_SnapToGrid;
}

/** snap-to-grid 表示フラグを設定する。 */
void FLevelEditorPanel::SetSnapToGrid(bool b) noexcept {
    m_SnapToGrid = b;
}

/** Workspace 登録時の初期化フック (基底実装 + FEditorCamera を 2D mode で再初期化)。 */
void FLevelEditorPanel::OnInit(acs::game::editor_core::FEditorWorkspace& workspace) noexcept {
    FEditorPanel::OnInit(workspace);
    m_Camera.Init(acs::game::editor_core::EEditorCameraMode::Mode2D);
    m_Camera.SetBaseOrthoSize(512.0f);
}

/** Toolbar + viewport canvas + inspector を 1 window レイアウトで描画する。 */
void FLevelEditorPanel::DrawUI() noexcept {
    if (!IsVisible()) return;

    if (!ImGui::Begin(Title(), &m_Visible)) {
        ImGui::End();
        return;
    }

    if (m_Tilemap == nullptr) {
        ImGui::TextDisabled("(No tilemap bound — call SetTilemap(&tm) to start editing)");
        ImGui::End();
        return;
    }

    FTilemap& map = *m_Tilemap;
    const u32 map_w = map.Width();
    const u32 map_h = map.Height();
    const f32 tile_size = map.TileSize();
    const u32 layer_count = map.LayerCount();

    // Toolbar: Brush kind / Active layer / Tile id picker / Show grid / Snap
    {
        // Brush kind combo
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::BeginCombo("Brush", BrushLabel(m_Brush))) {
            for (u32 k = 0; k < 4u; ++k) {
                const EBrushKind kk = static_cast<EBrushKind>(k);
                const bool selected = (kk == m_Brush);
                if (ImGui::Selectable(BrushLabel(kk), selected)) {
                    m_Brush = kk;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();

        // Active layer combo
        ImGui::SetNextItemWidth(80.0f);
        char layer_label[32] = {};
        std::snprintf(layer_label, sizeof(layer_label), "Layer %u", m_ActiveLayer);
        if (ImGui::BeginCombo("Layer", layer_label)) {
            for (u32 L = 0; L < layer_count; ++L) {
                const bool selected = (L == m_ActiveLayer);
                char item_label[32] = {};
                std::snprintf(item_label, sizeof(item_label), "Layer %u", L);
                if (ImGui::Selectable(item_label, selected)) {
                    SetActiveLayer(L);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();

        // Tile id picker (DragInt 0〜kTileIdMax)
        ImGui::SetNextItemWidth(120.0f);
        // Erase ブラシ中は tile id 編集を disable (= 強制 0 扱いだから)
        const bool tile_id_disabled = (m_Brush == EBrushKind::Erase);
        if (tile_id_disabled) ImGui::BeginDisabled();
        int tid = static_cast<int>(m_CurrentTileId);
        if (ImGui::DragInt("Tile ID", &tid, 1.0f, 0, static_cast<int>(kTileIdMax))) {
            if (tid < 0) tid = 0;
            if (tid > static_cast<int>(kTileIdMax)) tid = static_cast<int>(kTileIdMax);
            m_CurrentTileId = static_cast<u16>(tid);
        }
        if (tile_id_disabled) ImGui::EndDisabled();

        // Show grid / Snap to grid toggle
        ImGui::Checkbox("Show Grid", &m_ShowGrid);
        ImGui::SameLine();
        ImGui::Checkbox("Snap to Grid", &m_SnapToGrid);
    }

    ImGui::Separator();

    // Layout: viewport canvas (左 70%) | inspector (右 30%)
    const f32 content_w = ImGui::GetContentRegionAvail().x;
    const f32 content_h = ImGui::GetContentRegionAvail().y;
    const f32 inspector_w = (content_w > 300.0f) ? 240.0f : (content_w * 0.30f);
    const f32 canvas_w = content_w - inspector_w - 8.0f;   // 8px gap
    const f32 canvas_h = (content_h > 200.0f) ? content_h : 200.0f;

    // FViewport canvas
    ImGui::BeginChild("##leveledit_viewport",
                      ImVec2(canvas_w, canvas_h),
                      true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove);
    {
        const ImVec2 canvas_origin = ImGui::GetCursorScreenPos();
        ImVec2 canvas_size = ImGui::GetContentRegionAvail();
        if (canvas_size.x < 50.0f) canvas_size.x = 50.0f;
        if (canvas_size.y < 50.0f) canvas_size.y = 50.0f;
        const ImVec2 canvas_end(canvas_origin.x + canvas_size.x,
                                canvas_origin.y + canvas_size.y);
        const ImVec2 canvas_center((canvas_origin.x + canvas_end.x) * 0.5f,
                                   (canvas_origin.y + canvas_end.y) * 0.5f);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // 背景 (= 暗グレー枠)
        dl->AddRectFilled(canvas_origin, canvas_end, IM_COL32(28, 28, 32, 255));
        dl->AddRect      (canvas_origin, canvas_end, IM_COL32(70, 70, 80, 255));

        // クリック / drag を取るための InvisibleButton (= 領域全体に置く)。
        // ImGuiButtonFlags_MouseButtonLeft で LMB を受ける。
        ImGui::InvisibleButton("##leveledit_canvas", canvas_size,
                               ImGuiButtonFlags_MouseButtonLeft
                             | ImGuiButtonFlags_MouseButtonMiddle
                             | ImGuiButtonFlags_MouseButtonRight);
        const bool canvas_hovered = ImGui::IsItemHovered();
        const bool canvas_active  = ImGui::IsItemActive();
        const ImGuiIO& io = ImGui::GetIO();
        const ImVec2 mouse = io.MousePos;

        // FEditorCamera 状態を取得 (2D position と zoom)。
        const acs::game::editor_core::FEditorCameraState& cs = m_Camera.State();
        const f32 cam_x = cs.position.x;
        const f32 cam_y = cs.position.y;
        const f32 zoom  = (cs.zoom_2d > 0.001f) ? cs.zoom_2d : 0.001f;

        // ortho 幅 (world units / 画面横) を pixels_per_world に変換。
        // base_ortho_size は zoom=1 時の world 幅 (default 512 = 32 タイル分)。
        const f32 ortho_w_world  = m_Camera.GetBaseOrthoSize() / zoom;
        const f32 pixels_per_world = (ortho_w_world > 0.001f)
                                   ? (canvas_size.x / ortho_w_world)
                                   : 1.0f;

        // FCamera への mouse input 流し込み (= 2D pan / zoom)。
        // canvas hover 中だけ吸い上げる (= ImGui の他 widget を奪わない)。
        if (canvas_hovered) {
            // wheel: zoom
            const f32 wheel = io.MouseWheel;
            // 中ドラッグ / 右ドラッグ: pan (LMB はペイント用に予約するため除外)
            const bool mmb = io.MouseDown[ImGuiMouseButton_Middle];
            const bool rmb = io.MouseDown[ImGuiMouseButton_Right];
            const ImVec2 mdelta = io.MouseDelta;
            m_Camera.HandleMouseInput(FVec2{mdelta.x, mdelta.y},
                                     /*lmb=*/false, rmb, mmb, wheel);
        }
        // smoothing tick (Workspace 側でも呼ばれる可能性があるが、二重 Tick も
        // 単に 0 dt 補間で害が無いので安全)。
        m_Camera.Tick(io.DeltaTime);

        // tile 矩形を 1 cell ずつ描画 (= ImDrawList::AddRectFilled の row-major
        // スイープ)。world 座標は左下を原点 (FTilemap.TileToWorld 慣習)、
        // tile (x, y) の中心が ((x+0.5)*tile_size, (y+0.5)*tile_size)。
        // 矩形描画は「中心 ± tile_size/2」で行う。
        // 全 layer を描画 (layer 0 から layer_count-1 まで重ね描き = 後ろ手前優先)、
        // 空 (id=0) tile は描画スキップ。
        const f32 half_ts = tile_size * 0.5f;
        for (u32 L = 0; L < layer_count; ++L) {
            for (u32 y = 0; y < map_h; ++y) {
                for (u32 x = 0; x < map_w; ++x) {
                    const u16 tid = map.GetTile(x, y, L).value;
                    if (tid == 0u && L > 0u) continue;   // 上層の空は skip
                    const FVec2 wp_center = map.TileToWorld(x, y);
                    const ImVec2 a = WorldToScreen(wp_center.x - half_ts,
                                                   wp_center.y + half_ts,
                                                   cam_x, cam_y,
                                                   pixels_per_world,
                                                   canvas_center);
                    const ImVec2 b = WorldToScreen(wp_center.x + half_ts,
                                                   wp_center.y - half_ts,
                                                   cam_x, cam_y,
                                                   pixels_per_world,
                                                   canvas_center);
                    // canvas 外なら描画 skip (= clip)
                    if (b.x < canvas_origin.x || a.x > canvas_end.x) continue;
                    if (b.y < canvas_origin.y || a.y > canvas_end.y) continue;
                    // 描画
                    dl->AddRectFilled(a, b, TileIdToColor(tid));
                }
            }
        }

        // grid line 描画 (show_grid=true の時)
        if (m_ShowGrid) {
            const ImU32 grid_col = IM_COL32(80, 80, 90, 180);
            // 縦線: x = 0, 1, ..., map_w (= tile_size 単位)
            for (u32 x = 0; x <= map_w; ++x) {
                const f32 wx = static_cast<f32>(x) * tile_size;
                const ImVec2 a = WorldToScreen(wx, 0.0f,
                                               cam_x, cam_y,
                                               pixels_per_world, canvas_center);
                const ImVec2 b = WorldToScreen(wx, static_cast<f32>(map_h) * tile_size,
                                               cam_x, cam_y,
                                               pixels_per_world, canvas_center);
                if (a.x < canvas_origin.x - 1.0f || a.x > canvas_end.x + 1.0f) continue;
                dl->AddLine(a, b, grid_col, 1.0f);
            }
            // 横線: y = 0, 1, ..., map_h
            for (u32 y = 0; y <= map_h; ++y) {
                const f32 wy = static_cast<f32>(y) * tile_size;
                const ImVec2 a = WorldToScreen(0.0f, wy,
                                               cam_x, cam_y,
                                               pixels_per_world, canvas_center);
                const ImVec2 b = WorldToScreen(static_cast<f32>(map_w) * tile_size, wy,
                                               cam_x, cam_y,
                                               pixels_per_world, canvas_center);
                if (a.y < canvas_origin.y - 1.0f || a.y > canvas_end.y + 1.0f) continue;
                dl->AddLine(a, b, grid_col, 1.0f);
            }
        }

        // マウス位置 → world → tile 変換 (canvas 上にホバー中のみ)。
        // hovered tile を強調表示 + クリック / drag でブラシ動作。
        if (canvas_hovered) {
            f32 wx = 0.0f, wy = 0.0f;
            ScreenToWorld(mouse.x, mouse.y,
                          cam_x, cam_y, pixels_per_world, canvas_center,
                          wx, wy);
            u32 tx = 0u, ty = 0u;
            const bool in_grid = map.WorldToTile(FVec2{wx, wy}, tx, ty);

            if (in_grid) {
                // hovered tile のハイライト (snap_to_grid=true なら太枠)
                const FVec2 hc = map.TileToWorld(tx, ty);
                const ImVec2 ha = WorldToScreen(hc.x - half_ts, hc.y + half_ts,
                                                cam_x, cam_y,
                                                pixels_per_world, canvas_center);
                const ImVec2 hb = WorldToScreen(hc.x + half_ts, hc.y - half_ts,
                                                cam_x, cam_y,
                                                pixels_per_world, canvas_center);
                const ImU32 hilite_col = IM_COL32(255, 220, 80, 200);
                dl->AddRect(ha, hb, hilite_col, 0.0f, 0, m_SnapToGrid ? 2.5f : 1.5f);

                // ブラシ動作: LMB に応じて 4 種を分岐
                const bool lmb_down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
                const bool lmb_click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);

                // canvas_active が true なら LMB drag 中
                switch (m_Brush) {
                case EBrushKind::Paint:
                    // drag 対応: lmb_down 中は毎フレーム SetTile (= 連続塗り)。
                    if (canvas_active && lmb_down) {
                        map.SetTile(tx, ty, FTileId{m_CurrentTileId}, m_ActiveLayer);
                        m_SelectedX = tx;
                        m_SelectedY = ty;
                    }
                    break;

                case EBrushKind::Erase:
                    // drag 対応: lmb_down 中は毎フレーム 0 を SetTile。
                    if (canvas_active && lmb_down) {
                        map.SetTile(tx, ty, FTileId{0u}, m_ActiveLayer);
                        m_SelectedX = tx;
                        m_SelectedY = ty;
                    }
                    break;

                case EBrushKind::Fill:
                    // クリック単発で flood-fill。drag は無効化 (誤動作防止)。
                    if (lmb_click) {
                        FloodFill(map, m_ActiveLayer, tx, ty,
                                  m_CurrentTileId, kFloodFillMaxCells);
                        m_SelectedX = tx;
                        m_SelectedY = ty;
                    }
                    break;

                case EBrushKind::Pick:
                    // クリック単発で tile id をコピー (= スポイト)。
                    if (lmb_click) {
                        const u16 picked = map.GetTile(tx, ty, m_ActiveLayer).value;
                        SetCurrentTileId(picked);
                        m_SelectedX = tx;
                        m_SelectedY = ty;
                    }
                    break;
                }
            }
        }
    }
    ImGui::EndChild();

    // Inspector (右側)
    ImGui::SameLine();
    ImGui::BeginChild("##leveledit_inspector",
                      ImVec2(inspector_w, canvas_h),
                      true,
                      ImGuiWindowFlags_None);
    {
        ImGui::TextUnformatted("Selected Tile");
        ImGui::Separator();
        if (m_SelectedX == kNoCoord || m_SelectedY == kNoCoord) {
            ImGui::TextDisabled("(none — hover & click in the viewport)");
        } else {
            ImGui::Text("x: %u", m_SelectedX);
            ImGui::Text("y: %u", m_SelectedY);
            ImGui::Text("layer: %u", m_ActiveLayer);
            const u16 here_id = (m_Tilemap != nullptr)
                              ? m_Tilemap->GetTile(m_SelectedX, m_SelectedY, m_ActiveLayer).value
                              : 0u;
            ImGui::Text("tile id (here): %u", static_cast<u32>(here_id));
        }
        ImGui::Spacing();
        ImGui::TextUnformatted("Current Brush");
        ImGui::Separator();
        ImGui::Text("brush: %s", BrushLabel(m_Brush));
        ImGui::Text("current tile id: %u", static_cast<u32>(m_CurrentTileId));

        ImGui::Spacing();
        ImGui::TextUnformatted("FCamera");
        ImGui::Separator();
        const acs::game::editor_core::FEditorCameraState& cs = m_Camera.State();
        ImGui::Text("pos: (%.1f, %.1f)",
                    static_cast<double>(cs.position.x),
                    static_cast<double>(cs.position.y));
        ImGui::Text("zoom: %.3f", static_cast<double>(cs.zoom_2d));
        if (ImGui::Button("Frame Map")) {
            // FTilemap 全体が画面に収まる camera 位置 / zoom に
            const f32 fw = static_cast<f32>(map_w) * tile_size;
            const f32 fh = static_cast<f32>(map_h) * tile_size;
            m_Camera.FrameToBoundingBox2D(FVec2{0.0f, 0.0f}, FVec2{fw, fh});
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset")) {
            m_Camera.Reset();
            m_Camera.SetBaseOrthoSize(512.0f);
        }

        ImGui::Spacing();
        ImGui::TextUnformatted("Map Info");
        ImGui::Separator();
        ImGui::Text("size: %u x %u", map_w, map_h);
        ImGui::Text("tile size: %.1f", static_cast<double>(tile_size));
        ImGui::Text("layers: %u", layer_count);
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace acs::game::leveledit
