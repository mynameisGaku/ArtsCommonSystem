// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — spriteatlas / ASpriteAtlasEditorPanel 実装
//
// 仕様の意図は ASpriteAtlasEditorPanel.h を参照。本ファイルでは:
//   ・AEditorPanel 基底 hook (OnInit / DrawUI) の override
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

namespace {

/**
 * f32 を [lo, hi] にクランプする (acs::math の Clamp に依存しない最小実装)。
 *
 * @param v クランプ対象の値。
 * @param lo 下限。
 * @param hi 上限。
 * @return [lo, hi] に収めた値。
 */
inline f32 ClampF32(f32 v, f32 lo, f32 hi) noexcept {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/**
 * i32 を [lo, hi] にクランプする (SliderInt の戻り値補正に使う)。
 *
 * @param v クランプ対象の値。
 * @param lo 下限。
 * @param hi 上限。
 * @return [lo, hi] に収めた値。
 */
inline i32 ClampI32(i32 v, i32 lo, i32 hi) noexcept {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/** pivot の (x, y) 値ペア。 */
struct FPivotPair {
    /** pivot の x 成分 (0..1)。 */
    f32 x;

    /** pivot の y 成分 (0..1)。 */
    f32 y;
};

/**
 * EPivotPreset を (pivot_x, pivot_y) ペアに変換する。
 *
 * @details Custom はゼロ扱いで返す (caller 側で「変更しない」分岐を入れる前提)。
 * @param preset 変換するプリセット。
 * @return preset に対応する pivot ペア。
 */
inline FPivotPair PivotForPreset(EPivotPreset preset) noexcept {
    switch (preset) {
        case EPivotPreset::Center:  return { 0.5f, 0.5f };
        case EPivotPreset::TopLeft: return { 0.0f, 0.0f };
        case EPivotPreset::Custom:  return { 0.0f, 0.0f };  // unused
    }
    return { 0.5f, 0.5f };
}

/**
 * 矩形 resize 用の 8 個の handle 識別子 (4 corner + 4 edge)。
 *
 * @details 番号は 0=TL, 1=TR, 2=BR, 3=BL, 4=T, 5=R, 6=B, 7=L。None は handle 未選択を表す。
 */
enum class EFrameHandle : u8 {
    /** どの handle も掴んでいない状態。 */
    None        = 0xFF,

    /** 左上 corner。 */
    TopLeft     = 0,

    /** 右上 corner。 */
    TopRight    = 1,

    /** 右下 corner。 */
    BottomRight = 2,

    /** 左下 corner。 */
    BottomLeft  = 3,

    /** 上辺 edge。 */
    Top         = 4,

    /** 右辺 edge。 */
    Right       = 5,

    /** 下辺 edge。 */
    Bottom      = 6,

    /** 左辺 edge。 */
    Left        = 7,
};

/** 1 handle の画面表示半径 (px)。 */
constexpr f32 kHandleVisualRadius = 4.0f;

/** 1 handle の当たり判定半径 (px)。 */
constexpr f32 kHandleHitRadius    = 6.0f;

/**
 * atlas 座標 (x, y) を viewport の ImGui 画面座標 (左上原点) に変換する。
 *
 * @param origin ChildWindow 左上の screen pos (描画 baseline)。
 * @param zoom 表示倍率。
 * @param x atlas 内の x 座標 (px)。
 * @param y atlas 内の y 座標 (px)。
 * @return zoom 適用後の画面座標。
 */
inline ImVec2 AtlasToScreen(ImVec2 origin, f32 zoom, i32 x, i32 y) noexcept {
    return ImVec2{ origin.x + static_cast<f32>(x) * zoom,
                   origin.y + static_cast<f32>(y) * zoom };
}

} // anonymous namespace

/** 内部 state を初期値へ完全リセットする (詳細はヘッダ宣言を参照)。 */
void ASpriteAtlasEditorPanel::Init() noexcept {
    m_Pack          = nullptr;
    m_Selected      = -1;
    m_Zoom          = 1.0f;
    m_PivotPreset  = EPivotPreset::Center;

    // 静的バッファは {} で 0 初期化済みだが、Init 多重呼び出しでも確定状態に
    // するため明示的に再ゼロ化 + 命名済個数を 0 リセット。
    for (u32 i = 0; i < kMaxOwnedFrames; ++i) {
        m_DefaultFrameNamePool[i][0] = '\0';
    }
    m_OwnedNameCount = 0;

    // viewport 系 panel は dock 中央に置きたいことが多い (AModelViewerPanel と
    // 同方針)。
    m_Visible       = true;
    m_DockedTarget = true;
}

/** 内部 state を解放する (frame name pool / selection / FSpritePack* をクリア)。 */
void ASpriteAtlasEditorPanel::Shutdown() noexcept {
    m_Pack          = nullptr;
    m_Selected      = -1;
    m_Zoom          = 1.0f;
    m_PivotPreset  = EPivotPreset::Center;
    for (u32 i = 0; i < kMaxOwnedFrames; ++i) {
        m_DefaultFrameNamePool[i][0] = '\0';
    }
    m_OwnedNameCount = 0;
}

/** 編集対象 FSpritePack を注入し、selection を先頭 frame (無ければ -1) にリセットする。 */
void ASpriteAtlasEditorPanel::SetSpritePack(acs::game::FSpritePack* pack) noexcept {
    m_Pack = pack;
    // 新しい pack を渡されたら selection を 0 (= 最初の frame) にリセット
    // するのが UX 上自然 (Unity Sprite Editor も同様)。frame が無ければ -1。
    if (m_Pack != nullptr && m_Pack->FrameCount() > 0u) {
        m_Selected = 0;
    } else {
        m_Selected = -1;
    }
}

/** 現在注入されている FSpritePack を返す (未注入なら nullptr)。 */
acs::game::FSpritePack* ASpriteAtlasEditorPanel::CurrentPack() const noexcept {
    return m_Pack;
}

/** 現在選択中の frame index を返す (未選択なら -1)。 */
i32 ASpriteAtlasEditorPanel::SelectedFrameIndex() const noexcept {
    return m_Selected;
}

/** frame を選択する (範囲外 / pack 未注入は -1 に正規化)。 */
void ASpriteAtlasEditorPanel::SelectFrame(i32 idx) noexcept {
    if (m_Pack == nullptr) {
        m_Selected = -1;
        return;
    }
    const i32 count = static_cast<i32>(m_Pack->FrameCount());
    if (idx < 0 || idx >= count) {
        m_Selected = -1;
        return;
    }
    m_Selected = idx;
}

/** default 64x64 の新規 frame を Frame_NN 名で FSpritePack に追加し、選択を移す。 */
void ASpriteAtlasEditorPanel::AddFrame() noexcept {
    if (m_Pack == nullptr) return;
    if (m_OwnedNameCount >= kMaxOwnedFrames) return;  // name pool 上限

    // name pool に "Frame_NN" を書き込む。NN は m_OwnedNameCount の現在値。
    // snprintf で確実に終端 '\0' を入れる。
    c8* name_buf = m_DefaultFrameNamePool[m_OwnedNameCount];
    std::snprintf(name_buf,
                  static_cast<usize>(kFrameNameMaxChars),
                  "Frame_%02u", static_cast<unsigned>(m_OwnedNameCount));
    // 念のため終端保証 (snprintf 仕様上は不要だが、念のため明示)。
    name_buf[kFrameNameMaxChars - 1] = '\0';

    // pivot は現在の preset を反映 (Custom なら 0.5, 0.5 を fallback)。
    const FPivotPair piv = (m_PivotPreset == EPivotPreset::Custom)
                              ? FPivotPair{ 0.5f, 0.5f }
                              : PivotForPreset(m_PivotPreset);

    // default 64x64 (atlas 左上 0,0 起点)、pivot は preset。
    FSpriteFrame f{};
    f.name    = name_buf;          // 静的バッファ寿命 = panel 寿命 ≧ FSpritePack
    f.x       = 0u;
    f.y       = 0u;
    f.w       = kDefaultFrameW;
    f.h       = kDefaultFrameH;
    f.pivot_x = piv.x;
    f.pivot_y = piv.y;
    m_Pack->AddFrame(f);

    ++m_OwnedNameCount;

    // 追加した frame を選択 (FSpritePack は順序保存追加 = 末尾)。
    m_Selected = static_cast<i32>(m_Pack->FrameCount()) - 1;
}

/**
 * 選択中 frame を FSpritePack から削除し、name pool slot を回収する。
 *
 * @details
 * FSpritePack::RemoveFrame は name ベース (同名 frame は全削除) なので、一意性のある
 * m_DefaultFrameNamePool 由来の name しか正しく扱えない。削除対象 name が pool 内アドレスと
 * 先頭ポインタ一致するか確認してから削除し、一致しない場合は no-op とする (「panel が知らない
 * frame は触らない」契約)。削除後は末尾 slot を空いた slot へ swap-fill して回収する。
 */
void ASpriteAtlasEditorPanel::DeleteSelectedFrame() noexcept {
    if (m_Pack == nullptr) return;
    const i32 count = static_cast<i32>(m_Pack->FrameCount());
    if (m_Selected < 0 || m_Selected >= count) return;

    u32 frame_count = 0;
    const FSpriteFrame* frames = m_Pack->AllFrames(frame_count);
    if (frames == nullptr || m_Selected >= static_cast<i32>(frame_count)) return;

    const FSpriteFrame& target = frames[static_cast<usize>(m_Selected)];
    const c8* target_name = target.name;
    if (target_name == nullptr) return;

    // pool 内アドレスかチェック (= panel が AddFrame した frame か確認)。
    // m_DefaultFrameNamePool[i][0] のアドレス範囲に target_name が含まれるか。
    // 一致した slot index も控える (= 削除後に name pool を回収するため)。
    bool owned_by_panel = false;
    u32  freed_slot     = 0;
    for (u32 i = 0; i < m_OwnedNameCount; ++i) {
        if (target_name == m_DefaultFrameNamePool[i]) {
            owned_by_panel = true;
            freed_slot     = i;
            break;
        }
    }
    if (!owned_by_panel) {
        // panel 外で AddFrame された frame は触らない (= 削除に同名巻き添えが
        // 起きる可能性があるため安全側へ倒す)。
        return;
    }

    m_Pack->RemoveFrame(target_name);

    // name pool slot の回収 (= 単調増加カウンタによる枯渇を防ぐ)。
    // 削除した slot を末尾 slot で swap-fill して live 区間 [0, m_OwnedNameCount)
    // を縮める。これにより Add/Delete を繰り返しても上限で枯渇しない。
    // pool slot は FSpritePack frame から raw const char* で参照されるため、
    // 末尾 slot を移動したら、その slot を name に持つ frame を repoint する
    // (pointer 一致でちょうど 1 個。同名 strcmp 衝突は pool 外なので無関係)。
    if (m_OwnedNameCount > 0) {
        const u32 last_slot = m_OwnedNameCount - 1;
        if (freed_slot != last_slot) {
            // 末尾 slot を name に持つ frame を pack 内から探して repoint する。
            // RemoveFrame 後の現配列を取り直す (swap remove で再配置済)。
            u32 cur_count = 0;
            const FSpriteFrame* cur = m_Pack->AllFrames(cur_count);
            if (cur != nullptr) {
                FSpriteFrame* mut_cur = const_cast<FSpriteFrame*>(cur);
                const c8* last_addr = m_DefaultFrameNamePool[last_slot];
                for (u32 i = 0; i < cur_count; ++i) {
                    if (mut_cur[i].name == last_addr) {
                        mut_cur[i].name = m_DefaultFrameNamePool[freed_slot];
                        break;  // pool pointer は一意なので 1 個で打ち切り
                    }
                }
            }
            // 末尾 slot の文字列を freed_slot にコピー (固定長 + 終端保証)。
            for (u32 c = 0; c < kFrameNameMaxChars; ++c) {
                m_DefaultFrameNamePool[freed_slot][c] =
                    m_DefaultFrameNamePool[last_slot][c];
            }
            m_DefaultFrameNamePool[freed_slot][kFrameNameMaxChars - 1] = '\0';
        }
        // 回収した末尾 slot をクリアして live 個数を 1 減らす。
        m_DefaultFrameNamePool[last_slot][0] = '\0';
        --m_OwnedNameCount;
    }

    // 削除後の selection は前の index に詰める。空なら -1。
    const i32 new_count = static_cast<i32>(m_Pack->FrameCount());
    if (new_count == 0) {
        m_Selected = -1;
    } else if (m_Selected >= new_count) {
        m_Selected = new_count - 1;
    }
    // FSpritePack は swap remove なので index 整合は完全には保証できないが、
    // panel 側からは「単一 selection が valid index に正規化される」だけ
    // 担保すれば UX 上問題ない。
}

/** atlas placeholder の表示倍率を返す。 */
f32 ASpriteAtlasEditorPanel::ZoomLevel() const noexcept {
    return m_Zoom;
}

/** atlas placeholder の表示倍率を [kMinZoom, kMaxZoom] にクランプして設定する。 */
void ASpriteAtlasEditorPanel::SetZoomLevel(f32 z) noexcept {
    m_Zoom = ClampF32(z, kMinZoom, kMaxZoom);
}

/** Pivot preset を設定し、Custom 以外なら selected frame に pivot を即適用する。 */
void ASpriteAtlasEditorPanel::SetPivotPreset(EPivotPreset p) noexcept {
    m_PivotPreset = p;
    // Custom 以外を選んだら selected frame に pivot を即適用する (toolbar 上で
    // toggle した瞬間に「揃う」UX)。Custom は inspector の slider が手動で
    // pivot_x/pivot_y を編集する。
    if (p == EPivotPreset::Custom) return;
    if (m_Pack == nullptr) return;
    const i32 count = static_cast<i32>(m_Pack->FrameCount());
    if (m_Selected < 0 || m_Selected >= count) return;

    u32 frame_count = 0;
    const FSpriteFrame* frames = m_Pack->AllFrames(frame_count);
    if (frames == nullptr || m_Selected >= static_cast<i32>(frame_count)) return;

    // FSpritePack の AllFrames は const ポインタを返すので、mutable 編集には
    // 本来 FSpritePack に SetFrame API を足したいところだが、現状は無いため
    // const_cast で書き換える (= 同一 FSpritePack 内のデータ書換、所有関係は
    // 維持される。FSpritePack の AllFrames コメント上「内部配列の先頭」と
    // 明記されており、frame の不変条件は名前以外 (= x/y/w/h/pivot) には無い)。
    FSpriteFrame* mut_frames = const_cast<FSpriteFrame*>(frames);
    const FPivotPair piv = PivotForPreset(p);
    mut_frames[static_cast<usize>(m_Selected)].pivot_x = piv.x;
    mut_frames[static_cast<usize>(m_Selected)].pivot_y = piv.y;
}

/** Workspace 登録時に基底初期化を呼び、frame name pool 各行の終端 0 を確認する。 */
void ASpriteAtlasEditorPanel::OnInit(acs::game::editor_core::CEditorWorkspace& workspace) noexcept {
    AEditorPanel::OnInit(workspace);
    // name pool の終端 0 を全行で再確認 (= 多重 OnInit でも安全)。
    for (u32 i = 0; i < kMaxOwnedFrames; ++i) {
        m_DefaultFrameNamePool[i][kFrameNameMaxChars - 1] = '\0';
    }
}

/**
 * 1 window "Sprite Atlas Editor" を描画する。
 *
 * @details
 * 上部に toolbar (+ New / - Delete / Pivot toggle / Zoom slider) を置き、その下を
 * 3 カラムに分けて左 frame list、中央 atlas viewport (grid + 矩形 overlay + 選択 frame の
 * 8 個 drag handle)、右 inspector (name / x / y / w / h SliderInt + pivot SliderFloat) を
 * 描く。IsVisible() が false なら早期 return する。
 */
void ASpriteAtlasEditorPanel::DrawUI() noexcept {
    if (!IsVisible()) return;

    if (!ImGui::Begin(Title(), &m_Visible)) {
        ImGui::End();
        return;
    }

    // 上部 toolbar。
    {
        const bool pack_ok = (m_Pack != nullptr);
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
        const EPivotPreset cur = m_PivotPreset;
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
        f32 zoom_tmp = m_Zoom;
        if (ImGui::SliderFloat("##atlas_zoom", &zoom_tmp,
                               kMinZoom, kMaxZoom, "%.2fx")) {
            SetZoomLevel(zoom_tmp);
        }

        // pack 未注入時のガイダンス (toolbar 直下に小さく)。
        if (!pack_ok) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f),
                               "  (No sprite pack attached)");
        }
    }

    ImGui::Separator();

    // 3 カラム (List / Viewport / Inspector) のサイズ計算。
    const f32 content_w = ImGui::GetContentRegionAvail().x;
    const f32 left_w    = (content_w > 600.0f) ? 160.0f : content_w * 0.20f;
    const f32 right_w   = (content_w > 600.0f) ? 220.0f : content_w * 0.30f;
    // viewport 幅 = 残り (gap を 2 x ItemSpacing 引いて確保)。
    const f32 gap_w     = ImGui::GetStyle().ItemSpacing.x * 2.0f;
    const f32 center_w  = content_w - left_w - right_w - gap_w;

    // 左カラム: frame list。
    ImGui::BeginChild("##atlas_left", ImVec2(left_w, 0.0f), true);
    {
        ImGui::TextUnformatted("Frames");
        ImGui::Separator();

        if (m_Pack == nullptr) {
            ImGui::TextDisabled("(no pack)");
        } else {
            u32 count = 0;
            const FSpriteFrame* frames = m_Pack->AllFrames(count);
            if (count == 0) {
                ImGui::TextDisabled("(empty)");
            } else {
                for (u32 i = 0; i < count; ++i) {
                    const c8* nm = (frames[i].name != nullptr)
                                       ? frames[i].name
                                       : "(unnamed)";
                    const bool selected = (static_cast<i32>(i) == m_Selected);
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::Selectable(nm, selected)) {
                        m_Selected = static_cast<i32>(i);
                    }
                    ImGui::PopID();
                }
            }
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // 中央カラム: atlas viewport (placeholder + grid + rect overlay)。
    ImGui::BeginChild("##atlas_viewport", ImVec2(center_w, 0.0f), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    {
        // atlas size を取得 (pack 未設定なら 256x256 を代表値として描画)。
        u32 aw = 256u;
        u32 ah = 256u;
        if (m_Pack != nullptr) {
            const FSpritePackInfo& info = m_Pack->Info();
            if (info.atlas_width  > 0u) aw = info.atlas_width;
            if (info.atlas_height > 0u) ah = info.atlas_height;
        }
        const f32 view_w = static_cast<f32>(aw) * m_Zoom;
        const f32 view_h = static_cast<f32>(ah) * m_Zoom;

        // viewport の左上 screen pos を取得 (drawing baseline)。
        const ImVec2 origin = ImGui::GetCursorScreenPos();

        // InvisibleButton で hit area + 内容領域確保 (Dummy より drag を取りやすい)。
        ImGui::InvisibleButton("##atlas_canvas", ImVec2(view_w, view_h));

        ImDrawList* dl = ImGui::GetWindowDrawList();

        // 背景塗り + grid 線。
        const ImU32 bg_col   = IM_COL32( 32,  32,  40, 255);
        const ImU32 grid_col = IM_COL32( 80,  80,  90, 255);
        dl->AddRectFilled(origin,
                          ImVec2(origin.x + view_w, origin.y + view_h),
                          bg_col);
        // 32px grid (atlas-space)。zoom 適用後の screen 距離が 8px 未満になる
        // 場合は描画を間引く (視認性 + 過密回避)。
        const f32 step_atlas = 32.0f;
        const f32 step_view  = step_atlas * m_Zoom;
        if (step_view >= 8.0f) {
            for (f32 gx = 0.0f; gx <= static_cast<f32>(aw) + 0.001f; gx += step_atlas) {
                const f32 sx = origin.x + gx * m_Zoom;
                dl->AddLine(ImVec2(sx, origin.y),
                            ImVec2(sx, origin.y + view_h),
                            grid_col, 1.0f);
            }
            for (f32 gy = 0.0f; gy <= static_cast<f32>(ah) + 0.001f; gy += step_atlas) {
                const f32 sy = origin.y + gy * m_Zoom;
                dl->AddLine(ImVec2(origin.x, sy),
                            ImVec2(origin.x + view_w, sy),
                            grid_col, 1.0f);
            }
        }

        // atlas 外枠を強調。
        dl->AddRect(origin,
                    ImVec2(origin.x + view_w, origin.y + view_h),
                    IM_COL32(180, 180, 180, 255), 0.0f, 0, 1.5f);

        // 全 frame の矩形 overlay。
        if (m_Pack != nullptr) {
            u32 count = 0;
            const FSpriteFrame* frames = m_Pack->AllFrames(count);
            for (u32 i = 0; i < count; ++i) {
                const FSpriteFrame& f = frames[i];
                const ImVec2 r_min = AtlasToScreen(origin, m_Zoom,
                                                   static_cast<i32>(f.x),
                                                   static_cast<i32>(f.y));
                const ImVec2 r_max = AtlasToScreen(origin, m_Zoom,
                                                   static_cast<i32>(f.x + f.w),
                                                   static_cast<i32>(f.y + f.h));
                const bool is_sel = (static_cast<i32>(i) == m_Selected);
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

                    // handle drag による rect 編集。
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
                            const f32 inv_zoom = (m_Zoom > 0.0f) ? (1.0f / m_Zoom) : 1.0f;

                            // FSpritePack は AllFrames が const なので、編集には
                            // const_cast を使う (FSpritePack::AllFrames コメントで
                            // 「内部配列の先頭」と明記、x/y/w/h は不変条件外)。
                            FSpriteFrame* mut_frames =
                                const_cast<FSpriteFrame*>(frames);
                            FSpriteFrame& tgt = mut_frames[static_cast<usize>(m_Selected)];

                            // 1 フレーム分の mouse delta を atlas px に丸めて
                            // 適用 (= cumulative delta をその場で反映、drag end で
                            // FSpritePack の現在値が確定)。
                            // 厳密な undo 一貫性 (drag 開始時 baseline → 累積 delta)
                            // が必要なら「drag 開始時 snapshot」を
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

    // 右カラム: inspector。
    ImGui::BeginChild("##atlas_inspector", ImVec2(right_w, 0.0f), true);
    {
        ImGui::TextUnformatted("Inspector");
        ImGui::Separator();

        if (m_Pack == nullptr) {
            ImGui::TextDisabled("(no pack)");
        } else {
            u32 count = 0;
            const FSpriteFrame* frames = m_Pack->AllFrames(count);
            if (m_Selected < 0 || m_Selected >= static_cast<i32>(count) || frames == nullptr) {
                ImGui::TextDisabled("(no selection)");
            } else {
                // const → mutable へ (Pivot 編集と同じ理由、§SetPivotPreset 参照)。
                FSpriteFrame* mut_frames = const_cast<FSpriteFrame*>(frames);
                FSpriteFrame& f = mut_frames[static_cast<usize>(m_Selected)];

                ImGui::Text("name: %s", (f.name != nullptr) ? f.name : "(null)");
                ImGui::Separator();

                // atlas size を取得 (slider 上限用)。
                u32 aw = 256u;
                u32 ah = 256u;
                const FSpritePackInfo& info = m_Pack->Info();
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
                    m_PivotPreset = EPivotPreset::Custom;
                }
                if (ImGui::SliderFloat("pivot_y", &py, 0.0f, 1.0f, "%.3f")) {
                    f.pivot_y = ClampF32(py, 0.0f, 1.0f);
                    m_PivotPreset = EPivotPreset::Custom;
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
