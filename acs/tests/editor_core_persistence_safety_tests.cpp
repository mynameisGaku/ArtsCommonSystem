// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/tools/editor_core/EditorPanel.h"
#include "gameframework/tools/editor_core/EditorTheme.h"
#include "gameframework/tools/editor_core/EditorWorkspace.h"
#include "foundation/Platform.h"

#include <imgui.h>

#include <cstring>
#include <cwchar>

using namespace acs;
using namespace acs::game::editor_core;

namespace {

constexpr char kCanonicalTheme[] =
    "ACS_THEME 1\n"
    "preset Custom\n"
    "font_scale 1.25\n"
    "corner_radius 4\n"
    "item_spacing_y 6\n"
    "window_bg 0.10 0.20 0.30 1\n"
    "title_bg 0.11 0.21 0.31 1\n"
    "button_bg 0.12 0.22 0.32 1\n"
    "button_hover 0.13 0.23 0.33 1\n"
    "button_active 0.14 0.24 0.34 1\n"
    "frame_bg 0.15 0.25 0.35 1\n"
    "text 0.90 0.80 0.70 1\n"
    "text_disabled 0.60 0.50 0.40 1\n"
    "border 0.16 0.26 0.36 1\n"
    "separator 0.17 0.27 0.37 1\n"
    "accent 0.18 0.28 0.38 1\n"
    "warning 0.90 0.60 0.10 1\n"
    "error 0.90 0.10 0.20 1\n";

class FTestPanel final : public FEditorPanel {
public:
    explicit FTestPanel(const char* title = "PanelA") noexcept
        : m_Title(title) {}

    const char* Title() const noexcept override { return m_Title; }
    void DrawUI() noexcept override {}

private:
    const char* m_Title = nullptr;
};

class FImGuiContextScope {
public:
    FImGuiContextScope() noexcept
        : m_Previous(ImGui::GetCurrentContext()) {
        if (m_Previous == nullptr) {
            m_Owned = ImGui::CreateContext();
        }
    }

    ~FImGuiContextScope() noexcept {
        if (m_Owned != nullptr) {
            ImGui::DestroyContext(m_Owned);
        } else {
            ImGui::SetCurrentContext(m_Previous);
        }
    }

