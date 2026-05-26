// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — spriteatlas / SpriteAtlasEditorPanel 実装 (Phase 22 第一弾)
//
// 仕様の意図は SpriteAtlasEditorPanel.h を参照。本ファイルでは:
//   ・EditorPanel 基底 hook (OnInit / DrawUI) の override
//   ・SetSpritePack / CurrentPack / SelectFrame / AddFrame / DeleteSelectedFrame
//     の小粒な mutator/accessor
//   ・toolbar / left frame list / center atlas viewport (placeholder + 矩形
//     overlay) / right inspector の 4 領域 ImGui レイアウト
//   ・mouse drag による frame rect resize (8 handle: 4 corner + 4 edge)
// を実装する。すべて noexcept、STL 不使用、ImGui 依存はこの .cpp に閉じる。
#include "gameframework/tools/spriteatlas/SpriteAtlasEditorPanel.h"

#include "gameframework/SpritePack.h"
#include "gameframework/tools/editor_core/EditorWorkspace.h"

#include <imgui.h>

#include <cstdio>   // snprintf (Frame_NN 命名)

namespace acs::game::spriteatlas {

// ============================================================================
// ローカルヘルパ
// ============================================================================
namespace {

// f32 を [lo, hi] にクランプ (acs::math の Clamp に依存しない最小実装。
// editor 系 panel での pivot / zoom クランプに使う)。
inline f32 ClampF32(f32 v, f32 lo, f32 hi) noexcept {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// i32 を [lo, hi] にクランプ。SliderInt の戻り値補正に使う。
inline i32 ClampI32(i32 v, i32 lo, i32 hi) noexcept {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// EPivotPreset → (pivot_x, pivot_y) ペアに変換。Custom はゼロ扱いで返す
// (= caller 側で「変更しない」分岐を入れる前提)。
struct PivotPair { f32 x; f32 y; };
inline PivotPair PivotForPreset(EPivotPreset preset) noexcept {
    switch (preset) {
        case EPivotPreset::Center:  return { 0.5f, 0.5f };
        case EPivotPreset::TopLeft: return { 0.0f, 0.0f };
        case EPivotPreset::Custom:  return { 0.0f, 0.0f };  // unused
    }
    return { 0.5f, 0.5f };
}

// 8 hit handle 識別子。corner / edge resize に使う。
// 番号: 0=TL, 1=TR, 2=BR, 3=BL, 4=T, 5=R, 6=B, 7=L
enum class EFrameHandle : u8 {
    None        = 0xFF,
    TopLeft     = 0,
    TopRight    = 1,
    BottomRight = 2,
    BottomLeft  = 3,
    Top         = 4,
    Right       = 5,
    Bottom      = 6,
    Left        = 7,
};

// 1 handle の画面表示半径 (px) と当たり判定半径。
constexpr f32 kHandleVisualRadius = 4.0f;
constexpr f32 kHandleHitRadius    = 6.0f;

// atlas 内の (x, y, w, h) ピクセル矩形 (i32) → viewport ImGui 座標 (左上原点)
// に変換するヘルパ。`origin` は ChildWindow 左上 (= IsItemAdded 後の cursor
// 位置の screen pos)、`zoom` は表示倍率。
inline ImVec2 AtlasToScreen(ImVec2 origin, f32 zoom, i32 x, i32 y) noexcept {
    return ImVec2{ origin.x + static_cast<f32>(x) * zoom,
                   origin.y + static_cast<f32>(y) * zoom };
}

} // anonymous namespace

// ============================================================================
// Init / Shutdown
// ============================================================================
void SpriteAtlasEditorPanel::Init() noexcept {
    _pack          = nullptr;
    _selected      = -1;
    _zoom          = 1.0f;
    _pivot_preset  = EPivotPreset::Center;

    // 静的バッファは {} で 0 初期化済みだが、Init 多重呼び出しでも確定状態に
    // するため明示的に再ゼロ化 + 命名済個数を 0 リセット。
    for (u32 i = 0; i < kMaxOwnedFrames; ++i) {
        _default_frame_name_pool[i][0] = '\0';
    }
    _owned_name_count = 0;

    // viewport 系 panel は dock 中央に置きたいことが多い (ModelViewerPanel と
    // 同方針)。
    _visible       = true;
    _docked_target = true;
}

void SpriteAtlasEditorPanel::Shutdown() noexcept {
    _pack          = nullptr;
    _selected      = -1;
    _zoom          = 1.0f;
    _pivot_preset  = EPivotPreset::Center;
    for (u32 i = 0; i < kMaxOwnedFrames; ++i) {
        _default_frame_name_pool[i][0] = '\0';
    }
    _owned_name_count = 0;
}

// ============================================================================
// SetSpritePack / CurrentPack
// ============================================================================
void SpriteAtlasEditorPanel::SetSpritePack(acs::game::SpritePack* pack) noexcept {
    _pack = pack;
    // 新しい pack を渡されたら selection を 0 (= 最初の frame) にリセット
    // するのが UX 上自然 (Unity Sprite Editor も同様)。frame が無ければ -1。
    if (_pack != nullptr && _pack->FrameCount() > 0u) {
        _selected = 0;
    } else {
        _selected = -1;
    }
}

acs::game::SpritePack* SpriteAtlasEditorPanel::CurrentPack() const noexcept {
    return _pack;
}

// ============================================================================
// SelectedFrameIndex / SelectFrame
// ============================================================================
i32 SpriteAtlasEditorPanel::SelectedFrameIndex() const noexcept {
    return _selected;
}

void SpriteAtlasEditorPanel::SelectFrame(i32 idx) noexcept {
    if (_pack == nullptr) {
        _selected = -1;
        return;
    }
    const i32 count = static_cast<i32>(_pack->FrameCount());
    if (idx < 0 || idx >= count) {
        _selected = -1;
        return;
    }
    _selected = idx;
}

// ============================================================================
// AddFrame — default 64x64 で新規 frame、Frame_NN 名で SpritePack に登録
// ============================================================================
void SpriteAtlasEditorPanel::AddFrame() noexcept {
    if (_pack == nullptr) return;
    if (_owned_name_count >= kMaxOwnedFrames) return;  // name pool 上限

    // name pool に "Frame_NN" を書き込む。NN は _owned_name_count の現在値。
    // snprintf で確実に終端 '\0' を入れる。
    c8* name_buf = _default_frame_name_pool[_owned_name_count];
    std::snprintf(name_buf,
                  static_cast<usize>(kFrameNameMaxChars),
                  "Frame_%02u", static_cast<unsigned>(_owned_name_count));
    // 念のため終端保証 (snprintf 仕様上は不要だが、念のため明示)。
    name_buf[kFrameNameMaxChars - 1] = '\0';

    // pivot は現在の preset を反映 (Custom なら 0.5, 0.5 を fallback)。
    const PivotPair piv = (_pivot_preset == EPivotPreset::Custom)
                              ? PivotPair{ 0.5f, 0.5f }
                              : PivotForPreset(_pivot_preset);

    // default 64x64 (atlas 左上 0,0 起点)、pivot は preset。
    SpriteFrame f{};
    f.name    = name_buf;          // 静的バッファ寿命 = panel 寿命 ≧ SpritePack
    f.x       = 0u;
    f.y       = 0u;
    f.w       = kDefaultFrameW;
    f.h       = kDefaultFrameH;
    f.pivot_x = piv.x;
    f.pivot_y = piv.y;
    _pack->AddFrame(f);

    ++_owned_name_count;

    // 追加した frame を選択 (SpritePack は順序保存追加 = 末尾)。
    _selected = static_cast<i32>(_pack->FrameCount()) - 1;
}

// ============================================================================
// DeleteSelectedFrame
// ============================================================================
// SpritePack::RemoveFrame は name ベース (= 同名 frame は全削除) なので、
// 一意性のある _default_frame_name_pool 由来の name しか正しく扱えない。
// 「panel 外で AddFrame されたが、同名 frame」を保護するため、削除対象 name が
// pool 内アドレスか先頭ポインタ一致で確認してから削除する。一致しない場合は
// no-op (= 「panel が知らない frame は触らない」契約)。
// ============================================================================
void SpriteAtlasEditorPanel::DeleteSelectedFrame() noexcept {
    if (_pack == nullptr) return;
    const i32 count = static_cast<i32>(_pack->FrameCount());
    if (_selected < 0 || _selected >= count) return;

    u32 frame_count = 0;
    const SpriteFrame* frames = _pack->AllFrames(frame_count);
    if (frames == nullptr || _selected >= static_cast<i32>(frame_count)) return;

    const SpriteFrame& target = frames[static_cast<usize>(_selected)];
    const c8* target_name = target.name;
    if (target_name == nullptr) return;

    // pool 内アドレスかチェック (= panel が AddFrame した frame か確認)。
    // _default_frame_name_pool[i][0] のアドレス範囲に target_name が含まれるか。
    bool owned_by_panel = false;
    for (u32 i = 0; i < _owned_name_count; ++i) {
        if (target_name == _default_frame_name_pool[i]) {
            owned_by_panel = true;
            break;
        }
    }
    if (!owned_by_panel) {
        // panel 外で AddFrame された frame は触らない (= 削除に同名巻き添えが
        // 起きる可能性があるため安全側へ倒す)。
        return;
    }

    _pack->RemoveFrame(target_name);

    // 削除後の selection は前の index に詰める。空なら -1。
    const i32 new_count = static_cast<i32>(_pack->FrameCount());
    if (new_count == 0) {
        _selected = -1;
    } else if (_selected >= new_count) {
        _selected = new_count - 1;
    }
    // SpritePack は swap remove なので index 整合は完全には保証できないが、
    // panel 側からは「単一 selection が valid index に正規化される」だけ
    // 担保すれば UX 上問題ない。
}

// ============================================================================
// Zoom / PivotPreset
// ============================================================================
f32 SpriteAtlasEditorPanel::ZoomLevel() const noexcept {
    return _zoom;
}

void SpriteAtlasEditorPanel::SetZoomLevel(f32 z) noexcept {
    _zoom = ClampF32(z, kMinZoom, kMaxZoom);
}

void SpriteAtlasEditorPanel::SetPivotPreset(EPivotPreset p) noexcept {
    _pivot_preset = p;
    // Custom 以外を選んだら selected frame に pivot を即適用する (toolbar 上で
    // toggle した瞬間に「揃う」UX)。Custom は inspector の slider が手動で
    // pivot_x/pivot_y を編集する。
    if (p == EPivotPreset::Custom) return;
    if (_pack == nullptr) return;
    const i32 count = static_cast<i32>(_pack->FrameCount());
    if (_selected < 0 || _selected >= count) return;

    u32 frame_count = 0;
    const SpriteFrame* frames = _pack->AllFrames(frame_count);
    if (frames == nullptr || _selected >= static_cast<i32>(frame_count)) return;

    // SpritePack の AllFrames は const ポインタを返すので、mutable 編集には
    // 本来 SpritePack に SetFrame API を足したいところだが、現状は無いため
    // const_cast で書き換える (= 同一 SpritePack 内のデータ書換、所有関係は
    // 維持される。SpritePack の AllFrames コメント上「内部配列の先頭」と
    // 明記されており、frame の不変条件は名前以外 (= x/y/w/h/pivot) には無い)。
    SpriteFrame* mut_frames = const_cast<SpriteFrame*>(frames);
    const PivotPair piv = PivotForPreset(p);
    mut_frames[static_cast<usize>(_selected)].pivot_x = piv.x;
    mut_frames[static_cast<usize>(_selected)].pivot_y = piv.y;
}

// ============================================================================
// EditorPanel override: OnInit
// ============================================================================
void SpriteAtlasEditorPanel::OnInit(acs::game::editor_core::EditorWorkspace& workspace) noexcept {
    EditorPanel::OnInit(workspace);
    // name pool の終端 0 を全行で再確認 (= 多重 OnInit でも安全)。
    for (u32 i = 0; i < kMaxOwnedFrames; ++i) {
        _default_frame_name_pool[i][kFrameNameMaxChars - 1] = '\0';
    }
}

// ============================================================================
// EditorPanel override: DrawUI
// ============================================================================
// レイアウト (1 window "Sprite Atlas Editor"):
//   ┌──────────────────────────────────────────────────────────────────┐
//   │ [+ New] [- Delete] | Pivot: [Center][TopLeft][Custom] | Zoom: ◄►  │  <- toolbar
//   ├──────────────────────────────────────────────────────────────────┤
//   │ ┌─────────┐ ┌────────────────────────┐ ┌──────────────────────┐  │
//   │ │ Frames  │ │ Atlas FViewport         │ │ Inspector            │  │
//   │ │ Frame_00│ │  (grid + rect overlay  │ │  name: Frame_00      │  │
//   │ │ Frame_01│ │   + drag handle)       │ │  x: [SliderInt]      │  │
//   │ │ Frame_02│ │                        │ │  y: [SliderInt]      │  │
//   │ │ ...     │ │                        │ │  w: [SliderInt]      │  │
//   │ │         │ │                        │ │  h: [SliderInt]      │  │
//   │ │         │ │                        │ │  pivot: [SliderFloat]│  │
//   │ └─────────┘ └────────────────────────┘ └──────────────────────┘  │
//   └──────────────────────────────────────────────────────────────────┘
// ============================================================================
void SpriteAtlasEditorPanel::DrawUI() noexcept {
    if (!IsVisible()) return;

    if (!ImGui::Begin(Title(), &_visible)) {
        ImGui::End();
        return;
    }

    // ----- 上部 toolbar -----
    {
        const bool pack_ok = (_pack != nullptr);
        if (!pack_ok) ImGui::BeginDisabled();

        if (ImGui::Button("+ New Frame")) {
            AddFrame();
        }
        ImGui::SameLine();
        if (ImGui::Button("- Delete Selected")) {
            DeleteSelectedFrame();
        }

        if (!pack_ok) ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::TextUnformatted("|  Pivot:");
        ImGui::SameLine();

        // Pivot toggle: ラジオ風に 3 ボタン並べる。
        const EPivotPreset cur = _pivot_preset;
        if (ImGui::RadioButton("Center",  cur == EPivotPreset::Center)) {
            SetPivotPreset(EPivotPreset::Center);
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("TopLeft", cur == EPivotPreset::TopLeft)) {
            SetPivotPreset(EPivotPreset::TopLeft);
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Custom",  cur == EPivotPreset::Custom)) {
            SetPivotPreset(EPivotPreset::Custom);
        }

        ImGui::SameLine();
        ImGui::TextUnformatted("|  Zoom:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        f32 zoom_tmp = _zoom;
        if (ImGui::SliderFloat("##atlas_zoom", &zoom_tmp,
                               kMinZoom, kMaxZoom, "%.2fx")) {
            SetZoomLevel(zoom_tmp);
        }

        // pack 未注入時のガイダンス (toolbar 直下に小さく)。
        if (!pack_ok) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f),
                               "  (No SpritePack attached)");
        }
    }

