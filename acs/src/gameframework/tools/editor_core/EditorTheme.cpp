// SPDX-License-Identifier: Apache-2.0
// GameFramework Tools — editor_core / FEditorTheme 実装 (Phase 21a)
//
// 仕様詳細は FEditorTheme.h を参照。本ファイルでは:
//   ・preset 種別ごとの色テーブル定義 (Dark / DarkBlue / Light / HighContrast /
//     Sepia / Custom)
//   ・ACS::FVec4 → ImVec4 の橋渡し + ImGui::GetStyle() への適用
//   ・ImGui font_scale / corner_radius / spacing の流し込み
//   ・DrawThemeSettingsUI: preset combo + ColorEdit4 群 + Save/Load ボタン
//   ・SaveTheme / LoadTheme: `.acstheme` テキスト I/O (FFxeditSerializer と
//     同設計、1 行 1 key=value、magic + version、git diff フレンドリー)
// を実装する。全 noexcept、STL 不使用、ImGui 依存はこの .cpp に閉じる。
#include "gameframework/tools/editor_core/EditorTheme.h"

#include "foundation/Log.h"
#include "platform/FileSystem.h"

#include <imgui.h>

#include <cstdio>   // std::snprintf
#include <cstring>  // std::strncmp / std::strlen
#include <cstdlib>  // std::strtof