    bool IsValid() const noexcept {
        return ImGui::GetCurrentContext() != nullptr;
    }

private:
    ImGuiContext* m_Previous = nullptr;
    ImGuiContext* m_Owned = nullptr;
};

void MakeEditorCoreTempPath(
    wchar_t* output, usize capacity, const wchar_t* suffix) noexcept {
    wchar_t directory[512]{};
    const DWORD directory_length = ::GetTempPathW(
        static_cast<DWORD>(sizeof(directory) / sizeof(directory[0])),
        directory);
    EXPECT_TRUE(directory_length > 0u);
    static volatile LONG serial = 0;
    const LONG value = ::InterlockedIncrement(&serial);
    const int written = std::swprintf(
        output, capacity, L"%lsacs_editor_core_%lu_%ld_%ls",
        directory, static_cast<unsigned long>(::GetCurrentProcessId()),
        static_cast<long>(value), suffix);
    EXPECT_TRUE(written > 0);
}

bool WriteRawFile(
    const wchar_t* path, const char* bytes, u32 size) noexcept {
    HANDLE file = ::CreateFileW(
        path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0u;
    const BOOL write_ok = ::WriteFile(file, bytes, size, &written, nullptr);
    const BOOL flush_ok = ::FlushFileBuffers(file);
    const BOOL close_ok = ::CloseHandle(file);
    return write_ok && written == size && flush_ok && close_ok;
}

} // namespace

ACS_TEST(EditorCorePersistenceSafety, ThemeParsesCanonicalV1) {
    FEditorTheme theme;
    const FEditorThemePersistenceResult result =
        theme.TryParseThemeText(kCanonicalTheme, sizeof(kCanonicalTheme) - 1u);

    EXPECT_TRUE(result.Succeeded());
    EXPECT_EQ(theme.CurrentPreset(), EEditorThemePreset::Custom);
    EXPECT_NEAR(theme.FontScale(), 1.25f, 1e-6f);
    EXPECT_NEAR(theme.RoundedCorners(), 4.0f, 1e-6f);
    EXPECT_NEAR(theme.Spacing(), 6.0f, 1e-6f);
    EXPECT_NEAR(theme.Colors().window_bg.x, 0.10f, 1e-6f);
    EXPECT_NEAR(theme.Colors().accent.z, 0.38f, 1e-6f);
}

ACS_TEST(EditorCorePersistenceSafety, ThemeRejectsHostileTextTransactionally) {
    FEditorTheme theme;
    theme.ApplyPreset(EEditorThemePreset::Sepia);
    theme.SetFontScale(1.75f);
    theme.SetRoundedCorners(7.0f);
    theme.SetSpacing(9.0f);
    const FVec4 original_window = theme.Colors().window_bg;

    constexpr char duplicate[] =
        "ACS_THEME 1\npreset Dark\npreset Light\n";
    FEditorThemePersistenceResult result =
        theme.TryParseThemeText(duplicate, sizeof(duplicate) - 1u);
    EXPECT_EQ(result.error, EEditorThemePersistenceError::DuplicateKey);

    constexpr char non_finite[] =
        "ACS_THEME 1\npreset Dark\nfont_scale nan\n";
    result = theme.TryParseThemeText(non_finite, sizeof(non_finite) - 1u);
    EXPECT_TRUE(
        result.error == EEditorThemePersistenceError::InvalidValue ||
        result.error == EEditorThemePersistenceError::ValueOutOfRange);

    const char embedded_nul[] = {'A', 'C', 'S', '_', 'T', 'H', 'E', 'M', 'E',
                                 ' ', '1', '\n', '\0', 'x'};
    result = theme.TryParseThemeText(embedded_nul, sizeof(embedded_nul));
    EXPECT_EQ(result.error, EEditorThemePersistenceError::EmbeddedNul);

    EXPECT_EQ(theme.CurrentPreset(), EEditorThemePreset::Sepia);
    EXPECT_NEAR(theme.FontScale(), 1.75f, 1e-6f);
    EXPECT_NEAR(theme.RoundedCorners(), 7.0f, 1e-6f);
    EXPECT_NEAR(theme.Spacing(), 9.0f, 1e-6f);
    EXPECT_NEAR(theme.Colors().window_bg.x, original_window.x, 1e-6f);
}

ACS_TEST(EditorCorePersistenceSafety, WorkspaceCommitsOnlyAfterFullValidation) {
    FImGuiContextScope imgui;
    EXPECT_TRUE(imgui.IsValid());
    if (!imgui.IsValid()) return;

    FTestPanel panel;
    FEditorWorkspace workspace;
    workspace.RegisterPanel(&panel);
    panel.SetVisible(true);
    panel.SetDockTarget(false);

    constexpr char valid[] =
        "ACS_EDLAYOUT 1\n"
        "IMGUI_INI 0\n"
        "PANEL PanelA 0 1\n";
    FEditorWorkspacePersistenceResult result =
        workspace.TryParseLayoutText(valid, sizeof(valid) - 1u);
    EXPECT_TRUE(result.Succeeded());
    EXPECT_EQ(result.panel_entries, 1u);
    EXPECT_FALSE(panel.IsVisible());
    EXPECT_TRUE(panel.IsDockTarget());

    panel.SetVisible(true);
    panel.SetDockTarget(false);
    constexpr char duplicate[] =
        "ACS_EDLAYOUT 1\n"
        "IMGUI_INI 0\n"
        "PANEL PanelA 0 1\n"
        "PANEL PanelA 1 0\n";
    result = workspace.TryParseLayoutText(
        duplicate, sizeof(duplicate) - 1u);
    EXPECT_EQ(
        result.error, EEditorWorkspacePersistenceError::DuplicatePanel);
    EXPECT_TRUE(panel.IsVisible());
    EXPECT_FALSE(panel.IsDockTarget());

    constexpr char truncated[] =
        "ACS_EDLAYOUT 1\nIMGUI_INI 20\nshort";
    result = workspace.TryParseLayoutText(
        truncated, sizeof(truncated) - 1u);
    EXPECT_EQ(result.error, EEditorWorkspacePersistenceError::TruncatedIni);
    EXPECT_TRUE(panel.IsVisible());
    EXPECT_FALSE(panel.IsDockTarget());
}

ACS_TEST(EditorCorePersistenceSafety, WorkspaceTitleWithSpacesRoundTripsV1) {
    FImGuiContextScope imgui;
    EXPECT_TRUE(imgui.IsValid());
    if (!imgui.IsValid()) return;

    wchar_t path[768]{};
    MakeEditorCoreTempPath(
        path, sizeof(path) / sizeof(path[0]), L"layout.acslayout");

    FTestPanel panel("Panel With Spaces");
    FEditorWorkspace workspace;
    workspace.RegisterPanel(&panel);
    panel.SetVisible(false);
    panel.SetDockTarget(true);

    const FEditorWorkspacePersistenceResult save_result =
        workspace.TrySaveLayout(path);
    EXPECT_TRUE(save_result.Succeeded());
    EXPECT_EQ(save_result.panel_entries, 1u);

    // 保存後に反転し、file load が空白入り title で同じ panel を引けることを確認する。
    panel.SetVisible(true);
    panel.SetDockTarget(false);
    const FEditorWorkspacePersistenceResult load_result =
        workspace.TryLoadLayout(path);
    EXPECT_TRUE(load_result.Succeeded());
    EXPECT_EQ(load_result.panel_entries, 1u);
    EXPECT_FALSE(panel.IsVisible());
    EXPECT_TRUE(panel.IsDockTarget());

    workspace.Shutdown();
    EXPECT_TRUE(::DeleteFileW(path));
}

ACS_TEST(EditorCorePersistenceSafety, WorkspaceRejectsInvalidPanelTitles) {
    FImGuiContextScope imgui;
    EXPECT_TRUE(imgui.IsValid());
    if (!imgui.IsValid()) return;

    FTestPanel panel("Panel Name");
    FEditorWorkspace workspace;
    workspace.RegisterPanel(&panel);
    panel.SetVisible(true);
    panel.SetDockTarget(false);

    constexpr char leading_space[] =
        "ACS_EDLAYOUT 1\n"
        "IMGUI_INI 0\n"
        "PANEL  Panel Name 0 1\n";
    FEditorWorkspacePersistenceResult result =
        workspace.TryParseLayoutText(
            leading_space, sizeof(leading_space) - 1u);
    EXPECT_EQ(result.error, EEditorWorkspacePersistenceError::InvalidTitle);
    EXPECT_TRUE(panel.IsVisible());
    EXPECT_FALSE(panel.IsDockTarget());

    constexpr char trailing_space[] =
        "ACS_EDLAYOUT 1\n"
        "IMGUI_INI 0\n"
        "PANEL Panel Name  0 1\n";
    result = workspace.TryParseLayoutText(
        trailing_space, sizeof(trailing_space) - 1u);
    EXPECT_EQ(result.error, EEditorWorkspacePersistenceError::InvalidTitle);
    EXPECT_TRUE(panel.IsVisible());
    EXPECT_FALSE(panel.IsDockTarget());

    constexpr char control_character[] =
        "ACS_EDLAYOUT 1\n"
        "IMGUI_INI 0\n"
        "PANEL Panel" "\x1f" "Name 0 1\n";
    result = workspace.TryParseLayoutText(
        control_character, sizeof(control_character) - 1u);
    EXPECT_EQ(result.error, EEditorWorkspacePersistenceError::InvalidTitle);
    EXPECT_TRUE(panel.IsVisible());
    EXPECT_FALSE(panel.IsDockTarget());

    constexpr char non_ascii[] =
        "ACS_EDLAYOUT 1\n"
        "IMGUI_INI 0\n"
        "PANEL Panel" "\xc3\xa9" "Name 0 1\n";
    result = workspace.TryParseLayoutText(
        non_ascii, sizeof(non_ascii) - 1u);
    EXPECT_EQ(result.error, EEditorWorkspacePersistenceError::InvalidTitle);
    EXPECT_TRUE(panel.IsVisible());
    EXPECT_FALSE(panel.IsDockTarget());

    constexpr char empty_title[] =
        "ACS_EDLAYOUT 1\n"
        "IMGUI_INI 0\n"
        "PANEL  0 1\n";
    result = workspace.TryParseLayoutText(
        empty_title, sizeof(empty_title) - 1u);
    EXPECT_EQ(result.error, EEditorWorkspacePersistenceError::InvalidTitle);
    EXPECT_TRUE(panel.IsVisible());
    EXPECT_FALSE(panel.IsDockTarget());

    char too_long[256]{};
    constexpr char prefix[] =
        "ACS_EDLAYOUT 1\n"
        "IMGUI_INI 0\n"
        "PANEL ";
    constexpr char suffix[] = " 0 1\n";
    usize size = sizeof(prefix) - 1u;
    std::memcpy(too_long, prefix, size);
    for (usize i = 0u; i <= FEditorWorkspace::kMaxPanelTitleBytes; ++i) {
        too_long[size++] = 'A';
    }
    std::memcpy(too_long + size, suffix, sizeof(suffix) - 1u);
    size += sizeof(suffix) - 1u;
    result = workspace.TryParseLayoutText(too_long, size);
    EXPECT_EQ(result.error, EEditorWorkspacePersistenceError::TitleTooLong);
    EXPECT_TRUE(panel.IsVisible());
    EXPECT_FALSE(panel.IsDockTarget());

    // Save 側も同じ title 規約を使い、不正名では file を作る前に失敗する。
    wchar_t path[768]{};
    MakeEditorCoreTempPath(
        path, sizeof(path) / sizeof(path[0]), L"invalid_layout.acslayout");
    const char* invalid_titles[] = {
        "",
        " Leading",
        "Trailing ",
        "Control" "\x1f" "Name",
        "NonAscii" "\xc3\xa9",
    };
    for (const char* invalid_title : invalid_titles) {
        FTestPanel invalid_panel(invalid_title);
        FEditorWorkspace invalid_workspace;
        invalid_workspace.RegisterPanel(&invalid_panel);
        result = invalid_workspace.TrySaveLayout(path);
        EXPECT_EQ(
            result.error, EEditorWorkspacePersistenceError::InvalidTitle);
        invalid_workspace.Shutdown();
        (void)::DeleteFileW(path);
    }
}

ACS_TEST(EditorCorePersistenceSafety, ParserBoundsAreCheckedBeforeScanning) {
    FEditorTheme theme;
    FEditorWorkspace workspace;

    EXPECT_EQ(
        theme.TryParseThemeText(
                 "", FEditorTheme::kMaxThemeBytes + 1u).error,
        EEditorThemePersistenceError::InputTooLarge);
    EXPECT_EQ(
        workspace.TryParseLayoutText(
                     "", FEditorWorkspace::kMaxLayoutBytes + 1u).error,
        EEditorWorkspacePersistenceError::InputTooLarge);
    EXPECT_EQ(
        theme.TryParseThemeText(nullptr, 0u).error,
        EEditorThemePersistenceError::NullArgument);
    EXPECT_EQ(
        workspace.TryParseLayoutText(nullptr, 0u).error,
        EEditorWorkspacePersistenceError::NullArgument);
}

ACS_TEST(EditorCorePersistenceSafety, ThemeSaveIsAtomicForOpenReaders) {
    wchar_t path[768]{};
    MakeEditorCoreTempPath(
        path, sizeof(path) / sizeof(path[0]), L"theme.acstheme");
    constexpr char original[] = "OLD";
    EXPECT_TRUE(WriteRawFile(path, original, sizeof(original) - 1u));

    HANDLE old_reader = ::CreateFileW(
        path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_TRUE(old_reader != INVALID_HANDLE_VALUE);
    if (old_reader == INVALID_HANDLE_VALUE) {
        ::DeleteFileW(path);
        return;
    }

    FEditorTheme saved;
    saved.ApplyPreset(EEditorThemePreset::DarkBlue);
    saved.SetFontScale(1.5f);
    const FEditorThemePersistenceResult save_result =
        saved.TrySaveTheme(path);
    EXPECT_TRUE(save_result.Succeeded());

    char old_bytes[4]{};
    DWORD old_read = 0u;
    EXPECT_TRUE(::ReadFile(
        old_reader, old_bytes, sizeof(original) - 1u, &old_read, nullptr));
    EXPECT_EQ(old_read, static_cast<DWORD>(sizeof(original) - 1u));
    EXPECT_TRUE(std::memcmp(
        old_bytes, original, sizeof(original) - 1u) == 0);
    EXPECT_TRUE(::CloseHandle(old_reader));

    FEditorTheme loaded;
    const FEditorThemePersistenceResult load_result =
        loaded.TryLoadTheme(path);
    EXPECT_TRUE(load_result.Succeeded());
    EXPECT_EQ(loaded.CurrentPreset(), EEditorThemePreset::DarkBlue);
    EXPECT_NEAR(loaded.FontScale(), 1.5f, 1e-6f);
    EXPECT_TRUE(::DeleteFileW(path));
}