    ImGui::Separator();

    // ----- 3 カラム (List / FViewport / Inspector) のサイズ計算 -----
    const f32 content_w = ImGui::GetContentRegionAvail().x;
    const f32 left_w    = (content_w > 600.0f) ? 160.0f : content_w * 0.20f;
    const f32 right_w   = (content_w > 600.0f) ? 220.0f : content_w * 0.30f;
    // viewport 幅 = 残り (gap を 2 x ItemSpacing 引いて確保)。
    const f32 gap_w     = ImGui::GetStyle().ItemSpacing.x * 2.0f;
    const f32 center_w  = content_w - left_w - right_w - gap_w;

    // ===== 左カラム: frame list =====
    ImGui::BeginChild("##atlas_left", ImVec2(left_w, 0.0f), true);
    {
        ImGui::TextUnformatted("Frames");
        ImGui::Separator();

        if (_pack == nullptr) {
            ImGui::TextDisabled("(no pack)");
        } else {
            u32 count = 0;
            const SpriteFrame* frames = _pack->AllFrames(count);
            if (count == 0) {
                ImGui::TextDisabled("(empty)");
            } else {
                for (u32 i = 0; i < count; ++i) {
                    const c8* nm = (frames[i].name != nullptr)
                                       ? frames[i].name
                                       : "(unnamed)";
                    const bool selected = (static_cast<i32>(i) == _selected);
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::Selectable(nm, selected)) {
                        _selected = static_cast<i32>(i);
                    }
                    ImGui::PopID();
                }
            }
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ===== 中央カラム: atlas viewport (placeholder + grid + rect overlay) =====
    ImGui::BeginChild("##atlas_viewport", ImVec2(center_w, 0.0f), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    {
        // atlas size を取得 (pack 未設定なら 256x256 を代表値として描画)。
        u32 aw = 256u;
        u32 ah = 256u;
        if (_pack != nullptr) {
            const SpritePackInfo& info = _pack->Info();
            if (info.atlas_width  > 0u) aw = info.atlas_width;
            if (info.atlas_height > 0u) ah = info.atlas_height;
        }
        const f32 view_w = static_cast<f32>(aw) * _zoom;
        const f32 view_h = static_cast<f32>(ah) * _zoom;

        // viewport の左上 screen pos を取得 (drawing baseline)。
        const ImVec2 origin = ImGui::GetCursorScreenPos();

        // InvisibleButton で hit area + 内容領域確保 (Dummy より drag を取りやすい)。
        ImGui::InvisibleButton("##atlas_canvas", ImVec2(view_w, view_h));

        ImDrawList* dl = ImGui::GetWindowDrawList();

        // ---- 背景塗り + grid 線 ----
        const ImU32 bg_col   = IM_COL32( 32,  32,  40, 255);
        const ImU32 grid_col = IM_COL32( 80,  80,  90, 255);
        dl->AddRectFilled(origin,
                          ImVec2(origin.x + view_w, origin.y + view_h),
                          bg_col);
        // 32px grid (atlas-space)。zoom 適用後の screen 距離が 8px 未満になる
        // 場合は描画を間引く (視認性 + 過密回避)。
        const f32 step_atlas = 32.0f;
        const f32 step_view  = step_atlas * _zoom;
        if (step_view >= 8.0f) {
            for (f32 gx = 0.0f; gx <= static_cast<f32>(aw) + 0.001f; gx += step_atlas) {
                const f32 sx = origin.x + gx * _zoom;
                dl->AddLine(ImVec2(sx, origin.y),
                            ImVec2(sx, origin.y + view_h),
                            grid_col, 1.0f);
            }
            for (f32 gy = 0.0f; gy <= static_cast<f32>(ah) + 0.001f; gy += step_atlas) {
                const f32 sy = origin.y + gy * _zoom;
                dl->AddLine(ImVec2(origin.x, sy),
                            ImVec2(origin.x + view_w, sy),
                            grid_col, 1.0f);
            }
        }

        // atlas 外枠を強調。
        dl->AddRect(origin,
                    ImVec2(origin.x + view_w, origin.y + view_h),
                    IM_COL32(180, 180, 180, 255), 0.0f, 0, 1.5f);

        // ---- 全 frame の矩形 overlay ----
        if (_pack != nullptr) {
            u32 count = 0;
            const SpriteFrame* frames = _pack->AllFrames(count);
            for (u32 i = 0; i < count; ++i) {
                const SpriteFrame& f = frames[i];
                const ImVec2 r_min = AtlasToScreen(origin, _zoom,
                                                   static_cast<i32>(f.x),
                                                   static_cast<i32>(f.y));
                const ImVec2 r_max = AtlasToScreen(origin, _zoom,
                                                   static_cast<i32>(f.x + f.w),
                                                   static_cast<i32>(f.y + f.h));
                const bool is_sel = (static_cast<i32>(i) == _selected);
                const ImU32 col   = is_sel
                                        ? IM_COL32(255, 220,  80, 255)
                                        : IM_COL32(120, 200, 255, 200);
                const f32   thick = is_sel ? 2.0f : 1.0f;
                dl->AddRect(r_min, r_max, col, 0.0f, 0, thick);

                // selected frame は半透明塗り + 8 個の resize handle を描画。
                if (is_sel) {
                    dl->AddRectFilled(r_min, r_max,
                                      IM_COL32(255, 220, 80, 32));

                    // 8 handle 位置を計算 (atlas-space integer は viewport 上の f32 に変換済)。
                    const f32 mx = (r_min.x + r_max.x) * 0.5f;
                    const f32 my = (r_min.y + r_max.y) * 0.5f;
                    const ImVec2 handles[8] = {
                        { r_min.x, r_min.y }, // TL
                        { r_max.x, r_min.y }, // TR
                        { r_max.x, r_max.y }, // BR
                        { r_min.x, r_max.y }, // BL
                        { mx,      r_min.y }, // T
                        { r_max.x, my      }, // R
                        { mx,      r_max.y }, // B
                        { r_min.x, my      }, // L
                    };
                    for (u32 hi = 0; hi < 8; ++hi) {
                        dl->AddRectFilled(
                            ImVec2(handles[hi].x - kHandleVisualRadius,
                                   handles[hi].y - kHandleVisualRadius),
                            ImVec2(handles[hi].x + kHandleVisualRadius,
                                   handles[hi].y + kHandleVisualRadius),
                            IM_COL32(255, 255, 255, 255));
                    }

                    // ---- handle drag による rect 編集 ----
                    // ImGui の canvas は InvisibleButton 1 個なので、handle ごとに
                    // 個別の hit-test は GetMousePos vs handles[k] で行う。
                    // canvas が active (= マウス押下中で本 button が捕まえている) の
                    // ときだけ drag 処理を発火する。
                    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
                        // drag 開始時の click 位置から、どの handle が "掴まれた"
                        // かを決定する (= 後続のフレームで判定がブレないため、
                        // 当該フレームでの mouse pos ではなく click pos を使う)。
                        const ImVec2 mclicked = ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Left];
                        EFrameHandle picked = EFrameHandle::None;
                        f32 best_dist2 = kHandleHitRadius * kHandleHitRadius;
                        for (u32 hi = 0; hi < 8; ++hi) {
                            const f32 dx = mclicked.x - handles[hi].x;
                            const f32 dy = mclicked.y - handles[hi].y;
                            const f32 d2 = dx*dx + dy*dy;
                            if (d2 <= best_dist2) {
                                best_dist2 = d2;
                                picked = static_cast<EFrameHandle>(hi);
                            }
                        }
                        if (picked != EFrameHandle::None) {
                            // delta は zoom 換算で atlas-pixel に戻す。
                            const f32 inv_zoom = (_zoom > 0.0f) ? (1.0f / _zoom) : 1.0f;

                            // SpritePack は AllFrames が const なので、編集には
                            // const_cast を使う (SpritePack::AllFrames コメントで
                            // 「内部配列の先頭」と明記、x/y/w/h は不変条件外)。
                            SpriteFrame* mut_frames =
                                const_cast<SpriteFrame*>(frames);
                            SpriteFrame& tgt = mut_frames[static_cast<usize>(_selected)];

                            // 1 フレーム分の mouse delta を atlas px に丸めて
                            // 適用 (= cumulative delta をその場で反映、drag end で
                            // SpritePack の現在値が確定)。
                            // 厳密な undo 一貫性 (drag 開始時 baseline → 累積 delta)
                            // が必要なら Phase 22+ で「drag 開始時 snapshot」を
                            // パネル内に持つ拡張で対応する。
                            i32 nx = static_cast<i32>(tgt.x);
                            i32 ny = static_cast<i32>(tgt.y);
                            i32 nw = static_cast<i32>(tgt.w);
                            i32 nh = static_cast<i32>(tgt.h);
                            const i32 ddx = static_cast<i32>(
                                ImGui::GetIO().MouseDelta.x * inv_zoom);
                            const i32 ddy = static_cast<i32>(
                                ImGui::GetIO().MouseDelta.y * inv_zoom);
                            switch (picked) {
                                case EFrameHandle::TopLeft:
                                    nx += ddx; ny += ddy; nw -= ddx; nh -= ddy; break;
                                case EFrameHandle::TopRight:
                                    ny += ddy; nw += ddx; nh -= ddy;            break;
                                case EFrameHandle::BottomRight:
                                    nw += ddx; nh += ddy;                       break;
                                case EFrameHandle::BottomLeft:
                                    nx += ddx; nw -= ddx; nh += ddy;            break;
                                case EFrameHandle::Top:
                                    ny += ddy; nh -= ddy;                       break;
                                case EFrameHandle::Right:
                                    nw += ddx;                                  break;
                                case EFrameHandle::Bottom:
                                    nh += ddy;                                  break;
                                case EFrameHandle::Left:
                                    nx += ddx; nw -= ddx;                       break;
                                default: break;
                            }
                            // クランプ: w/h は最小 1、座標は atlas 内に収める。
                            if (nw < 1) nw = 1;
                            if (nh < 1) nh = 1;
                            const i32 aw_i = static_cast<i32>(aw);
                            const i32 ah_i = static_cast<i32>(ah);
                            nx = ClampI32(nx, 0, aw_i - nw);
                            ny = ClampI32(ny, 0, ah_i - nh);
                            nw = ClampI32(nw, 1, aw_i - nx);
                            nh = ClampI32(nh, 1, ah_i - ny);
                            tgt.x = static_cast<u32>(nx);
                            tgt.y = static_cast<u32>(ny);
                            tgt.w = static_cast<u32>(nw);
                            tgt.h = static_cast<u32>(nh);
                        }
                    }
                }
            }
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ===== 右カラム: inspector =====
    ImGui::BeginChild("##atlas_inspector", ImVec2(right_w, 0.0f), true);
    {
        ImGui::TextUnformatted("Inspector");
        ImGui::Separator();

        if (_pack == nullptr) {
            ImGui::TextDisabled("(no pack)");
        } else {
            u32 count = 0;
            const SpriteFrame* frames = _pack->AllFrames(count);
            if (_selected < 0 || _selected >= static_cast<i32>(count) || frames == nullptr) {
                ImGui::TextDisabled("(no selection)");
            } else {
                // const → mutable へ (Pivot 編集と同じ理由、§SetPivotPreset 参照)。
                SpriteFrame* mut_frames = const_cast<SpriteFrame*>(frames);
                SpriteFrame& f = mut_frames[static_cast<usize>(_selected)];

                ImGui::Text("name: %s", (f.name != nullptr) ? f.name : "(null)");
                ImGui::Separator();

                // atlas size を取得 (slider 上限用)。
                u32 aw = 256u;
                u32 ah = 256u;
                const SpritePackInfo& info = _pack->Info();
                if (info.atlas_width  > 0u) aw = info.atlas_width;
                if (info.atlas_height > 0u) ah = info.atlas_height;

                const i32 aw_i = static_cast<i32>(aw);
                const i32 ah_i = static_cast<i32>(ah);

                // 各 slider: 個別 ID で値変更 → クランプ → 反映。
                i32 xx = static_cast<i32>(f.x);
                i32 yy = static_cast<i32>(f.y);
                i32 ww = static_cast<i32>(f.w);
                i32 hh = static_cast<i32>(f.h);

                if (ImGui::SliderInt("x", &xx, 0, aw_i - 1)) {
                    f.x = static_cast<u32>(ClampI32(xx, 0, aw_i - 1));
                }
                if (ImGui::SliderInt("y", &yy, 0, ah_i - 1)) {
                    f.y = static_cast<u32>(ClampI32(yy, 0, ah_i - 1));
                }
                if (ImGui::SliderInt("w", &ww, 1, aw_i)) {
                    // x + w <= aw を維持。
                    const i32 max_w = aw_i - static_cast<i32>(f.x);
                    f.w = static_cast<u32>(ClampI32(ww, 1, (max_w < 1) ? 1 : max_w));
                }
                if (ImGui::SliderInt("h", &hh, 1, ah_i)) {
                    const i32 max_h = ah_i - static_cast<i32>(f.y);
                    f.h = static_cast<u32>(ClampI32(hh, 1, (max_h < 1) ? 1 : max_h));
                }

                ImGui::Separator();

                f32 px = f.pivot_x;
                f32 py = f.pivot_y;
                if (ImGui::SliderFloat("pivot_x", &px, 0.0f, 1.0f, "%.3f")) {
                    f.pivot_x = ClampF32(px, 0.0f, 1.0f);
                    // 手で pivot を弄ったら preset を Custom に追従。
                    _pivot_preset = EPivotPreset::Custom;
                }
                if (ImGui::SliderFloat("pivot_y", &py, 0.0f, 1.0f, "%.3f")) {
                    f.pivot_y = ClampF32(py, 0.0f, 1.0f);
                    _pivot_preset = EPivotPreset::Custom;
                }

                ImGui::Separator();
                ImGui::TextDisabled("UV: (%.3f, %.3f) - (%.3f, %.3f)",
                                    static_cast<f32>(f.x) / static_cast<f32>(aw),
                                    static_cast<f32>(f.y) / static_cast<f32>(ah),
                                    static_cast<f32>(f.x + f.w) / static_cast<f32>(aw),
                                    static_cast<f32>(f.y + f.h) / static_cast<f32>(ah));
            }
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace acs::game::spriteatlas
