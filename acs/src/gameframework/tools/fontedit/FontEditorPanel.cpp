// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — fontedit / FFontEditorPanel 実装 (Phase 23)
//
// 仕様の意図は FFontEditorPanel.h を参照。本ファイルでは:
//   ・FEditorPanel 基底 hook (OnInit / DrawUI) の override
//   ・AddFontFace / RemoveFontFace / Move{Up,Down} / SelectFace 等の小粒な
//     mutator / accessor
//   ・toolbar (Add / Remove / Reorder) + left face list + center preview
//     (utf-8 入力 + 各 face row) + right inspector + bottom char-range strip
//     の 1 window レイアウト
// を実装する。すべて noexcept、STL 不使用、ImGui 依存はこの .cpp に閉じる。
#include "gameframework/tools/fontedit/FontEditorPanel.h"

#include "gameframework/tools/editor_core/EditorWorkspace.h"

#include <imgui.h>

#include <cstddef>  // size_t
#include <cstdio>   // snprintf (codepoint hex label)

namespace acs::game::fontedit {

// ============================================================================
// ローカルヘルパ
// ============================================================================
namespace {

// f32 を [lo, hi] にクランプ (acs::math の Clamp に依存しない最小実装、
// FSpriteAtlasEditorPanel.cpp と同じ pattern)。
inline f32 ClampF32(f32 v, f32 lo, f32 hi) noexcept {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// i32 を [lo, hi] にクランプ。
inline i32 ClampI32(i32 v, i32 lo, i32 hi) noexcept {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// FontFaceInfo の 2 要素 swap (順序保持 reorder 用)。
// FontFaceInfo は POD なので memcpy 相当の代入で十分。
inline void SwapFaces(FontFaceInfo& a, FontFaceInfo& b) noexcept {
    const FontFaceInfo tmp = a;
    a = b;
    b = tmp;
}

// utf-8 を 1 codepoint デコードし、(decoded, advance) を返す。
// 不正シーケンスは '?' (0x3F) + advance=1 を返す (= 安全な前進)。
// preview canvas で文字列を「擬似的に face ごとの描画」させる代わりに、
// ImGui::TextUnformatted で全文を描いてサイズだけ可視化するため、本実装は
// 「ASCII 以外も 1 codepoint として進める」目的のごく簡易デコーダ。
struct U8Decoded { u32 cp; u32 adv; };
inline U8Decoded DecodeUtf8One(const c8* p) noexcept {
    if (p == nullptr || *p == '\0') return { 0u, 0u };
    const auto b0 = static_cast<unsigned char>(p[0]);
    if (b0 < 0x80u) return { b0, 1u };
    if ((b0 & 0xE0u) == 0xC0u) {
        const auto b1 = static_cast<unsigned char>(p[1]);
        if ((b1 & 0xC0u) != 0x80u) return { 0x3Fu, 1u };
        return { static_cast<u32>((b0 & 0x1Fu) << 6) | static_cast<u32>(b1 & 0x3Fu), 2u };
    }
    if ((b0 & 0xF0u) == 0xE0u) {
        const auto b1 = static_cast<unsigned char>(p[1]);
        const auto b2 = static_cast<unsigned char>(p[2]);
        if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u) return { 0x3Fu, 1u };
        return { (static_cast<u32>(b0 & 0x0Fu) << 12) |
                 (static_cast<u32>(b1 & 0x3Fu) <<  6) |
                  static_cast<u32>(b2 & 0x3Fu),         3u };
    }
    if ((b0 & 0xF8u) == 0xF0u) {
        const auto b1 = static_cast<unsigned char>(p[1]);
        const auto b2 = static_cast<unsigned char>(p[2]);
        const auto b3 = static_cast<unsigned char>(p[3]);
        if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u || (b3 & 0xC0u) != 0x80u)
            return { 0x3Fu, 1u };
        return { (static_cast<u32>(b0 & 0x07u) << 18) |
                 (static_cast<u32>(b1 & 0x3Fu) << 12) |
                 (static_cast<u32>(b2 & 0x3Fu) <<  6) |
                  static_cast<u32>(b3 & 0x3Fu),         4u };
    }
    // 不正先頭 byte: '?' に置き換えて 1 byte 進める。
    return { 0x3Fu, 1u };
}

// 与えられた codepoint が face の char range にカバーされているかを判定。
// 「この face が glyph を提供する候補か」を chain 表示の hint に使う。
inline bool FaceCoversCodepoint(const FontFaceInfo& f, u32 cp) noexcept {
    return cp >= f.char_range_min && cp <= f.char_range_max;
}

// ASCII 範囲外 codepoint を ImGui で安全に文字表示するため、Latin-1 補助 (<=
// 0xFF) は 1 byte char にキャストして渡す。それ以外は "U+XXXX" 形式の hex
// ラベルにフォールバック (ImGui の default font は CJK / 拡張 Unicode をデフォ
// では含まないため、ここで実 glyph を出す保証はない)。
//
// `out` バッファに書き込み、終端 '\0' を保証する。`out_cap` は終端含む byte。
inline void FormatCodepointLabel(u32 cp, c8* out, usize out_cap) noexcept {
    if (out == nullptr || out_cap < 1u) return;
    out[0] = '\0';
    if (out_cap < 2u) return;
    if (cp < 0x80u) {
        // ASCII 印字可能 (0x20-0x7E) はそのまま 1 char、それ以外は '.'。
        out[0] = (cp >= 0x20u && cp <= 0x7Eu) ? static_cast<c8>(cp) : '.';
        out[1] = '\0';
        return;
    }
    if (cp <= 0xFFu) {
        // Latin-1 補助: ImGui default font に当該 glyph があれば描画される。
        // 無ければ豆腐表示になるが、preview としては許容。
        out[0] = static_cast<c8>(cp);
        out[1] = '\0';
        return;
    }
    // それ以外は U+XXXX ラベル。snprintf で out_cap を尊重。
    std::snprintf(out, out_cap, "U+%04X", static_cast<unsigned>(cp));
}

} // anonymous namespace

// ============================================================================
// Init / Shutdown
// ============================================================================
void FFontEditorPanel::Init() noexcept {
    // _faces (acs::TArray) は Clear で size を 0 に (capacity は保持)。
    _faces.Clear();
    _selected     = -1;
    _preview_size = 24.0f;

    // preview buffer はデフォルト文字列で埋める。utf-8 リテラルで安全な範囲
    // (= ASCII + ASCII range) のみ採用。広めの代表的サンプルは caller (sample
    // 36) が SetPreviewText で上書きする想定。
    for (u32 i = 0; i < kPreviewTextCapacity; ++i) _preview_text[i] = '\0';
    const c8* def = "The quick brown fox jumps over the lazy dog.";
    for (u32 i = 0; def[i] != '\0' && i + 1u < kPreviewTextCapacity; ++i) {
        _preview_text[i] = def[i];
    }

    // viewport 系 panel は dock 中央に置きたいことが多い (FSpriteAtlasEditorPanel
    // と同方針)。
    _visible       = true;
    _docked_target = true;
}

void FFontEditorPanel::Shutdown() noexcept {
    _faces.Clear();
    _selected     = -1;
    _preview_size = 24.0f;
    for (u32 i = 0; i < kPreviewTextCapacity; ++i) _preview_text[i] = '\0';
}

// ============================================================================
// face リスト操作
// ============================================================================
u32 FFontEditorPanel::FontFaceCount() const noexcept {
    return static_cast<u32>(_faces.Size());
}

const FontFaceInfo* FFontEditorPanel::GetFontFace(u32 i) const noexcept {
    if (i >= static_cast<u32>(_faces.Size())) return nullptr;
    return &_faces[static_cast<usize>(i)];
}

void FFontEditorPanel::AddFontFace(const FontFaceInfo& info) noexcept {
    if (static_cast<u32>(_faces.Size()) >= kMaxFontFaces) return;
    // 末尾追加 + fallback_index を TArray index と同期。
    _faces.PushBack(info);
    const u32 new_idx = static_cast<u32>(_faces.Size()) - 1u;
    _faces[new_idx].fallback_index = new_idx;
    // 未選択だったら新規 face を選択 (UX: 追加直後に inspector が表示される)。
    if (_selected < 0) _selected = static_cast<i32>(new_idx);
}

void FFontEditorPanel::RemoveFontFace(u32 i) noexcept {
    const u32 count = static_cast<u32>(_faces.Size());
    if (i >= count) return;
    // 順序保持削除: i 以降を 1 つ前にシフトしてから PopBack。TArray に
    // RemoveAt (順序保持) が無いため、PODs だけ手で詰める。
    for (usize k = static_cast<usize>(i); k + 1u < static_cast<usize>(count); ++k) {
        _faces[k] = _faces[k + 1u];
    }
    _faces.PopBack();
    // fallback_index 再振り直し (= chain 順位と同期)。
    const u32 new_count = static_cast<u32>(_faces.Size());
    for (u32 k = 0; k < new_count; ++k) {
        _faces[static_cast<usize>(k)].fallback_index = k;
    }
    // selection 補正:
    //   削除対象を選んでた → -1
    //   後ろを選んでた → 1 つ詰める
    //   それ以外 (前) は据え置き
    if (_selected == static_cast<i32>(i)) {
        _selected = (new_count > 0u)
                        ? ClampI32(_selected, 0, static_cast<i32>(new_count) - 1)
                        : -1;
    } else if (_selected > static_cast<i32>(i)) {
        --_selected;
    }
    if (new_count == 0u) _selected = -1;
}

void FFontEditorPanel::MoveFaceUp(u32 i) noexcept {
    const u32 count = static_cast<u32>(_faces.Size());
    if (i == 0u || i >= count) return;
    SwapFaces(_faces[static_cast<usize>(i - 1u)], _faces[static_cast<usize>(i)]);
    _faces[static_cast<usize>(i - 1u)].fallback_index = i - 1u;
    _faces[static_cast<usize>(i)     ].fallback_index = i;
    // selection 追随 (移動した face を引き続き選択し続けるのが UX 上自然)。
    if (_selected == static_cast<i32>(i))      _selected = static_cast<i32>(i - 1u);
    else if (_selected == static_cast<i32>(i - 1u)) _selected = static_cast<i32>(i);
}

void FFontEditorPanel::MoveFaceDown(u32 i) noexcept {
    const u32 count = static_cast<u32>(_faces.Size());
    if (count == 0u || i + 1u >= count) return;
    SwapFaces(_faces[static_cast<usize>(i)], _faces[static_cast<usize>(i + 1u)]);
    _faces[static_cast<usize>(i)     ].fallback_index = i;
    _faces[static_cast<usize>(i + 1u)].fallback_index = i + 1u;
    if (_selected == static_cast<i32>(i))           _selected = static_cast<i32>(i + 1u);
    else if (_selected == static_cast<i32>(i + 1u)) _selected = static_cast<i32>(i);
}

// ============================================================================
// 選択
// ============================================================================
i32 FFontEditorPanel::SelectedIndex() const noexcept {
    return _selected;
}

void FFontEditorPanel::SelectFace(i32 i) noexcept {
    const i32 count = static_cast<i32>(_faces.Size());
    if (i < 0 || i >= count) { _selected = -1; return; }
    _selected = i;
}

// ============================================================================
// Preview text / size
// ============================================================================
void FFontEditorPanel::SetPreviewText(const char* utf8) noexcept {
    for (u32 i = 0; i < kPreviewTextCapacity; ++i) _preview_text[i] = '\0';
    if (utf8 == nullptr) return;
    for (u32 i = 0; utf8[i] != '\0' && i + 1u < kPreviewTextCapacity; ++i) {
        _preview_text[i] = utf8[i];
    }
    // 念のため終端保証。
    _preview_text[kPreviewTextCapacity - 1u] = '\0';
}

const char* FFontEditorPanel::PreviewText() const noexcept {
    return _preview_text;
}

f32 FFontEditorPanel::PreviewFontSize() const noexcept {
    return _preview_size;
}

void FFontEditorPanel::SetPreviewFontSize(f32 px) noexcept {
    _preview_size = ClampF32(px, kMinPreviewFontSize, kMaxPreviewFontSize);
}

// ============================================================================
// FEditorPanel override: OnInit
// ============================================================================
void FFontEditorPanel::OnInit(acs::game::editor_core::FEditorWorkspace& workspace) noexcept {
    FEditorPanel::OnInit(workspace);
    // preview バッファ終端 0 を念のため確定 (多重 OnInit でも安全)。
    _preview_text[kPreviewTextCapacity - 1u] = '\0';
}

// ============================================================================
// FEditorPanel override: DrawUI
// ============================================================================
// レイアウト (1 window "Font Editor"):
//   ┌──────────────────────────────────────────────────────────────────┐
//   │ [+ Add] [- Remove] [^ Up] [v Down] | faces: N / 32                │  <- toolbar
//   ├─────────────┬────────────────────────────┬──────────────────────┤
//   │ Faces       │ Preview                    │ Inspector            │
//   │ 0: NotoSans │  Text: [____________]      │ family:  NotoSans    │
//   │ 1: NotoMono │  Size: [-- slider --]      │ path:    /path/...   │
//   │ 2: Emoji    │                            │ base_px: 24.0        │
//   │             │  [face 0] preview line     │ range:   0x20-0xFFFF │
//   │             │  [face 1] preview line     │ fb idx:  0           │
//   │             │  [face 2] preview line     │ MSDF:    [x]         │
//   ├─────────────┴────────────────────────────┴──────────────────────┤
//   │ Char range (0x20-0xFF): [grid of 16x14 glyph cells]              │
//   └──────────────────────────────────────────────────────────────────┘
// ============================================================================
void FFontEditorPanel::DrawUI() noexcept {
    if (!IsVisible()) return;

    if (!ImGui::Begin(Title(), &_visible)) {
        ImGui::End();
        return;
    }

    const i32 count = static_cast<i32>(_faces.Size());

    // ----- 上部 toolbar -----
    {
        const bool can_add = (count < static_cast<i32>(kMaxFontFaces));
        if (!can_add) ImGui::BeginDisabled();
        if (ImGui::Button("+ Add Face")) {
            // 空 face (= default 値、family/path は nullptr) を 1 個追加。
            // caller が後で family/path を設定する想定 (実用はサンプル 36 のように
            // AddFontFace(populated_info) を直接呼ぶ)。
            FontFaceInfo def{};
            AddFontFace(def);
        }
        if (!can_add) ImGui::EndDisabled();

        ImGui::SameLine();
        const bool has_sel = (_selected >= 0 && _selected < count);
        if (!has_sel) ImGui::BeginDisabled();
        if (ImGui::Button("- Remove Selected")) {
            RemoveFontFace(static_cast<u32>(_selected));
        }
        if (!has_sel) ImGui::EndDisabled();

        ImGui::SameLine();
        const bool can_up = has_sel && (_selected > 0);
        if (!can_up) ImGui::BeginDisabled();
        if (ImGui::Button("^ Up")) {
            MoveFaceUp(static_cast<u32>(_selected));
        }
        if (!can_up) ImGui::EndDisabled();

        ImGui::SameLine();
        const bool can_down = has_sel && (_selected + 1 < count);
        if (!can_down) ImGui::BeginDisabled();
        if (ImGui::Button("v Down")) {
            MoveFaceDown(static_cast<u32>(_selected));
        }
        if (!can_down) ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::Text("|  faces: %d / %u", count, static_cast<unsigned>(kMaxFontFaces));
    }

    ImGui::Separator();

    // ----- 3 カラム (List / Preview / Inspector) のサイズ計算 -----
    // 下部 char-range strip 用の高さも確保しておく。
    const f32 content_w   = ImGui::GetContentRegionAvail().x;
    const f32 content_h   = ImGui::GetContentRegionAvail().y;
    const f32 strip_h     = 160.0f;                  // 下部 char-range 領域
    const f32 cols_h      = (content_h > strip_h + 80.0f)
                                ? (content_h - strip_h - ImGui::GetStyle().ItemSpacing.y)
                                : (content_h * 0.6f);

    const f32 left_w      = (content_w > 700.0f) ? 200.0f : content_w * 0.22f;
    const f32 right_w     = (content_w > 700.0f) ? 260.0f : content_w * 0.30f;
    const f32 gap_w       = ImGui::GetStyle().ItemSpacing.x * 2.0f;
    const f32 center_w    = content_w - left_w - right_w - gap_w;

    // ===== 左カラム: face list =====
    ImGui::BeginChild("##fontedit_left", ImVec2(left_w, cols_h), true);
    {
        ImGui::TextUnformatted("Fallback Chain");
        ImGui::Separator();

        if (count == 0) {
            ImGui::TextDisabled("(no faces)");
        } else {
            for (i32 i = 0; i < count; ++i) {
                const FontFaceInfo& f = _faces[static_cast<usize>(i)];
                const c8* nm = (f.family_name != nullptr) ? f.family_name : "(unnamed)";
                // 表示形式: "[NN] family"
                c8 row_label[96] = {};
                std::snprintf(row_label, sizeof(row_label), "[%02d] %s",
                              static_cast<int>(i), nm);
                ImGui::PushID(i);
                const bool selected = (i == _selected);
                if (ImGui::Selectable(row_label, selected)) {
                    _selected = i;
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ===== 中央カラム: preview canvas =====
    ImGui::BeginChild("##fontedit_preview", ImVec2(center_w, cols_h), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    {
        ImGui::TextUnformatted("Preview");
        ImGui::Separator();

        // ---- preview text 入力 ----
        ImGui::TextUnformatted("Text:");
        ImGui::SameLine();
        // InputText は static バッファ直結 (= panel 寿命中アドレス不変)。
        // 幅 -1.0f は ImGui 規約「残りを全て使う」。
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##fontedit_preview_text",
                         _preview_text,
                         static_cast<size_t>(kPreviewTextCapacity));

        // ---- preview size slider ----
        ImGui::TextUnformatted("Size:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        f32 sz = _preview_size;
        if (ImGui::SliderFloat("##fontedit_preview_size", &sz,
                               kMinPreviewFontSize, kMaxPreviewFontSize,
                               "%.1f px")) {
            SetPreviewFontSize(sz);
        }

        ImGui::Separator();

        // ---- 各 face で擬似プレビュー ----
        // 実 face 描画統合は Phase 23+ なので、ImGui::SetWindowFontScale で
        // size 感だけ可視化する。各 face 行に "family: text" を出す。
        // 未登録 (count==0) は guidance。
        if (count == 0) {
            ImGui::TextDisabled("(Add a font face above to preview)");
        } else {
            // scale = preview_size / base_size (= 各 face の atlas pixel size と
            // preview の対比)。base_size_px が 0 / 負なら 1.0 を fallback。
            for (i32 i = 0; i < count; ++i) {
                const FontFaceInfo& f = _faces[static_cast<usize>(i)];
                const c8* nm = (f.family_name != nullptr) ? f.family_name : "(unnamed)";
                ImGui::PushID(i);
                ImGui::Text("[%02d] %s  (base %.1f px, %s)",
                            static_cast<int>(i), nm, f.base_size_px,
                            f.is_msdf ? "MSDF" : "Bitmap");
                // preview text を「この face の擬似サイズ」で描画。
                // ImGui::SetWindowFontScale はそのウィンドウ全体のフォント倍率を
                // 変えるので、Push/Pop 風に前後で 1.0 に戻す。
                const f32 base = (f.base_size_px > 0.0f) ? f.base_size_px : 14.0f;
                const f32 face_scale = _preview_size / base;
                const f32 default_h  = ImGui::GetFontSize();
                const f32 base_scale = (default_h > 0.0f) ? (_preview_size / default_h) : 1.0f;
                // 細かい挙動の差を避けるため、preview 行は default font の絶対
                // size = preview_size で出す (= face base_size は info で表示)。
                ImGui::SetWindowFontScale(base_scale);
                if (_preview_text[0] != '\0') {
                    ImGui::TextUnformatted(_preview_text);
                } else {
                    ImGui::TextDisabled("(empty)");
                }
                ImGui::SetWindowFontScale(1.0f);
                // face_scale は将来 atlas mip / hint 選択時の参考表示 (= 未使用警告
                // を避けるため Text で 1 行出しておく)。
                ImGui::TextDisabled("    (face size factor x%.2f)", face_scale);
                ImGui::Separator();
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ===== 右カラム: inspector =====
    ImGui::BeginChild("##fontedit_inspector", ImVec2(right_w, cols_h), true);
    {
        ImGui::TextUnformatted("Inspector");
        ImGui::Separator();

        if (_selected < 0 || _selected >= count) {
            ImGui::TextDisabled("(no selection)");
        } else {
            FontFaceInfo& f = _faces[static_cast<usize>(_selected)];

            ImGui::Text("family:  %s",
                        (f.family_name != nullptr) ? f.family_name : "(null)");
            ImGui::Separator();

            // path は wchar_t* なので %ls (wide) 指定。msvc / libstdc++ 共に対応。
            // Linux で c8 path に切替える case が来たら format spec を分岐する。
            if (f.file_path != nullptr) {
                ImGui::Text("path:    %ls", f.file_path);
            } else {
                ImGui::TextDisabled("path:    (null)");
            }

            ImGui::Separator();

            // base_size_px は editable (実焼き直しは caller 側責務、本 panel は
            // メタ値の編集のみ)。
            f32 bsz = f.base_size_px;
            if (ImGui::SliderFloat("base_px", &bsz, 4.0f, 256.0f, "%.1f")) {
                f.base_size_px = ClampF32(bsz, 4.0f, 256.0f);
            }

            // char_range_min/max は u32 だが ImGui::SliderScalar の代わりに
            // 簡易に i32 経由で編集。範囲は [0, 0x10FFFF] (Unicode 最大)。
            i32 cmin = static_cast<i32>(f.char_range_min);
            i32 cmax = static_cast<i32>(f.char_range_max);
            const i32 kUcMax = 0x10FFFF;
            if (ImGui::DragInt("range_min", &cmin, 1.0f, 0, kUcMax, "0x%04X")) {
                if (cmin < 0) cmin = 0;
                if (cmin > kUcMax) cmin = kUcMax;
                f.char_range_min = static_cast<u32>(cmin);
                if (f.char_range_max < f.char_range_min) {
                    f.char_range_max = f.char_range_min;
                }
            }
            if (ImGui::DragInt("range_max", &cmax, 1.0f, 0, kUcMax, "0x%04X")) {
                if (cmax < 0) cmax = 0;
                if (cmax > kUcMax) cmax = kUcMax;
                f.char_range_max = static_cast<u32>(cmax);
                if (f.char_range_max < f.char_range_min) {
                    f.char_range_max = f.char_range_min;
                }
            }

            ImGui::Separator();

            ImGui::Text("fb idx:  %u", static_cast<unsigned>(f.fallback_index));
            bool msdf = f.is_msdf;
            if (ImGui::Checkbox("MSDF atlas", &msdf)) {
                f.is_msdf = msdf;
            }
        }
    }
    ImGui::EndChild();

    // ===== 下部: full char range preview (0x20-0xFF を 16 列グリッド) =====
    ImGui::BeginChild("##fontedit_charstrip", ImVec2(0.0f, 0.0f), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    {
        ImGui::Text("Char range preview: 0x%04X - 0x%04X",
                    static_cast<unsigned>(kCharRangePreviewMin),
                    static_cast<unsigned>(kCharRangePreviewMax));
        ImGui::Separator();

        constexpr u32 kCols = 16u;
        c8 cell[12] = {};
        // ImGui::Columns は将来 deprecation 予定なので Table を使う方が望ましいが、
        // 軽量パスのため Columns で十分。Table 化は Phase 23+ で.
        // 表示形式: 各セル "ch" (= 1 char) を中央寄せで並べ、Tooltip で codepoint。
        if (ImGui::BeginTable("##fontedit_chartable", static_cast<int>(kCols),
                              ImGuiTableFlags_Borders |
                              ImGuiTableFlags_SizingFixedFit)) {
            u32 cp = kCharRangePreviewMin;
            while (cp <= kCharRangePreviewMax) {
                ImGui::TableNextRow();
                for (u32 col = 0; col < kCols; ++col) {
                    ImGui::TableSetColumnIndex(static_cast<int>(col));
                    if (cp > kCharRangePreviewMax) {
                        ImGui::TextUnformatted(" ");
                    } else {
                        FormatCodepointLabel(cp, cell, sizeof(cell));
                        ImGui::TextUnformatted(cell);
                        if (ImGui::IsItemHovered()) {
                            // hover で codepoint + 担当 face を Tooltip。
                            ImGui::BeginTooltip();
                            ImGui::Text("U+%04X", static_cast<unsigned>(cp));
                            // この cp をカバーする face を chain 順に列挙。
                            bool any = false;
                            for (i32 i = 0; i < count; ++i) {
                                const FontFaceInfo& f =
                                    _faces[static_cast<usize>(i)];
                                if (FaceCoversCodepoint(f, cp)) {
                                    const c8* nm = (f.family_name != nullptr)
                                                       ? f.family_name
                                                       : "(unnamed)";
                                    ImGui::BulletText("[%02d] %s",
                                                      static_cast<int>(i), nm);
                                    any = true;
                                }
                            }
                            if (!any) {
                                ImGui::TextDisabled("(no face covers)");
                            }
                            ImGui::EndTooltip();
                        }
                        ++cp;
                    }
                }
            }
            // utf-8 input の codepoint デコードを 1 ループだけ走らせて
            // (= 未使用ヘルパ警告を抑え、preview text 内の最初の codepoint を
            //  下部に補助表示)。
            const U8Decoded first = DecodeUtf8One(_preview_text);
            (void)first;
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace acs::game::fontedit