namespace acs::game::editor_core {

// =============================================================================
// ローカルヘルパ
// =============================================================================
namespace {

// ACS::FVec4 → ImVec4 の単純変換。レイアウトは互換 (どちらも float x4) だが
// 型は別物なので明示変換で混乱を避ける。
ImVec4 ToImVec4(const FVec4& v) noexcept {
    return ImVec4(v.x, v.y, v.z, v.w);
}

// ImVec4 → ACS::FVec4 の逆変換 (LoadTheme で ImGui::ColorEdit が出した値を
// 受け取る経路では使わないが、対称性のため用意)。
FVec4 FromImVec4(const ImVec4& v) noexcept {
    return FVec4{v.x, v.y, v.z, v.w};
}

// preset 名を `.acstheme` テキスト用に const char* で返す。LoadTheme の
// parser でも逆引きする (= ToPreset)。Custom は読み書きどちらも対応。
const char* PresetName(EEditorThemePreset p) noexcept {
    switch (p) {
        case EEditorThemePreset::Dark:         return "Dark";
        case EEditorThemePreset::DarkBlue:     return "DarkBlue";
        case EEditorThemePreset::Light:        return "Light";
        case EEditorThemePreset::HighContrast: return "HighContrast";
        case EEditorThemePreset::Sepia:        return "Sepia";
        case EEditorThemePreset::Custom:       return "Custom";
    }
    return "Dark";
}

// 名前 → preset の逆引き。strncmp で正確一致のみ受け入れる。
// 一致しなければ Dark を返す (= 安全側 fallback)。
EEditorThemePreset ToPreset(const char* name) noexcept {
    if (name == nullptr) return EEditorThemePreset::Dark;
    // 順序は enum 値順に揃え、文字列長で枝刈り。
    if (std::strcmp(name, "Dark")         == 0) return EEditorThemePreset::Dark;
    if (std::strcmp(name, "DarkBlue")     == 0) return EEditorThemePreset::DarkBlue;
    if (std::strcmp(name, "Light")        == 0) return EEditorThemePreset::Light;
    if (std::strcmp(name, "HighContrast") == 0) return EEditorThemePreset::HighContrast;
    if (std::strcmp(name, "Sepia")        == 0) return EEditorThemePreset::Sepia;
    if (std::strcmp(name, "Custom")       == 0) return EEditorThemePreset::Custom;
    return EEditorThemePreset::Dark;
}

// ImGui context が存在するかの簡易チェック。Init 前 / context 破棄後の
// 多重呼び出しから守るため、各 Apply* 系の冒頭で確認する。
bool HasImGuiContext() noexcept {
    return ImGui::GetCurrentContext() != nullptr;
}

} // namespace

// =============================================================================
// preset 色テーブル
// -----------------------------------------------------------------------------
// 各 preset の RGBA `[0, 1]` 範囲の色。コメントの hex 値は人間用の参考表記
// (実値は f32 で書く: `0x1E / 255.0 ≒ 0.118` 等)。基本色相設計:
//
//   Dark         : 中間グレー (#2E2E2E) ベース、accent オレンジ (#FF9F40)。
//                  ImGui 既定 StyleColorsDark より僅かに暖色寄り。
//   DarkBlue     : VS Code "Dark+" 風青み grey (#1F232C)、accent VSCode blue
//                  (#007ACC)。
//   Light        : 白基調 (#F0F0F0)、accent VSCode blue (#0078D7) を darker に
//                  下げて視認性確保。
//   HighContrast : 黒背景 (#000000) + 白文字 (#FFFFFF) + 黄 accent (#FFD700)。
//                  WCAG AAA レベルのコントラスト比 (≥ 7:1) を意識。
//   Sepia        : 茶背景 (#3A2E22) + クリーム文字 (#F4E8D8) + 焦茶 accent
//                  (#A07050)。e-reader 風。
// =============================================================================
void FEditorTheme::FillPresetColors(EEditorThemePreset       preset,
                                   EditorThemeColors&       out) noexcept {
    switch (preset) {
        case EEditorThemePreset::Dark: {
            out.window_bg     = FVec4{0.180f, 0.180f, 0.180f, 1.0f};  // #2E2E2E
            out.title_bg      = FVec4{0.130f, 0.130f, 0.130f, 1.0f};  // #212121
            out.button_bg     = FVec4{0.300f, 0.300f, 0.300f, 1.0f};  // #4D4D4D
            out.button_hover  = FVec4{0.420f, 0.420f, 0.420f, 1.0f};  // #6B6B6B
            out.button_active = FVec4{0.550f, 0.550f, 0.550f, 1.0f};  // #8C8C8C
            out.frame_bg      = FVec4{0.230f, 0.230f, 0.230f, 1.0f};  // #3B3B3B
            out.text          = FVec4{0.950f, 0.950f, 0.950f, 1.0f};  // #F2F2F2
            out.text_disabled = FVec4{0.500f, 0.500f, 0.500f, 1.0f};  // #808080
            out.border        = FVec4{0.430f, 0.430f, 0.430f, 0.50f};
            out.separator     = FVec4{0.430f, 0.430f, 0.430f, 0.50f};
            out.accent        = FVec4{1.000f, 0.624f, 0.251f, 1.0f};  // #FF9F40
            out.warning       = FVec4{1.000f, 0.800f, 0.200f, 1.0f};  // #FFCC33
            out.error         = FVec4{0.940f, 0.330f, 0.310f, 1.0f};  // #F0544F
            break;
        }
        case EEditorThemePreset::DarkBlue: {
            out.window_bg     = FVec4{0.122f, 0.137f, 0.173f, 1.0f};  // #1F232C
            out.title_bg      = FVec4{0.094f, 0.106f, 0.137f, 1.0f};  // #181B23
            out.button_bg     = FVec4{0.180f, 0.220f, 0.290f, 1.0f};  // #2E384A
            out.button_hover  = FVec4{0.250f, 0.310f, 0.420f, 1.0f};  // #404F6B
            out.button_active = FVec4{0.330f, 0.420f, 0.560f, 1.0f};  // #546B8F
            out.frame_bg      = FVec4{0.160f, 0.180f, 0.230f, 1.0f};  // #292E3B
            out.text          = FVec4{0.870f, 0.890f, 0.930f, 1.0f};  // #DEE3ED
            out.text_disabled = FVec4{0.450f, 0.480f, 0.540f, 1.0f};  // #737A8A
            out.border        = FVec4{0.250f, 0.290f, 0.360f, 0.60f};
            out.separator     = FVec4{0.250f, 0.290f, 0.360f, 0.60f};
            out.accent        = FVec4{0.000f, 0.478f, 0.800f, 1.0f};  // #007ACC
            out.warning       = FVec4{0.900f, 0.700f, 0.100f, 1.0f};  // #E5B219
            out.error         = FVec4{0.960f, 0.270f, 0.270f, 1.0f};  // #F54545
            break;
        }
        case EEditorThemePreset::Light: {
            out.window_bg     = FVec4{0.940f, 0.940f, 0.940f, 1.0f};  // #F0F0F0
            out.title_bg      = FVec4{0.850f, 0.850f, 0.850f, 1.0f};  // #D9D9D9
            out.button_bg     = FVec4{0.860f, 0.860f, 0.860f, 1.0f};  // #DBDBDB
            out.button_hover  = FVec4{0.730f, 0.730f, 0.730f, 1.0f};  // #BABABA
            out.button_active = FVec4{0.610f, 0.610f, 0.610f, 1.0f};  // #9C9C9C
            out.frame_bg      = FVec4{1.000f, 1.000f, 1.000f, 1.0f};  // #FFFFFF
            out.text          = FVec4{0.100f, 0.100f, 0.100f, 1.0f};  // #1A1A1A
            out.text_disabled = FVec4{0.550f, 0.550f, 0.550f, 1.0f};  // #8C8C8C
            out.border        = FVec4{0.000f, 0.000f, 0.000f, 0.30f};
            out.separator     = FVec4{0.390f, 0.390f, 0.390f, 0.50f};
            out.accent        = FVec4{0.000f, 0.470f, 0.840f, 1.0f};  // #0078D7
            out.warning       = FVec4{0.800f, 0.520f, 0.000f, 1.0f};  // #CC8500
            out.error         = FVec4{0.820f, 0.180f, 0.180f, 1.0f};  // #D12E2E
            break;
        }
        case EEditorThemePreset::HighContrast: {
            // WCAG AAA (>= 7:1) 想定の三色設計 (黒 / 白 / 黄)。
            out.window_bg     = FVec4{0.000f, 0.000f, 0.000f, 1.0f};  // #000000
            out.title_bg      = FVec4{0.000f, 0.000f, 0.000f, 1.0f};
            out.button_bg     = FVec4{0.100f, 0.100f, 0.100f, 1.0f};  // #1A1A1A
            out.button_hover  = FVec4{1.000f, 0.843f, 0.000f, 1.0f};  // #FFD700 (黄)
            out.button_active = FVec4{1.000f, 1.000f, 0.200f, 1.0f};  // #FFFF33
            out.frame_bg      = FVec4{0.100f, 0.100f, 0.100f, 1.0f};
            out.text          = FVec4{1.000f, 1.000f, 1.000f, 1.0f};  // #FFFFFF
            out.text_disabled = FVec4{0.700f, 0.700f, 0.700f, 1.0f};
            out.border        = FVec4{1.000f, 1.000f, 1.000f, 1.0f};  // 白枠で明示
            out.separator     = FVec4{1.000f, 1.000f, 1.000f, 0.80f};
            out.accent        = FVec4{1.000f, 0.843f, 0.000f, 1.0f};  // #FFD700
            out.warning       = FVec4{1.000f, 0.843f, 0.000f, 1.0f};  // 黄
            out.error         = FVec4{1.000f, 0.300f, 0.300f, 1.0f};  // 高彩度赤
            break;
        }
        case EEditorThemePreset::Sepia: {
            out.window_bg     = FVec4{0.227f, 0.180f, 0.133f, 1.0f};  // #3A2E22
            out.title_bg      = FVec4{0.184f, 0.149f, 0.114f, 1.0f};  // #2F261D
            out.button_bg     = FVec4{0.337f, 0.275f, 0.212f, 1.0f};  // #564636
            out.button_hover  = FVec4{0.471f, 0.388f, 0.298f, 1.0f};  // #78634C
            out.button_active = FVec4{0.580f, 0.482f, 0.376f, 1.0f};  // #947B60
            out.frame_bg      = FVec4{0.290f, 0.231f, 0.180f, 1.0f};  // #4A3B2E
            out.text          = FVec4{0.957f, 0.910f, 0.847f, 1.0f};  // #F4E8D8
            out.text_disabled = FVec4{0.620f, 0.560f, 0.490f, 1.0f};
            out.border        = FVec4{0.560f, 0.470f, 0.380f, 0.50f};
            out.separator     = FVec4{0.560f, 0.470f, 0.380f, 0.50f};
            out.accent        = FVec4{0.627f, 0.439f, 0.314f, 1.0f};  // #A07050
            out.warning       = FVec4{0.900f, 0.620f, 0.190f, 1.0f};
            out.error         = FVec4{0.840f, 0.290f, 0.190f, 1.0f};
            break;
        }
        case EEditorThemePreset::Custom:
            // Custom は呼び出し側 (SetCustomColors) が値を直接書き込むため、
            // ここでは out を変更しない (= 既存値を保持)。
            break;
    }
}

// =============================================================================
// Init — default = Dark を ImGui に流す
// =============================================================================
void FEditorTheme::Init() noexcept {
    _preset         = EEditorThemePreset::Dark;
    _font_scale     = 1.0f;
    _corner_radius  = 3.0f;
    _item_spacing_y = 4.0f;
    FillPresetColors(_preset, _colors);
    ApplyToImGui();
}

// =============================================================================
// ApplyPreset — preset 既定値を `_colors` に書き、ImGui に流す
// =============================================================================
void FEditorTheme::ApplyPreset(EEditorThemePreset preset) noexcept {
    _preset = preset;
    // Custom 以外なら _colors を上書き。Custom は SetCustomColors 経由で
    // 設定された値を保持したまま現値を再適用する (= preset 切替で Custom に
    // 戻すと、直前の Custom パレットが復活する設計)。
    if (preset != EEditorThemePreset::Custom) {
        FillPresetColors(preset, _colors);
    }
    ApplyToImGui();
}

// =============================================================================
// SetCustomColors — 任意パレットを設定 + Custom に切り替え
// =============================================================================
void FEditorTheme::SetCustomColors(const EditorThemeColors& colors) noexcept {
    _colors = colors;
    _preset = EEditorThemePreset::Custom;
    ApplyToImGui();
}

// =============================================================================
// SetFontScale — global font scale (高 DPI / HighContrast 視認性)
// =============================================================================
void FEditorTheme::SetFontScale(f32 scale) noexcept {
    // <= 0 は無視 (= no-op)。0 倍は ImGui を壊す。
    if (scale <= 0.0f) return;
    // 上限 4.0 で safety clamp (典型用途は 1.0 〜 2.0)。
    if (scale > 4.0f) {
        ACS_LOG_WARN("FEditorTheme::SetFontScale: clamp %.2f -> 4.0",
                     static_cast<double>(scale));
        scale = 4.0f;
    }
    _font_scale = scale;
    if (HasImGuiContext()) {
        ImGui::GetIO().FontGlobalScale = _font_scale;
    }
}

// =============================================================================
// SetRoundedCorners — 全 corner radius を統一
// =============================================================================
void FEditorTheme::SetRoundedCorners(f32 radius) noexcept {
    if (radius < 0.0f) radius = 0.0f;
    _corner_radius = radius;
    if (HasImGuiContext()) {
        ImGuiStyle& s     = ImGui::GetStyle();
        s.WindowRounding    = _corner_radius;
        s.FrameRounding     = _corner_radius;
        s.PopupRounding     = _corner_radius;
        s.GrabRounding      = _corner_radius;
        s.TabRounding       = _corner_radius;
        s.ScrollbarRounding = _corner_radius;
        s.ChildRounding     = _corner_radius;
    }
}

// =============================================================================
// SetSpacing — ItemSpacing.y (情報密度の主軸)
// =============================================================================
void FEditorTheme::SetSpacing(f32 item_spacing_y) noexcept {
    if (item_spacing_y < 0.0f) item_spacing_y = 0.0f;
    _item_spacing_y = item_spacing_y;
    if (HasImGuiContext()) {
        ImGuiStyle& s = ImGui::GetStyle();
        // x は y の 0.5 倍 (見た目バランスのための経験則: 横は詰め気味)。
        s.ItemSpacing = ImVec2(_item_spacing_y * 0.5f, _item_spacing_y);
    }
}

// =============================================================================
// ApplyToImGui — `_colors` / 各 metric を ImGui::GetStyle() / IO に流し込む
// =============================================================================
void FEditorTheme::ApplyToImGui() noexcept {
    if (!HasImGuiContext()) return;

    ImGuiStyle& s    = ImGui::GetStyle();
    ImVec4*     col  = s.Colors;

    // ---- 基本背景 / フレーム --------------------------------------------
    col[ImGuiCol_WindowBg]              = ToImVec4(_colors.window_bg);
    col[ImGuiCol_ChildBg]               = ToImVec4(_colors.window_bg);
    col[ImGuiCol_PopupBg]               = ToImVec4(_colors.window_bg);
    col[ImGuiCol_FrameBg]               = ToImVec4(_colors.frame_bg);
    col[ImGuiCol_FrameBgHovered]        = ToImVec4(_colors.button_hover);
    col[ImGuiCol_FrameBgActive]         = ToImVec4(_colors.button_active);

    // ---- タイトルバー ---------------------------------------------------
    col[ImGuiCol_TitleBg]               = ToImVec4(_colors.title_bg);
    col[ImGuiCol_TitleBgActive]         = ToImVec4(_colors.title_bg);
    col[ImGuiCol_TitleBgCollapsed]      = ToImVec4(_colors.title_bg);
    col[ImGuiCol_MenuBarBg]             = ToImVec4(_colors.title_bg);

    // ---- ボタン ---------------------------------------------------------
    col[ImGuiCol_Button]                = ToImVec4(_colors.button_bg);
    col[ImGuiCol_ButtonHovered]         = ToImVec4(_colors.button_hover);
    col[ImGuiCol_ButtonActive]          = ToImVec4(_colors.button_active);

    // ---- ヘッダ (CollapsingHeader / Selectable hover 等) -----------------
    col[ImGuiCol_Header]                = ToImVec4(_colors.button_bg);
    col[ImGuiCol_HeaderHovered]         = ToImVec4(_colors.button_hover);
    col[ImGuiCol_HeaderActive]          = ToImVec4(_colors.button_active);

    // ---- 文字 -----------------------------------------------------------
    col[ImGuiCol_Text]                  = ToImVec4(_colors.text);
    col[ImGuiCol_TextDisabled]          = ToImVec4(_colors.text_disabled);

    // ---- 枠 / 区切り ----------------------------------------------------
    col[ImGuiCol_Border]                = ToImVec4(_colors.border);
    col[ImGuiCol_BorderShadow]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    col[ImGuiCol_Separator]             = ToImVec4(_colors.separator);
    col[ImGuiCol_SeparatorHovered]      = ToImVec4(_colors.accent);
    col[ImGuiCol_SeparatorActive]       = ToImVec4(_colors.accent);

    // ---- accent (CheckMark / Slider / Tab / Resize) ----------------------
    col[ImGuiCol_CheckMark]             = ToImVec4(_colors.accent);
    col[ImGuiCol_SliderGrab]            = ToImVec4(_colors.accent);
    col[ImGuiCol_SliderGrabActive]      = ToImVec4(_colors.accent);
    col[ImGuiCol_ResizeGrip]            = ToImVec4(_colors.accent);
    col[ImGuiCol_ResizeGripHovered]     = ToImVec4(_colors.accent);
    col[ImGuiCol_ResizeGripActive]      = ToImVec4(_colors.accent);
    col[ImGuiCol_Tab]                   = ToImVec4(_colors.button_bg);
    col[ImGuiCol_TabHovered]            = ToImVec4(_colors.accent);
    col[ImGuiCol_TabActive]             = ToImVec4(_colors.accent);
    col[ImGuiCol_TabUnfocused]          = ToImVec4(_colors.button_bg);
    col[ImGuiCol_TabUnfocusedActive]    = ToImVec4(_colors.button_active);

    // ---- スクロールバー -------------------------------------------------
    col[ImGuiCol_ScrollbarBg]           = ToImVec4(_colors.frame_bg);
    col[ImGuiCol_ScrollbarGrab]         = ToImVec4(_colors.button_bg);
    col[ImGuiCol_ScrollbarGrabHovered]  = ToImVec4(_colors.button_hover);
    col[ImGuiCol_ScrollbarGrabActive]   = ToImVec4(_colors.button_active);

    // ---- corner / spacing / font ---------------------------------------
    s.WindowRounding    = _corner_radius;
    s.FrameRounding     = _corner_radius;
    s.PopupRounding     = _corner_radius;
    s.GrabRounding      = _corner_radius;
    s.TabRounding       = _corner_radius;
    s.ScrollbarRounding = _corner_radius;
    s.ChildRounding     = _corner_radius;

    // x は y の 0.5 倍 (見た目バランス)。
    s.ItemSpacing = ImVec2(_item_spacing_y * 0.5f, _item_spacing_y);

    ImGui::GetIO().FontGlobalScale = _font_scale;
}

// =============================================================================
// DrawThemeSettingsUI — preset combo + ColorEdit4 + Save/Load
// =============================================================================
void FEditorTheme::DrawThemeSettingsUI() noexcept {
    if (!HasImGuiContext()) return;

    if (!ImGui::Begin("Theme FSettings")) {
        ImGui::End();
        return;
    }

    // ---- preset combo ---------------------------------------------------
    const char* items[] = {
        "Dark", "DarkBlue", "Light", "HighContrast", "Sepia", "Custom",
    };
    int current = static_cast<int>(_preset);
    if (ImGui::Combo("Preset", &current, items, IM_ARRAYSIZE(items))) {
        ApplyPreset(static_cast<EEditorThemePreset>(current));
    }

    ImGui::Separator();

    // ---- カラー編集 -----------------------------------------------------
    // ColorEdit4 で値を drag するたびに ApplyToImGui を呼び、Custom に切替。
    bool changed = false;
    auto edit = [&](const char* label, FVec4& v) {
        ImVec4 tmp = ToImVec4(v);
        if (ImGui::ColorEdit4(label, &tmp.x)) {
            v = FromImVec4(tmp);
            changed = true;
        }
    };

    edit("FWindow BG",     _colors.window_bg);
    edit("Title BG",      _colors.title_bg);
    edit("Button BG",     _colors.button_bg);
    edit("Button Hover",  _colors.button_hover);
    edit("Button Active", _colors.button_active);
    edit("Frame BG",      _colors.frame_bg);
    edit("Text",          _colors.text);
    edit("Text Disabled", _colors.text_disabled);
    edit("Border",        _colors.border);
    edit("Separator",     _colors.separator);
    edit("Accent",        _colors.accent);
    edit("Warning",       _colors.warning);
    edit("Error",         _colors.error);

    if (changed) {
        _preset = EEditorThemePreset::Custom;
        ApplyToImGui();
    }

    ImGui::Separator();

    // ---- metric (font / corner / spacing) -------------------------------
    f32 font_scale = _font_scale;
    if (ImGui::SliderFloat("Font Scale", &font_scale, 0.5f, 3.0f, "%.2fx")) {
        SetFontScale(font_scale);
    }
    f32 corner = _corner_radius;
    if (ImGui::SliderFloat("Corner Radius", &corner, 0.0f, 16.0f, "%.1fpx")) {
        SetRoundedCorners(corner);
    }
    f32 spacing = _item_spacing_y;
    if (ImGui::SliderFloat("Item Spacing Y", &spacing, 0.0f, 16.0f, "%.1fpx")) {
        SetSpacing(spacing);
    }

    ImGui::Separator();

    // ---- Save / Load ----------------------------------------------------
    // Phase 21a 時点ではファイルダイアログが無いので固定パスを使う。
    // Phase 21b で FAssetBrowser 連動のファイルピッカーに置き換え予定。
    if (ImGui::Button("Save Theme")) {
        SaveTheme(L"data/editor/theme.acstheme");
    }
    ImGui::SameLine();
    if (ImGui::Button("Load Theme")) {
        LoadTheme(L"data/editor/theme.acstheme");
    }
    ImGui::TextDisabled("Path: data/editor/theme.acstheme");

    ImGui::End();
}

// =============================================================================
// SaveTheme — `.acstheme` テキスト書き出し
// -----------------------------------------------------------------------------
// フォーマット (FFxeditSerializer と同設計、1 行 1 key=value):
//   ACS_THEME 1
//   preset Dark
//   font_scale 1.00
//   corner_radius 3.00
//   item_spacing_y 4.00
//   window_bg 0.180 0.180 0.180 1.000
//   title_bg 0.130 0.130 0.130 1.000
//   ... (全 13 カラー)
//
// 失敗時は ACS_LOG_WARN で記録 (戻り値 void = ベストエフォート)。
// =============================================================================
void FEditorTheme::SaveTheme(const wchar_t* file_path) noexcept {
    if (file_path == nullptr) return;

    // 1 カラー = 約 50B (`key %.3f %.3f %.3f %.3f\n` = 5 + 6*4 + 1)。
    // 13 カラー + ヘッダ + metric 3 行 ≒ 1KB 弱。安全側で 2KB 確保。
    char buf[2048];
    int  pos = 0;

    // snprintf ベース。len < 0 (encoding error) または len + pos > capacity の
    // ケースは BufferOverflow 扱いで打ち切り。
    auto write_line = [&](const char* fmt, auto... args) noexcept -> bool {
        const int remain = static_cast<int>(sizeof(buf)) - pos;
        if (remain <= 1) return false;
        const int n = std::snprintf(buf + pos, static_cast<usize>(remain),
                                    fmt, args...);
        if (n < 0 || n >= remain) return false;
        pos += n;
        return true;
    };

    auto write_color = [&](const char* key, const FVec4& v) noexcept -> bool {
        return write_line("%s %.3f %.3f %.3f %.3f\n",
                          key,
                          static_cast<double>(v.x),
                          static_cast<double>(v.y),
                          static_cast<double>(v.z),
                          static_cast<double>(v.w));
    };

    // ---- ヘッダ + metric ------------------------------------------------
    if (!write_line("%s %u\n", kMagic, kCurrentVersion) ||
        !write_line("preset %s\n", PresetName(_preset)) ||
        !write_line("font_scale %.3f\n",      static_cast<double>(_font_scale)) ||
        !write_line("corner_radius %.3f\n",   static_cast<double>(_corner_radius)) ||
        !write_line("item_spacing_y %.3f\n",  static_cast<double>(_item_spacing_y))) {
        ACS_LOG_WARN("FEditorTheme::SaveTheme: header buffer overflow");
        return;
    }

    // ---- カラー 13 件 ---------------------------------------------------
    if (!write_color("window_bg",     _colors.window_bg)     ||
        !write_color("title_bg",      _colors.title_bg)      ||
        !write_color("button_bg",     _colors.button_bg)     ||
        !write_color("button_hover",  _colors.button_hover)  ||
        !write_color("button_active", _colors.button_active) ||
        !write_color("frame_bg",      _colors.frame_bg)      ||
        !write_color("text",          _colors.text)          ||
        !write_color("text_disabled", _colors.text_disabled) ||
        !write_color("border",        _colors.border)        ||
        !write_color("separator",     _colors.separator)     ||
        !write_color("accent",        _colors.accent)        ||
        !write_color("warning",       _colors.warning)       ||
        !write_color("error",         _colors.error)) {
        ACS_LOG_WARN("FEditorTheme::SaveTheme: color buffer overflow");
        return;
    }

    // ---- 書き出し -------------------------------------------------------
    auto r = FileSystem::WriteAllBytes(file_path,
                                       reinterpret_cast<const byte*>(buf),
                                       static_cast<usize>(pos));
    if (r.IsErr()) {
        ACS_LOG_WARN("FEditorTheme::SaveTheme: WriteAllBytes failed: os=%u",
                     r.Error().os_error);
    }
}

// =============================================================================
// LoadTheme — `.acstheme` テキスト読み込み
// -----------------------------------------------------------------------------
// 失敗時 (ファイル無し / magic mismatch / parse 失敗) は ACS_LOG_WARN + 現状
// 維持 (= 旧 theme を保持)。部分成功 (= 一部のキーだけ読めた) は許容し、未知
// キー / 解析失敗キーは単に黙ってスキップ (前方互換)。
// =============================================================================
void FEditorTheme::LoadTheme(const wchar_t* file_path) noexcept {
    if (file_path == nullptr) return;
    if (!FileSystem::Exists(file_path)) {
        ACS_LOG_WARN("FEditorTheme::LoadTheme: file not found");
        return;
    }

    auto rr = FileSystem::ReadAllText(file_path);
    if (rr.IsErr()) {
        ACS_LOG_WARN("FEditorTheme::LoadTheme: ReadAllText failed: os=%u",
                     rr.Error().os_error);
        return;
    }
    const TArray<char>& text = rr.Value();
    if (text.Size() == 0) {
        ACS_LOG_WARN("FEditorTheme::LoadTheme: empty file");
        return;
    }

    // ReadAllText は末尾 NUL 付きで返すので、char* として扱える前提。
    // 念のため最後の char が '\0' でなくても、Size() で範囲をクランプする。
    const char* p     = text.Data();
    const char* end   = p + text.Size();

    // ---- 行単位 parser --------------------------------------------------
    // 既存値を破壊しないよう一時バッファに読んだ結果を蓄積し、最後に
    // commit する (= magic mismatch 等で部分上書きされない)。
    EditorThemeColors  new_colors      = _colors;
    EEditorThemePreset new_preset      = _preset;
    f32                new_font_scale  = _font_scale;
    f32                new_corner      = _corner_radius;
    f32                new_spacing     = _item_spacing_y;
    bool               magic_ok        = false;

    char line[256];

    while (p < end) {
        // 1 行抜き出し (LF / CRLF / 末尾 NUL 終端)。
        usize len = 0;
        while (p < end && *p != '\n' && *p != '\0' && len + 1 < sizeof(line)) {
            // CR は飛ばす (Windows CRLF 互換)。
            if (*p != '\r') line[len++] = *p;
            ++p;
        }
        line[len] = '\0';
        // 行末まで読み進める (overflow した場合の残りバイトを捨てる)。
        while (p < end && *p != '\n' && *p != '\0') ++p;
        if (p < end && *p == '\n') ++p;

        // 空行 / コメント (# 始まり) スキップ。
        const char* q = line;
        while (*q == ' ' || *q == '\t') ++q;
        if (*q == '\0' || *q == '#') continue;

        // 最初の token を読む (空白までを key とする)。
        char key[32] = {};
        usize ki = 0;
        while (*q != '\0' && *q != ' ' && *q != '\t' && ki + 1 < sizeof(key)) {
            key[ki++] = *q++;
        }
        key[ki] = '\0';
        while (*q == ' ' || *q == '\t') ++q;

        // ---- magic ------------------------------------------------------
        // 初回行は `ACS_THEME <ver>` のはず。違うなら明示エラー。
        if (!magic_ok) {
            if (std::strcmp(key, kMagic) == 0) {
                const u32 v = static_cast<u32>(std::strtoul(q, nullptr, 10));
                if (v != kCurrentVersion) {
                    ACS_LOG_WARN("FEditorTheme::LoadTheme: unsupported version %u",
                                 v);
                    return;
                }
                magic_ok = true;
                continue;
            }
            // magic 行が来る前に他の key が来たらフォーマット不正で abort。
            ACS_LOG_WARN("FEditorTheme::LoadTheme: missing %s header", kMagic);
            return;
        }

        // ---- preset 名 --------------------------------------------------
        if (std::strcmp(key, "preset") == 0) {
            // `preset <name>` の name は空白なし ASCII 想定。
            char name[32] = {};
            usize ni = 0;
            while (*q != '\0' && *q != ' ' && *q != '\t' &&
                   *q != '\r' && *q != '\n' && ni + 1 < sizeof(name)) {
                name[ni++] = *q++;
            }
            name[ni] = '\0';
            new_preset = ToPreset(name);
            continue;
        }

        // ---- スカラー metric --------------------------------------------
        if (std::strcmp(key, "font_scale") == 0) {
            new_font_scale = std::strtof(q, nullptr);
            continue;
        }
        if (std::strcmp(key, "corner_radius") == 0) {
            new_corner = std::strtof(q, nullptr);
            continue;
        }
        if (std::strcmp(key, "item_spacing_y") == 0) {
            new_spacing = std::strtof(q, nullptr);
            continue;
        }

        // ---- カラー (FVec4) ----------------------------------------------
        // ヘルパで `x y z w` の 4 値を読む。失敗時は黙って skip。
        auto parse_vec4 = [&](FVec4& out_v) noexcept {
            char* tail = nullptr;
            const f32 x = std::strtof(q, &tail); if (tail == q) return;
            const f32 y = std::strtof(tail, &tail);
            const f32 z = std::strtof(tail, &tail);
            const f32 w = std::strtof(tail, &tail);
            out_v = FVec4{x, y, z, w};
        };
        if      (std::strcmp(key, "window_bg")     == 0) parse_vec4(new_colors.window_bg);
        else if (std::strcmp(key, "title_bg")      == 0) parse_vec4(new_colors.title_bg);
        else if (std::strcmp(key, "button_bg")     == 0) parse_vec4(new_colors.button_bg);
        else if (std::strcmp(key, "button_hover")  == 0) parse_vec4(new_colors.button_hover);
        else if (std::strcmp(key, "button_active") == 0) parse_vec4(new_colors.button_active);
        else if (std::strcmp(key, "frame_bg")      == 0) parse_vec4(new_colors.frame_bg);
        else if (std::strcmp(key, "text")          == 0) parse_vec4(new_colors.text);
        else if (std::strcmp(key, "text_disabled") == 0) parse_vec4(new_colors.text_disabled);
        else if (std::strcmp(key, "border")        == 0) parse_vec4(new_colors.border);
        else if (std::strcmp(key, "separator")     == 0) parse_vec4(new_colors.separator);
        else if (std::strcmp(key, "accent")        == 0) parse_vec4(new_colors.accent);
        else if (std::strcmp(key, "warning")       == 0) parse_vec4(new_colors.warning);
        else if (std::strcmp(key, "error")         == 0) parse_vec4(new_colors.error);
        // 未知 key は黙ってスキップ (前方互換)。
    }

    if (!magic_ok) {
        ACS_LOG_WARN("FEditorTheme::LoadTheme: no %s header found", kMagic);
        return;
    }

    // ---- commit (= 全部読めたら一括反映) ---------------------------------
    _colors          = new_colors;
    _preset          = new_preset;
    _font_scale      = new_font_scale;
    _corner_radius   = new_corner;
    _item_spacing_y  = new_spacing;
    ApplyToImGui();
}

} // namespace acs::game::editor_core
