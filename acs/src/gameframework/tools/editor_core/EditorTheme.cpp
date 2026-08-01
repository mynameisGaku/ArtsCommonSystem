// SPDX-License-Identifier: Apache-2.0
// GameFramework Tools — editor_core / CEditorTheme 実装
//
// 仕様詳細は CEditorTheme.h を参照。本ファイルでは:
//   ・preset 種別ごとの色テーブル定義 (Dark / DarkBlue / Light / HighContrast /
//     Sepia / Custom)
//   ・ACS::FVec4 → ImVec4 の橋渡し + ImGui::GetStyle() への適用
//   ・ImGui font_scale / corner_radius / spacing の流し込み
//   ・DrawThemeSettingsUI: preset combo + ColorEdit4 群 + Save/Load ボタン
//   ・SaveTheme / LoadTheme: `.acstheme` テキスト I/O (CFxeditSerializer と
//     同設計、1 行 1 key=value、magic + version、git diff フレンドリー)
// を実装する。全 noexcept、STL 不使用、ImGui 依存はこの .cpp に閉じる。
#include "gameframework/tools/editor_core/EditorTheme.h"

#include "container/Array.h"
#include "foundation/Log.h"
#include "foundation/Platform.h"

#include <imgui.h>

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>

namespace acs::game::editor_core {

namespace {

/**
 * FVec4 を ImVec4 に変換する。
 *
 * @details レイアウトは互換 (どちらも float x4) だが型は別物なので明示変換で混乱を避ける。
 * @param v 変換元の ACS カラー。
 * @return 対応する ImVec4。
 */
ImVec4 ToImVec4(const FVec4& v) noexcept {
    return ImVec4(v.x, v.y, v.z, v.w);
}

/**
 * ImVec4 を FVec4 に変換する。
 *
 * @details ImGui::ColorEdit が出した値を受け取る経路で使う逆変換。
 * @param v 変換元の ImVec4。
 * @return 対応する ACS カラー。
 */
FVec4 FromImVec4(const ImVec4& v) noexcept {
    return FVec4{v.x, v.y, v.z, v.w};
}

/**
 * preset を `.acstheme` テキスト用の名前文字列に変換する。
 *
 * @details LoadTheme の parser でも ToPreset で逆引きする。Custom も読み書きに対応。
 * @param p 名前を得たい preset。
 * @return preset 名 (未知値は "Dark")。
 */
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

/**
 * preset 名文字列を EEditorThemePreset に逆引きする。
 *
 * @details strcmp で完全一致のみ受け入れ、一致しなければ Dark を返す (安全側 fallback)。
 * @param name preset 名 (nullptr 可)。
 * @return 一致した preset (不一致 / nullptr は Dark)。
 */
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

/**
 * ImGui context が存在するかを返す。
 *
 * @details Init 前 / context 破棄後の多重呼び出しから守るため各 Apply* 系の冒頭で確認する。
 * @return ImGui context が生きていれば true。
 */
bool HasImGuiContext() noexcept {
    return ImGui::GetCurrentContext() != nullptr;
}

bool IsBoundedWidePath(
    const wchar_t* path, usize max_chars, usize& out_length) noexcept {
    out_length = 0u;
    if (path == nullptr) return false;
    while (out_length <= max_chars && path[out_length] != L'\0') ++out_length;
    return out_length > 0u && out_length <= max_chars;
}

bool BuildUniqueTempPath(
    const wchar_t* destination, usize destination_length,
    wchar_t* out, usize capacity, u32 attempt) noexcept {
    wchar_t suffix[96]{};
    static volatile LONG counter = 0;
    const LONG serial = ::InterlockedIncrement(&counter);
    const int suffix_length = std::swprintf(
        suffix, sizeof(suffix) / sizeof(suffix[0]),
        L".tmp.%lu.%lu.%ld.%u",
        static_cast<unsigned long>(::GetCurrentProcessId()),
        static_cast<unsigned long>(::GetCurrentThreadId()),
        static_cast<long>(serial), attempt);
    if (suffix_length <= 0) return false;
    const usize suffix_size = static_cast<usize>(suffix_length);
    if (destination_length + suffix_size + 1u > capacity) return false;
    std::memcpy(out, destination, destination_length * sizeof(wchar_t));
    std::memcpy(
        out + destination_length, suffix,
        (suffix_size + 1u) * sizeof(wchar_t));
    return true;
}

struct FFileRenameInfoEx {
    DWORD flags = 0u;
    HANDLE root_directory = nullptr;
    DWORD file_name_length = 0u;
    wchar_t file_name[1]{};
};

bool TryPosixAtomicReplace(
    const wchar_t* temporary_path,
    const wchar_t* destination,
    usize destination_length,
    DWORD& out_error) noexcept {
    constexpr DWORD kRenameReplaceIfExists = 0x00000001u;
    constexpr DWORD kRenamePosixSemantics = 0x00000002u;
    constexpr auto kFileRenameInfoEx =
        static_cast<FILE_INFO_BY_HANDLE_CLASS>(22);
    constexpr usize kPrefixBytes = offsetof(FFileRenameInfoEx, file_name);
    alignas(FFileRenameInfoEx)
        u8 storage[kPrefixBytes +
                   (CEditorTheme::kMaxPersistencePathChars + 1u) *
                       sizeof(wchar_t)]{};
    auto* info = reinterpret_cast<FFileRenameInfoEx*>(storage);
    const usize destination_bytes = destination_length * sizeof(wchar_t);
    info->flags = kRenameReplaceIfExists | kRenamePosixSemantics;
    info->file_name_length = static_cast<DWORD>(destination_bytes);
    std::memcpy(info->file_name, destination, destination_bytes);

    HANDLE source = ::CreateFileW(
        temporary_path, DELETE | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (source == INVALID_HANDLE_VALUE) {
        out_error = ::GetLastError();
        return false;
    }
    const DWORD info_bytes =
        static_cast<DWORD>(kPrefixBytes + destination_bytes);
    const BOOL renamed = ::SetFileInformationByHandle(
        source, kFileRenameInfoEx, info, info_bytes);
    if (!renamed) out_error = ::GetLastError();
    (void)::CloseHandle(source);
    return renamed != 0;
}

bool AppendBytes(TArray<char>& out, const char* text, usize length) noexcept {
    const usize old_size = out.Size();
    if (length > std::numeric_limits<usize>::max() - old_size ||
        !out.TryResize(old_size + length)) {
        return false;
    }
    if (length > 0u) std::memcpy(out.Data() + old_size, text, length);
    return true;
}

bool AppendFloatLine(
    TArray<char>& out, const char* key, const f32* values, u32 count) noexcept {
    if (key == nullptr || values == nullptr || count == 0u || count > 4u) {
        return false;
    }
    char line[160]{};
    const usize key_length = std::strlen(key);
    if (key_length + 2u >= sizeof(line)) return false;
    std::memcpy(line, key, key_length);
    usize length = key_length;
    for (u32 i = 0u; i < count; ++i) {
        if (!std::isfinite(values[i]) || length + 2u >= sizeof(line)) return false;
        line[length++] = ' ';
        const std::to_chars_result converted = std::to_chars(
            line + length, line + sizeof(line) - 1u, values[i],
            std::chars_format::general, std::numeric_limits<f32>::max_digits10);
        if (converted.ec != std::errc{}) return false;
        length = static_cast<usize>(converted.ptr - line);
    }
    line[length++] = '\n';
    return AppendBytes(out, line, length);
}

struct FToken {
    const char* begin = nullptr;
    const char* end = nullptr;
};

bool NextToken(const char*& cursor, const char* end, FToken& out) noexcept {
    while (cursor < end && (*cursor == ' ' || *cursor == '\t')) ++cursor;
    if (cursor == end) return false;
    out.begin = cursor;
    while (cursor < end && *cursor != ' ' && *cursor != '\t') ++cursor;
    out.end = cursor;
    return true;
}

bool TokenEquals(const FToken& token, const char* literal) noexcept {
    const usize literal_length = std::strlen(literal);
    return static_cast<usize>(token.end - token.begin) == literal_length &&
        std::memcmp(token.begin, literal, literal_length) == 0;
}

enum class EThemeNumberStatus : u8 { Ok, Invalid, OutOfRange };

EThemeNumberStatus ParseFloat(const FToken& token, f32& out) noexcept {
    const char* begin = token.begin;
    if (begin < token.end && *begin == '+') ++begin;
    if (begin == token.end) return EThemeNumberStatus::Invalid;
    f32 value = 0.0f;
    const std::from_chars_result converted = std::from_chars(
        begin, token.end, value, std::chars_format::general);
    if (converted.ec == std::errc::result_out_of_range) {
        return EThemeNumberStatus::OutOfRange;
    }
    if (converted.ec != std::errc{} || converted.ptr != token.end) {
        return EThemeNumberStatus::Invalid;
    }
    if (!std::isfinite(value)) return EThemeNumberStatus::OutOfRange;
    out = value;
    return EThemeNumberStatus::Ok;
}

bool ParsePreset(const FToken& token, EEditorThemePreset& out) noexcept {
    constexpr const char* names[] = {
        "Dark", "DarkBlue", "Light", "HighContrast", "Sepia", "Custom"};
    for (u32 i = 0u; i < 6u; ++i) {
        if (TokenEquals(token, names[i])) {
            out = static_cast<EEditorThemePreset>(i);
            return true;
        }
    }
    return false;
}

bool IsUnitColor(const FVec4& value) noexcept {
    const f32 components[4] = {value.x, value.y, value.z, value.w};
    for (f32 component : components) {
        if (!std::isfinite(component) || component < 0.0f || component > 1.0f) {
            return false;
        }
    }
    return true;
}

} // namespace

/**
 * preset 種別ごとの既定カラーパレットを out に書き込む。
 *
 * @details
 * 各 preset の RGBA [0,1] カラーを設定する (コメントの hex は人間用の参考表記)。
 * Dark は中間グレー + 暖色オレンジ accent、DarkBlue は青み grey + VSCode blue、
 * Light は白基調、HighContrast は黒/白/黄の WCAG AAA 想定、Sepia は茶系 e-reader 風。
 * Custom は SetCustomColors が直接書き込むため out を変更しない。
 * @param preset カラーを得たい preset 種別。
 * @param out 書き込み先のカラーパレット (Custom では不変)。
 */
void CEditorTheme::FillPresetColors(EEditorThemePreset       preset,
                                   FEditorThemeColors&       out) noexcept {
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

/** 既定 (Dark preset + 標準 metric) で初期化し ImGui に流す。 */
void CEditorTheme::Init() noexcept {
    m_Preset         = EEditorThemePreset::Dark;
    m_FontScale     = 1.0f;
    m_CornerRadius  = 3.0f;
    m_ItemSpacingY = 4.0f;
    FillPresetColors(m_Preset, m_Colors);
    ApplyToImGui();
}

/** preset を適用する (Custom 以外は既定色で m_Colors を上書きしてから ImGui に流す)。 */
void CEditorTheme::ApplyPreset(EEditorThemePreset preset) noexcept {
    m_Preset = preset;
    // Custom 以外なら m_Colors を上書き。Custom は SetCustomColors 経由で
    // 設定された値を保持したまま現値を再適用する (= preset 切替で Custom に
    // 戻すと、直前の Custom パレットが復活する設計)。
    if (preset != EEditorThemePreset::Custom) {
        FillPresetColors(preset, m_Colors);
    }
    ApplyToImGui();
}

/** 任意のカラーパレットを設定して preset を Custom に切り替え ImGui に流す。 */
void CEditorTheme::SetCustomColors(const FEditorThemeColors& colors) noexcept {
    m_Colors = colors;
    m_Preset = EEditorThemePreset::Custom;
    ApplyToImGui();
}

/** global font scale を設定する (<=0 は無視、上限 4.0 で clamp、ImGui IO に反映)。 */
void CEditorTheme::SetFontScale(f32 scale) noexcept {
    // <= 0 は無視 (= no-op)。0 倍は ImGui を壊す。
    if (scale <= 0.0f) return;
    // 上限 4.0 で safety clamp (典型用途は 1.0 〜 2.0)。
    if (scale > 4.0f) {
        ACS_LOG_WARN("FEditorTheme::SetFontScale: clamp %.2f -> 4.0",
                     static_cast<double>(scale));
        scale = 4.0f;
    }
    m_FontScale = scale;
    if (HasImGuiContext()) {
        ImGui::GetIO().FontGlobalScale = m_FontScale;
    }
}

/** 全 ImGui corner radius (window/frame/popup/grab/tab/scrollbar/child) を統一する。 */
void CEditorTheme::SetRoundedCorners(f32 radius) noexcept {
    if (radius < 0.0f) radius = 0.0f;
    m_CornerRadius = radius;
    if (HasImGuiContext()) {
        ImGuiStyle& s     = ImGui::GetStyle();
        s.WindowRounding    = m_CornerRadius;
        s.FrameRounding     = m_CornerRadius;
        s.PopupRounding     = m_CornerRadius;
        s.GrabRounding      = m_CornerRadius;
        s.TabRounding       = m_CornerRadius;
        s.ScrollbarRounding = m_CornerRadius;
        s.ChildRounding     = m_CornerRadius;
    }
}

/** ItemSpacing.y を設定する (x は y の 0.5 倍に連動、情報密度の主軸)。 */
void CEditorTheme::SetSpacing(f32 item_spacing_y) noexcept {
    if (item_spacing_y < 0.0f) item_spacing_y = 0.0f;
    m_ItemSpacingY = item_spacing_y;
    if (HasImGuiContext()) {
        ImGuiStyle& s = ImGui::GetStyle();
        // x は y の 0.5 倍 (見た目バランスのための経験則: 横は詰め気味)。
        s.ItemSpacing = ImVec2(m_ItemSpacingY * 0.5f, m_ItemSpacingY);
    }
}

/** m_Colors と各 metric (corner/spacing/font) を ImGui::GetStyle() / IO に流し込む。 */
void CEditorTheme::ApplyToImGui() noexcept {
    if (!HasImGuiContext()) return;

    ImGuiStyle& s    = ImGui::GetStyle();
    ImVec4*     col  = s.Colors;

    // 基本背景 / フレーム。
    col[ImGuiCol_WindowBg]              = ToImVec4(m_Colors.window_bg);
    col[ImGuiCol_ChildBg]               = ToImVec4(m_Colors.window_bg);
    col[ImGuiCol_PopupBg]               = ToImVec4(m_Colors.window_bg);
    col[ImGuiCol_FrameBg]               = ToImVec4(m_Colors.frame_bg);
    col[ImGuiCol_FrameBgHovered]        = ToImVec4(m_Colors.button_hover);
    col[ImGuiCol_FrameBgActive]         = ToImVec4(m_Colors.button_active);

    // タイトルバー。
    col[ImGuiCol_TitleBg]               = ToImVec4(m_Colors.title_bg);
    col[ImGuiCol_TitleBgActive]         = ToImVec4(m_Colors.title_bg);
    col[ImGuiCol_TitleBgCollapsed]      = ToImVec4(m_Colors.title_bg);
    col[ImGuiCol_MenuBarBg]             = ToImVec4(m_Colors.title_bg);

    // ボタン。
    col[ImGuiCol_Button]                = ToImVec4(m_Colors.button_bg);
    col[ImGuiCol_ButtonHovered]         = ToImVec4(m_Colors.button_hover);
    col[ImGuiCol_ButtonActive]          = ToImVec4(m_Colors.button_active);

    // ヘッダ (CollapsingHeader / Selectable hover 等)。
    col[ImGuiCol_Header]                = ToImVec4(m_Colors.button_bg);
    col[ImGuiCol_HeaderHovered]         = ToImVec4(m_Colors.button_hover);
    col[ImGuiCol_HeaderActive]          = ToImVec4(m_Colors.button_active);

    // 文字。
    col[ImGuiCol_Text]                  = ToImVec4(m_Colors.text);
    col[ImGuiCol_TextDisabled]          = ToImVec4(m_Colors.text_disabled);

    // 枠 / 区切り。
    col[ImGuiCol_Border]                = ToImVec4(m_Colors.border);
    col[ImGuiCol_BorderShadow]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    col[ImGuiCol_Separator]             = ToImVec4(m_Colors.separator);
    col[ImGuiCol_SeparatorHovered]      = ToImVec4(m_Colors.accent);
    col[ImGuiCol_SeparatorActive]       = ToImVec4(m_Colors.accent);

    // accent (CheckMark / Slider / Tab / Resize)。
    col[ImGuiCol_CheckMark]             = ToImVec4(m_Colors.accent);
    col[ImGuiCol_SliderGrab]            = ToImVec4(m_Colors.accent);
    col[ImGuiCol_SliderGrabActive]      = ToImVec4(m_Colors.accent);
    col[ImGuiCol_ResizeGrip]            = ToImVec4(m_Colors.accent);
    col[ImGuiCol_ResizeGripHovered]     = ToImVec4(m_Colors.accent);
    col[ImGuiCol_ResizeGripActive]      = ToImVec4(m_Colors.accent);
    col[ImGuiCol_Tab]                   = ToImVec4(m_Colors.button_bg);
    col[ImGuiCol_TabHovered]            = ToImVec4(m_Colors.accent);
    col[ImGuiCol_TabActive]             = ToImVec4(m_Colors.accent);
    col[ImGuiCol_TabUnfocused]          = ToImVec4(m_Colors.button_bg);
    col[ImGuiCol_TabUnfocusedActive]    = ToImVec4(m_Colors.button_active);

    // スクロールバー。
    col[ImGuiCol_ScrollbarBg]           = ToImVec4(m_Colors.frame_bg);
    col[ImGuiCol_ScrollbarGrab]         = ToImVec4(m_Colors.button_bg);
    col[ImGuiCol_ScrollbarGrabHovered]  = ToImVec4(m_Colors.button_hover);
    col[ImGuiCol_ScrollbarGrabActive]   = ToImVec4(m_Colors.button_active);

    // corner / spacing / font。
    s.WindowRounding    = m_CornerRadius;
    s.FrameRounding     = m_CornerRadius;
    s.PopupRounding     = m_CornerRadius;
    s.GrabRounding      = m_CornerRadius;
    s.TabRounding       = m_CornerRadius;
    s.ScrollbarRounding = m_CornerRadius;
    s.ChildRounding     = m_CornerRadius;

    // x は y の 0.5 倍 (見た目バランス)。
    s.ItemSpacing = ImVec2(m_ItemSpacingY * 0.5f, m_ItemSpacingY);

    ImGui::GetIO().FontGlobalScale = m_FontScale;
}

/** Theme 設定ウィンドウを描画する (preset combo + ColorEdit4 群 + metric slider + Save/Load)。 */
void CEditorTheme::DrawThemeSettingsUI() noexcept {
    if (!HasImGuiContext()) return;

    if (!ImGui::Begin("Theme Settings")) {
        ImGui::End();
        return;
    }

    // preset combo。
    const char* items[] = {
        "Dark", "DarkBlue", "Light", "HighContrast", "Sepia", "Custom",
    };
    int current = static_cast<int>(m_Preset);
    if (ImGui::Combo("Preset", &current, items, IM_ARRAYSIZE(items))) {
        ApplyPreset(static_cast<EEditorThemePreset>(current));
    }

    ImGui::Separator();

    // カラー編集。ColorEdit4 で値を drag するたびに ApplyToImGui を呼び、Custom に切替。
    bool changed = false;
    auto edit = [&](const char* label, FVec4& v) {
        ImVec4 tmp = ToImVec4(v);
        if (ImGui::ColorEdit4(label, &tmp.x)) {
            v = FromImVec4(tmp);
            changed = true;
        }
    };

    edit("Window BG",      m_Colors.window_bg);
    edit("Title BG",      m_Colors.title_bg);
    edit("Button BG",     m_Colors.button_bg);
    edit("Button Hover",  m_Colors.button_hover);
    edit("Button Active", m_Colors.button_active);
    edit("Frame BG",      m_Colors.frame_bg);
    edit("Text",          m_Colors.text);
    edit("Text Disabled", m_Colors.text_disabled);
    edit("Border",        m_Colors.border);
    edit("Separator",     m_Colors.separator);
    edit("Accent",        m_Colors.accent);
    edit("Warning",       m_Colors.warning);
    edit("Error",         m_Colors.error);

    if (changed) {
        m_Preset = EEditorThemePreset::Custom;
        ApplyToImGui();
    }

    ImGui::Separator();

    // metric (font / corner / spacing)。
    f32 font_scale = m_FontScale;
    if (ImGui::SliderFloat("Font Scale", &font_scale, 0.5f, 3.0f, "%.2fx")) {
        SetFontScale(font_scale);
    }
    f32 corner = m_CornerRadius;
    if (ImGui::SliderFloat("Corner Radius", &corner, 0.0f, 16.0f, "%.1fpx")) {
        SetRoundedCorners(corner);
    }
    f32 spacing = m_ItemSpacingY;
    if (ImGui::SliderFloat("Item Spacing Y", &spacing, 0.0f, 16.0f, "%.1fpx")) {
        SetSpacing(spacing);
    }

    ImGui::Separator();

    // Save / Load。ファイルダイアログが無いので固定パスを使う。
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

/**
 * 現在の theme を `.acstheme` テキストファイルに書き出す。
 *
 * @details
 * 1 行 1 key=value 形式 (magic + version、preset、font_scale / corner_radius /
 * item_spacing_y の metric 3 行、全 13 カラーの順) で書く。バッファ overflow や
 * 書き込み失敗時は ACS_LOG_WARN で記録し打ち切る (戻り値 void = ベストエフォート)。
 * @param file_path 書き出し先のファイルパス (nullptr は no-op)。
 */
#if 0
void CEditorTheme::SaveTheme(const wchar_t* file_path) noexcept {
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

    // ヘッダ + metric。
    if (!write_line("%s %u\n", kMagic, kCurrentVersion) ||
        !write_line("preset %s\n", PresetName(m_Preset)) ||
        !write_line("font_scale %.3f\n",      static_cast<double>(m_FontScale)) ||
        !write_line("corner_radius %.3f\n",   static_cast<double>(m_CornerRadius)) ||
        !write_line("item_spacing_y %.3f\n",  static_cast<double>(m_ItemSpacingY))) {
        ACS_LOG_WARN("FEditorTheme::SaveTheme: header buffer overflow");
        return;
    }

    // カラー 13 件。
    if (!write_color("window_bg",     m_Colors.window_bg)     ||
        !write_color("title_bg",      m_Colors.title_bg)      ||
        !write_color("button_bg",     m_Colors.button_bg)     ||
        !write_color("button_hover",  m_Colors.button_hover)  ||
        !write_color("button_active", m_Colors.button_active) ||
        !write_color("frame_bg",      m_Colors.frame_bg)      ||
        !write_color("text",          m_Colors.text)          ||
        !write_color("text_disabled", m_Colors.text_disabled) ||
        !write_color("border",        m_Colors.border)        ||
        !write_color("separator",     m_Colors.separator)     ||
        !write_color("accent",        m_Colors.accent)        ||
        !write_color("warning",       m_Colors.warning)       ||
        !write_color("error",         m_Colors.error)) {
        ACS_LOG_WARN("FEditorTheme::SaveTheme: color buffer overflow");
        return;
    }

    // 書き出し。
    const auto r = FFileSystem::WriteAllBytes(file_path,
                                       reinterpret_cast<const byte*>(buf),
                                       static_cast<usize>(pos));
    if (r.IsErr()) {
        ACS_LOG_WARN("FEditorTheme::SaveTheme: WriteAllBytes failed: os=%u",
                     r.Error().os_error);
    }
}

/**
 * `.acstheme` テキストファイルを読み込んで theme に反映する。
 *
 * @details
 * 一時バッファに読み、magic + version 検証を通った後に一括 commit する (= 部分上書きを
 * 防ぐ)。ファイル無し / magic mismatch / 非対応 version では ACS_LOG_WARN + 現状維持。
 * 未知キー / 解析失敗キーは黙ってスキップする (前方互換)。
 * @param file_path 読み込み元のファイルパス (nullptr は no-op)。
 */
void CEditorTheme::LoadTheme(const wchar_t* file_path) noexcept {
    if (file_path == nullptr) return;
    if (!FFileSystem::Exists(file_path)) {
        ACS_LOG_WARN("FEditorTheme::LoadTheme: file not found");
        return;
    }

    const auto rr = FFileSystem::ReadAllText(file_path);
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

    // 行単位 parser。既存値を破壊しないよう一時バッファに読んだ結果を蓄積し、
    // 最後に commit する (= magic mismatch 等で部分上書きされない)。
    FEditorThemeColors  new_colors      = m_Colors;
    EEditorThemePreset new_preset      = m_Preset;
    f32                new_font_scale  = m_FontScale;
    f32                new_corner      = m_CornerRadius;
    f32                new_spacing     = m_ItemSpacingY;
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

        // magic。初回行は `ACS_THEME <ver>` のはず。違うなら明示エラー。
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

        // preset 名。
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

        // スカラー metric。
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

        // カラー (FVec4)。ヘルパで `x y z w` の 4 値を読む。失敗時は黙って skip。
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

    // commit (= 全部読めたら一括反映)。
    m_Colors          = new_colors;
    m_Preset          = new_preset;
    m_FontScale      = new_font_scale;
    m_CornerRadius   = new_corner;
    m_ItemSpacingY  = new_spacing;
    ApplyToImGui();
}
#endif

const char* FEditorThemePersistenceResult::ErrorName(
    EEditorThemePersistenceError error) noexcept {
    switch (error) {
        case EEditorThemePersistenceError::None: return "None";
        case EEditorThemePersistenceError::NullArgument: return "NullArgument";
        case EEditorThemePersistenceError::PathTooLong: return "PathTooLong";
        case EEditorThemePersistenceError::InputTooLarge: return "InputTooLarge";
        case EEditorThemePersistenceError::EmbeddedNul: return "EmbeddedNul";
        case EEditorThemePersistenceError::TooManyLines: return "TooManyLines";
        case EEditorThemePersistenceError::LineTooLong: return "LineTooLong";
        case EEditorThemePersistenceError::BadMagic: return "BadMagic";
        case EEditorThemePersistenceError::UnsupportedVersion: return "UnsupportedVersion";
        case EEditorThemePersistenceError::InvalidSyntax: return "InvalidSyntax";
        case EEditorThemePersistenceError::UnknownKey: return "UnknownKey";
        case EEditorThemePersistenceError::DuplicateKey: return "DuplicateKey";
        case EEditorThemePersistenceError::MissingKey: return "MissingKey";
        case EEditorThemePersistenceError::InvalidType: return "InvalidType";
        case EEditorThemePersistenceError::InvalidValue: return "InvalidValue";
        case EEditorThemePersistenceError::ValueOutOfRange: return "ValueOutOfRange";
        case EEditorThemePersistenceError::AllocationFailure: return "AllocationFailure";
        case EEditorThemePersistenceError::FileNotFound: return "FileNotFound";
        case EEditorThemePersistenceError::FileOpenFailed: return "FileOpenFailed";
        case EEditorThemePersistenceError::FileSizeFailed: return "FileSizeFailed";
        case EEditorThemePersistenceError::FileChanged: return "FileChanged";
        case EEditorThemePersistenceError::FileReadFailed: return "FileReadFailed";
        case EEditorThemePersistenceError::FileWriteFailed: return "FileWriteFailed";
        case EEditorThemePersistenceError::FileFlushFailed: return "FileFlushFailed";
        case EEditorThemePersistenceError::FileCloseFailed: return "FileCloseFailed";
        case EEditorThemePersistenceError::AtomicReplaceFailed: return "AtomicReplaceFailed";
    }
    return "Unknown";
}

FEditorThemePersistenceResult CEditorTheme::TryParseThemeText(
    const char* text, usize text_size) noexcept {
    FEditorThemePersistenceResult result{};
    result.bytes_processed = static_cast<u64>(text_size);
    if (text == nullptr) {
        result.error = EEditorThemePersistenceError::NullArgument;
        return result;
    }
    if (text_size > kMaxThemeBytes) {
        result.error = EEditorThemePersistenceError::InputTooLarge;
        return result;
    }
    if (std::memchr(text, '\0', text_size) != nullptr) {
        result.error = EEditorThemePersistenceError::EmbeddedNul;
        return result;
    }

    FEditorThemeColors staged_colors{};
    EEditorThemePreset staged_preset = EEditorThemePreset::Dark;
    f32 staged_font_scale = 1.0f;
    f32 staged_corner_radius = 0.0f;
    f32 staged_spacing = 0.0f;
    constexpr u32 kRequiredKeyCount = 17u;
    constexpr u32 kRequiredMask = (1u << kRequiredKeyCount) - 1u;
    u32 seen = 0u;
    bool saw_header = false;
    usize offset = 0u;
    u32 line_number = 0u;

    auto fail = [&](EEditorThemePersistenceError error) noexcept {
        result.error = error;
        result.line = line_number;
        return result;
    };

    while (offset < text_size) {
        if (++line_number > kMaxThemeLines) {
            return fail(EEditorThemePersistenceError::TooManyLines);
        }
        const usize line_begin_offset = offset;
        while (offset < text_size && text[offset] != '\n') ++offset;
        usize line_length = offset - line_begin_offset;
        if (offset < text_size) ++offset;
        if (line_length > 0u && text[line_begin_offset + line_length - 1u] == '\r') {
            --line_length;
        }
        if (line_length > kMaxThemeLineBytes) {
            return fail(EEditorThemePersistenceError::LineTooLong);
        }
        const char* cursor = text + line_begin_offset;
        const char* line_end = cursor + line_length;
        while (cursor < line_end && (*cursor == ' ' || *cursor == '\t')) ++cursor;
        if (cursor == line_end || *cursor == '#') continue;

        FToken key{};
        if (!NextToken(cursor, line_end, key)) {
            return fail(EEditorThemePersistenceError::InvalidSyntax);
        }
        if (!saw_header) {
            if (!TokenEquals(key, kMagic)) {
                return fail(EEditorThemePersistenceError::BadMagic);
            }
            FToken version_token{};
            if (!NextToken(cursor, line_end, version_token)) {
                return fail(EEditorThemePersistenceError::InvalidSyntax);
            }
            FToken trailing{};
            if (NextToken(cursor, line_end, trailing)) {
                return fail(EEditorThemePersistenceError::InvalidSyntax);
            }
            u32 version = 0u;
            const std::from_chars_result parsed = std::from_chars(
                version_token.begin, version_token.end, version, 10);
            if (parsed.ec != std::errc{} || parsed.ptr != version_token.end) {
                return fail(EEditorThemePersistenceError::InvalidSyntax);
            }
            if (version != kCurrentVersion) {
                return fail(EEditorThemePersistenceError::UnsupportedVersion);
            }
            saw_header = true;
            continue;
        }

        u32 key_index = kRequiredKeyCount;
        FVec4* vector_target = nullptr;
        if (TokenEquals(key, "preset")) key_index = 0u;
        else if (TokenEquals(key, "font_scale")) key_index = 1u;
        else if (TokenEquals(key, "corner_radius")) key_index = 2u;
        else if (TokenEquals(key, "item_spacing_y")) key_index = 3u;
        else if (TokenEquals(key, "window_bg")) {
            key_index = 4u; vector_target = &staged_colors.window_bg;
        } else if (TokenEquals(key, "title_bg")) {
            key_index = 5u; vector_target = &staged_colors.title_bg;
        } else if (TokenEquals(key, "button_bg")) {
            key_index = 6u; vector_target = &staged_colors.button_bg;
        } else if (TokenEquals(key, "button_hover")) {
            key_index = 7u; vector_target = &staged_colors.button_hover;
        } else if (TokenEquals(key, "button_active")) {
            key_index = 8u; vector_target = &staged_colors.button_active;
        } else if (TokenEquals(key, "frame_bg")) {
            key_index = 9u; vector_target = &staged_colors.frame_bg;
        } else if (TokenEquals(key, "text")) {
            key_index = 10u; vector_target = &staged_colors.text;
        } else if (TokenEquals(key, "text_disabled")) {
            key_index = 11u; vector_target = &staged_colors.text_disabled;
        } else if (TokenEquals(key, "border")) {
            key_index = 12u; vector_target = &staged_colors.border;
        } else if (TokenEquals(key, "separator")) {
            key_index = 13u; vector_target = &staged_colors.separator;
        } else if (TokenEquals(key, "accent")) {
            key_index = 14u; vector_target = &staged_colors.accent;
        } else if (TokenEquals(key, "warning")) {
            key_index = 15u; vector_target = &staged_colors.warning;
        } else if (TokenEquals(key, "error")) {
            key_index = 16u; vector_target = &staged_colors.error;
        } else {
            return fail(EEditorThemePersistenceError::UnknownKey);
        }

        const u32 key_bit = 1u << key_index;
        if ((seen & key_bit) != 0u) {
            return fail(EEditorThemePersistenceError::DuplicateKey);
        }
        seen |= key_bit;

        if (key_index == 0u) {
            FToken value{};
            FToken trailing{};
            if (!NextToken(cursor, line_end, value) ||
                NextToken(cursor, line_end, trailing)) {
                return fail(EEditorThemePersistenceError::InvalidSyntax);
            }
            if (!ParsePreset(value, staged_preset)) {
                return fail(EEditorThemePersistenceError::InvalidValue);
            }
            continue;
        }

        const u32 value_count = vector_target != nullptr ? 4u : 1u;
        f32 values[4]{};
        for (u32 i = 0u; i < value_count; ++i) {
            FToken value{};
            if (!NextToken(cursor, line_end, value)) {
                return fail(EEditorThemePersistenceError::InvalidSyntax);
            }
            const EThemeNumberStatus status = ParseFloat(value, values[i]);
            if (status == EThemeNumberStatus::Invalid) {
                return fail(EEditorThemePersistenceError::InvalidValue);
            }
            if (status == EThemeNumberStatus::OutOfRange) {
                return fail(EEditorThemePersistenceError::ValueOutOfRange);
            }
        }
        FToken trailing{};
        if (NextToken(cursor, line_end, trailing)) {
            return fail(EEditorThemePersistenceError::InvalidSyntax);
        }
        if (vector_target != nullptr) {
            *vector_target = FVec4{values[0], values[1], values[2], values[3]};
            if (!IsUnitColor(*vector_target)) {
                return fail(EEditorThemePersistenceError::ValueOutOfRange);
            }
        } else if (key_index == 1u) {
            if (values[0] < 0.25f || values[0] > 4.0f) {
                return fail(EEditorThemePersistenceError::ValueOutOfRange);
            }
            staged_font_scale = values[0];
        } else if (key_index == 2u) {
            if (values[0] < 0.0f || values[0] > 32.0f) {
                return fail(EEditorThemePersistenceError::ValueOutOfRange);
            }
            staged_corner_radius = values[0];
        } else {
            if (values[0] < 0.0f || values[0] > 64.0f) {
                return fail(EEditorThemePersistenceError::ValueOutOfRange);
            }
            staged_spacing = values[0];
        }
    }

    result.line = line_number;
    if (!saw_header) {
        result.error = EEditorThemePersistenceError::BadMagic;
        return result;
    }
    if (seen != kRequiredMask) {
        result.error = EEditorThemePersistenceError::MissingKey;
        return result;
    }
    m_Colors = staged_colors;
    m_Preset = staged_preset;
    m_FontScale = staged_font_scale;
    m_CornerRadius = staged_corner_radius;
    m_ItemSpacingY = staged_spacing;
    ApplyToImGui();
    return result;
}

FEditorThemePersistenceResult CEditorTheme::TryLoadTheme(
    const wchar_t* file_path) noexcept {
    FEditorThemePersistenceResult result{};
    usize path_length = 0u;
    if (file_path == nullptr) {
        result.error = EEditorThemePersistenceError::NullArgument;
        return result;
    }
    if (!IsBoundedWidePath(
            file_path, kMaxPersistencePathChars, path_length)) {
        result.error = EEditorThemePersistenceError::PathTooLong;
        return result;
    }
    HANDLE file = ::CreateFileW(
        file_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        result.os_error = ::GetLastError();
        result.error =
            result.os_error == ERROR_FILE_NOT_FOUND ||
                    result.os_error == ERROR_PATH_NOT_FOUND
                ? EEditorThemePersistenceError::FileNotFound
                : EEditorThemePersistenceError::FileOpenFailed;
        return result;
    }
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        result.os_error = ::GetLastError();
        (void)::CloseHandle(file);
        result.error = EEditorThemePersistenceError::FileSizeFailed;
        return result;
    }
    if (static_cast<u64>(size.QuadPart) > static_cast<u64>(kMaxThemeBytes)) {
        (void)::CloseHandle(file);
        result.error = EEditorThemePersistenceError::InputTooLarge;
        return result;
    }
    TArray<char> text;
    if (!text.TryResize(static_cast<usize>(size.QuadPart))) {
        (void)::CloseHandle(file);
        result.error = EEditorThemePersistenceError::AllocationFailure;
        return result;
    }
    usize total = 0u;
    while (total < text.Size()) {
        const usize remaining = text.Size() - total;
        const DWORD chunk = static_cast<DWORD>(
            remaining > 0x7ffff000u ? 0x7ffff000u : remaining);
        DWORD bytes_read = 0u;
        if (!::ReadFile(
                file, text.Data() + total, chunk, &bytes_read, nullptr) ||
            bytes_read == 0u) {
            result.os_error = ::GetLastError();
            (void)::CloseHandle(file);
            result.error = EEditorThemePersistenceError::FileReadFailed;
            result.bytes_processed = static_cast<u64>(total);
            return result;
        }
        total += bytes_read;
    }
    char probe = '\0';
    DWORD probe_read = 0u;
    if (!::ReadFile(file, &probe, 1u, &probe_read, nullptr)) {
        result.os_error = ::GetLastError();
        (void)::CloseHandle(file);
        result.error = EEditorThemePersistenceError::FileReadFailed;
        result.bytes_processed = static_cast<u64>(total);
        return result;
    }
    LARGE_INTEGER final_size{};
    if (probe_read != 0u) {
        (void)::CloseHandle(file);
        result.error = EEditorThemePersistenceError::FileChanged;
        result.bytes_processed = static_cast<u64>(total);
        return result;
    }
    if (!::GetFileSizeEx(file, &final_size)) {
        result.os_error = ::GetLastError();
        (void)::CloseHandle(file);
        result.error = EEditorThemePersistenceError::FileSizeFailed;
        return result;
    }
    if (final_size.QuadPart != size.QuadPart) {
        (void)::CloseHandle(file);
        result.error = EEditorThemePersistenceError::FileChanged;
        result.bytes_processed = static_cast<u64>(total);
        return result;
    }
    if (!::CloseHandle(file)) {
        result.os_error = ::GetLastError();
        result.error = EEditorThemePersistenceError::FileCloseFailed;
        return result;
    }
    result = TryParseThemeText(
        text.IsEmpty() ? "" : text.Data(), text.Size());
    result.bytes_processed = static_cast<u64>(total);
    return result;
}

FEditorThemePersistenceResult CEditorTheme::TrySaveTheme(
    const wchar_t* file_path) noexcept {
    FEditorThemePersistenceResult result{};
    usize path_length = 0u;
    if (file_path == nullptr) {
        result.error = EEditorThemePersistenceError::NullArgument;
        return result;
    }
    if (!IsBoundedWidePath(
            file_path, kMaxPersistencePathChars, path_length)) {
        result.error = EEditorThemePersistenceError::PathTooLong;
        return result;
    }
    if (static_cast<u8>(m_Preset) > static_cast<u8>(EEditorThemePreset::Custom) ||
        !std::isfinite(m_FontScale) || m_FontScale < 0.25f || m_FontScale > 4.0f ||
        !std::isfinite(m_CornerRadius) || m_CornerRadius < 0.0f ||
        m_CornerRadius > 32.0f ||
        !std::isfinite(m_ItemSpacingY) || m_ItemSpacingY < 0.0f ||
        m_ItemSpacingY > 64.0f) {
        result.error = EEditorThemePersistenceError::ValueOutOfRange;
        return result;
    }
    const FVec4* colors[] = {
        &m_Colors.window_bg, &m_Colors.title_bg, &m_Colors.button_bg,
        &m_Colors.button_hover, &m_Colors.button_active, &m_Colors.frame_bg,
        &m_Colors.text, &m_Colors.text_disabled, &m_Colors.border,
        &m_Colors.separator, &m_Colors.accent, &m_Colors.warning,
        &m_Colors.error};
    for (const FVec4* color : colors) {
        if (!IsUnitColor(*color)) {
            result.error = EEditorThemePersistenceError::ValueOutOfRange;
            return result;
        }
    }

    TArray<char> output;
    if (!output.TryReserve(2048u)) {
        result.error = EEditorThemePersistenceError::AllocationFailure;
        return result;
    }
    char header[96]{};
    int header_size = std::snprintf(
        header, sizeof(header), "%s %u\npreset %s\n",
        kMagic, kCurrentVersion, PresetName(m_Preset));
    if (header_size < 0 || static_cast<usize>(header_size) >= sizeof(header) ||
        !AppendBytes(output, header, static_cast<usize>(header_size)) ||
        !AppendFloatLine(output, "font_scale", &m_FontScale, 1u) ||
        !AppendFloatLine(output, "corner_radius", &m_CornerRadius, 1u) ||
        !AppendFloatLine(output, "item_spacing_y", &m_ItemSpacingY, 1u)) {
        result.error = EEditorThemePersistenceError::AllocationFailure;
        return result;
    }
    constexpr const char* color_keys[] = {
        "window_bg", "title_bg", "button_bg", "button_hover",
        "button_active", "frame_bg", "text", "text_disabled", "border",
        "separator", "accent", "warning", "error"};
    for (u32 i = 0u; i < 13u; ++i) {
        const f32 values[4] = {
            colors[i]->x, colors[i]->y, colors[i]->z, colors[i]->w};
        if (!AppendFloatLine(output, color_keys[i], values, 4u)) {
            result.error = EEditorThemePersistenceError::AllocationFailure;
            return result;
        }
    }
    if (output.Size() > kMaxThemeBytes) {
        result.error = EEditorThemePersistenceError::InputTooLarge;
        return result;
    }

    constexpr usize kTempPathCapacity = kMaxPersistencePathChars + 97u;
    wchar_t temp_path[kTempPathCapacity]{};
    HANDLE temp = INVALID_HANDLE_VALUE;
    for (u32 attempt = 0u; attempt < 8u; ++attempt) {
        if (!BuildUniqueTempPath(
                file_path, path_length, temp_path, kTempPathCapacity, attempt)) {
            result.error = EEditorThemePersistenceError::PathTooLong;
            return result;
        }
        temp = ::CreateFileW(
            temp_path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (temp != INVALID_HANDLE_VALUE) break;
        result.os_error = ::GetLastError();
        if (result.os_error != ERROR_FILE_EXISTS &&
            result.os_error != ERROR_ALREADY_EXISTS) {
            result.error = EEditorThemePersistenceError::FileOpenFailed;
            return result;
        }
    }
    if (temp == INVALID_HANDLE_VALUE) {
        result.error = EEditorThemePersistenceError::FileOpenFailed;
        return result;
    }
    usize total = 0u;
    while (total < output.Size()) {
        const usize remaining = output.Size() - total;
        const DWORD chunk = static_cast<DWORD>(
            remaining > 0x7ffff000u ? 0x7ffff000u : remaining);
        DWORD written = 0u;
        if (!::WriteFile(
                temp, output.Data() + total, chunk, &written, nullptr) ||
            written == 0u) {
            result.os_error = ::GetLastError();
            (void)::CloseHandle(temp);
            (void)::DeleteFileW(temp_path);
            result.error = EEditorThemePersistenceError::FileWriteFailed;
            result.bytes_processed = static_cast<u64>(total);
            return result;
        }
        total += written;
    }
    if (!::FlushFileBuffers(temp)) {
        result.os_error = ::GetLastError();
        (void)::CloseHandle(temp);
        (void)::DeleteFileW(temp_path);
        result.error = EEditorThemePersistenceError::FileFlushFailed;
        return result;
    }
    if (!::CloseHandle(temp)) {
        result.os_error = ::GetLastError();
        (void)::DeleteFileW(temp_path);
        result.error = EEditorThemePersistenceError::FileCloseFailed;
        return result;
    }
    if (!::MoveFileExW(
            temp_path, file_path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD move_error = ::GetLastError();
        DWORD posix_error = 0u;
        if (!TryPosixAtomicReplace(
                temp_path, file_path, path_length, posix_error)) {
            result.os_error = posix_error != 0u ? posix_error : move_error;
            (void)::DeleteFileW(temp_path);
            result.error = EEditorThemePersistenceError::AtomicReplaceFailed;
            return result;
        }
    }
    result.bytes_processed = static_cast<u64>(total);
    return result;
}

void CEditorTheme::SaveTheme(const wchar_t* file_path) noexcept {
    if (file_path == nullptr) return;
    const FEditorThemePersistenceResult result = TrySaveTheme(file_path);
    if (!result.Succeeded()) {
        ACS_LOG_WARN(
            "FEditorTheme::SaveTheme: %s (line=%u os=%u)",
            FEditorThemePersistenceResult::ErrorName(result.error),
            result.line, result.os_error);
    }
}

void CEditorTheme::LoadTheme(const wchar_t* file_path) noexcept {
    if (file_path == nullptr) return;
    const FEditorThemePersistenceResult result = TryLoadTheme(file_path);
    if (!result.Succeeded()) {
        ACS_LOG_WARN(
            "FEditorTheme::LoadTheme: %s (line=%u os=%u)",
            FEditorThemePersistenceResult::ErrorName(result.error),
            result.line, result.os_error);
    }
}

} // namespace acs::game::editor_core
