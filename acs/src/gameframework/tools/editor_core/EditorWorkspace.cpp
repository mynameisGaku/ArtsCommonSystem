// SPDX-License-Identifier: Apache-2.0
// GameFramework Tools — editor_core / CEditorWorkspace 実装
//
// 仕様の意図は CEditorWorkspace.h を参照。本ファイルでは:
//   ・panel 登録 / 解除 / 探索 (TArray<AEditorPanel*> ベース)
//   ・1 フレーム駆動 (OnFrameBegin → DockSpace → MenuBar → DrawUI)
//   ・ImGui DockSpaceOverViewport の生成
//   ・Window / Layout メニューの描画
//   ・`.acslayout` 形式 (テキスト: magic + ImGui ini + per-panel state) の save/load
//   ・CSelectionService 参照保管 + Broadcast の fan-out
// を実装する。全 noexcept、STL 不使用、ImGui 依存はこの .cpp に閉じる。

#include "gameframework/tools/editor_core/EditorWorkspace.h"

#include "gameframework/tools/editor_core/EditorPanel.h"
#include "foundation/Log.h"
#include "foundation/Platform.h"

#include <imgui.h>

#include <charconv>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>

// IMGUI_HAS_DOCK は docking branch (= ImGui の features/docking) のみで定義される。
// master branch を引いている ACS では未定義になる。DockSpaceOverViewport /
// ImGuiDockNodeFlags はこのマクロでガードする。docking 不在時は DrawDockSpace を
// 安全に no-op に倒し、各 panel は通常の float window として表示される
// (= editor として動作はするが dock 統合 UI は無し)。
// 注意: IMGUI_HAS_DOCK は imgui.h 内では未 #define なので、ここで自分側ガード用に
// ACS_EDITOR_HAS_IMGUI_DOCK にラップする (docking branch 採用時に upstream の
// マクロ名と衝突しないように)。
#ifndef IMGUI_HAS_DOCK
  #define ACS_EDITOR_HAS_IMGUI_DOCK 0
#else
  #define ACS_EDITOR_HAS_IMGUI_DOCK 1
#endif

namespace acs::game::editor_core {

namespace {

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
                   (CEditorWorkspace::kMaxPersistencePathChars + 1u) *
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

bool AppendBytes(TArray<char>& out, const char* data, usize length) noexcept {
    const usize old_size = out.Size();
    if (length > std::numeric_limits<usize>::max() - old_size ||
        !out.TryResize(old_size + length)) {
        return false;
    }
    if (length > 0u) std::memcpy(out.Data() + old_size, data, length);
    return true;
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
    const usize length = std::strlen(literal);
    return static_cast<usize>(token.end - token.begin) == length &&
        std::memcmp(token.begin, literal, length) == 0;
}

bool ParseUsizeToken(const FToken& token, usize& out) noexcept {
    u64 value = 0u;
    const std::from_chars_result parsed = std::from_chars(
        token.begin, token.end, value, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != token.end ||
        value > static_cast<u64>(std::numeric_limits<usize>::max())) {
        return false;
    }
    out = static_cast<usize>(value);
    return true;
}

/** panel title に許す byte 列かを検証する。内部の ASCII space だけを許可する。 */
bool IsValidPanelTitleBytes(const char* title, usize length) noexcept {
    if (title == nullptr || length == 0u ||
        length > CEditorWorkspace::kMaxPanelTitleBytes) {
        return false;
    }
    if (title[0] == ' ' || title[length - 1u] == ' ') return false;
    for (usize i = 0u; i < length; ++i) {
        const unsigned char c = static_cast<unsigned char>(title[i]);
        if (c < 0x20u || c > 0x7eu) return false;
    }
    return true;
}

bool IsValidPanelTitle(const char* title, usize& out_length) noexcept {
    out_length = 0u;
    if (title == nullptr) return false;
    while (out_length <= CEditorWorkspace::kMaxPanelTitleBytes &&
           title[out_length] != '\0') {
        ++out_length;
    }
    return IsValidPanelTitleBytes(title, out_length);
}

/** PANEL 行の title と末尾 2 flag の区切りに使える文字かを返す。 */
bool IsPanelFieldSeparator(char c) noexcept {
    return c == ' ' || c == '\t';
}

} // namespace

/**
 * panel ポインタが登録対象として安全かを判定する。
 *
 * @details
 * null と Title() == nullptr を弾く。Title() は ImGui::Begin の id / Find lookup key /
 * Layout シリアライズ key の 3 役を兼ねるため、ここで nullptr を必ず弾いて
 * 以降の処理を単純化する。
 * @param p 判定する panel。
 * @return 登録対象として安全なら true。
 */
static bool IsRegistrablePanel(const AEditorPanel* p) noexcept {
    if (p == nullptr) return false;
    if (p->Title() == nullptr) return false;
    return true;
}

/**
 * strcmp の null 安全版で 2 文字列が一致するかを返す。
 *
 * @details
 * 一方でも null なら不一致扱い (両 null も不一致)。Title() が nullptr の panel は
 * IsRegistrablePanel で弾かれている前提だが、FindPanelByTitle 引数 / Layout load
 * 文字列の null も等しく弾くため共通化する。
 * @param a 比較する文字列その 1。
 * @param b 比較する文字列その 2。
 * @return 両者が非 null で内容一致なら true。
 */
static bool StrEqual(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    return std::strcmp(a, b) == 0;
}

/** panel list / 参照 / フラグを default にリセットする。 */
void CEditorWorkspace::Init() noexcept {
    // 完全リセット: 登録 panel list を空に、参照 / フラグを default に。
    // 容量は保持 (Clear は size=0 にするだけ、capacity はそのまま)。
    m_Panels.Clear();

    m_SelectionService          = nullptr;

    m_NoDockingInCentralNode = false;
    m_EnableDockspace           = true;
    m_EnableMenuBar            = true;
}

/** 全 panel に OnShutdown を呼び、list と CSelectionService 参照を解放する。 */
void CEditorWorkspace::Shutdown() noexcept {
    // 登録済み panel に OnShutdown を呼び、list を空にする。
    // 呼び出し順は登録順 (= Init / Tick と同じ順序で逆順にしない)。
    const usize n = m_Panels.Size();
    for (usize i = 0; i < n; ++i) {
        AEditorPanel* p = m_Panels[i];
        if (p != nullptr) {
            p->OnShutdown();
        }
    }
    m_Panels.Clear();

    m_SelectionService = nullptr;
    // フラグ類は意図的にリセットしない (= Shutdown 後に再 Init で同じ host 設定を
    // 引き継げるよう)。完全 default に戻したい host は Init() を続けて呼ぶこと。
}

/** panel を末尾に追加し、登録後に OnInit を呼ぶ (二重登録 / 上限超過は弾く)。 */
void CEditorWorkspace::RegisterPanel(AEditorPanel* panel) noexcept {
    if (!IsRegistrablePanel(panel)) {
        // null / Title() == nullptr の panel は silent no-op。
        // ログを出すと panel コンストラクト直後のリテラル静的初期化前に呼ばれた
        // ような誤検知を煩く出してしまうため、無視に留める。
        return;
    }
    // 二重登録弾き (同一ポインタの重複は OnInit 二重呼出しを誘発するため不可)。
    if (FindPanelIndex(panel) != kInvalidIndex) {
        return;
    }
    // 上限到達 silent no-op (kMaxPanels はあくまで安全弁)。
    if (m_Panels.Size() >= static_cast<usize>(kMaxPanels)) {
        ACS_LOG_WARN("CEditorWorkspace::RegisterPanel: panel limit %u reached, ignoring '%s'",
                     static_cast<unsigned>(kMaxPanels),
                     panel->Title());
        return;
    }
    m_Panels.PushBack(panel);
    // OnInit はリスト登録 **後** に呼ぶ。これにより OnInit 内から
    // `Workspace()->PanelCount()` 等を呼んだ場合に自身も数に含まれる
    // (= 自己参照アクセスが破綻しない)。
    panel->OnInit(*this);
}

/** OnShutdown を呼んでから panel を順序保存削除 (shift) する。 */
void CEditorWorkspace::UnregisterPanel(AEditorPanel* panel) noexcept {
    if (panel == nullptr) return;
    const i32 idx = FindPanelIndex(panel);
    if (idx == kInvalidIndex) return;

    // OnShutdown はリストから外す **前** に呼ぶ。これにより OnShutdown 内から
    // 自身を再 Find しても見つかる状態を保つ (= panel 内 cleanup で workspace
    // 経由の API を呼んでも安全)。
    panel->OnShutdown();

    // 順序保存削除 (shift)。RemoveAtSwap は使わない (= UI 表示順を保ちたい)。
    // TArray に Erase API が無いため手書きシフト (= AParticleEditorPanel と同形)。
    const usize sel = static_cast<usize>(idx);
    for (usize i = sel + 1; i < m_Panels.Size(); ++i) {
        m_Panels[i - 1] = m_Panels[i];
    }
    m_Panels.PopBack();
}

/** 現在の登録 panel 数を返す。 */
u32 CEditorWorkspace::PanelCount() const noexcept {
    return static_cast<u32>(m_Panels.Size());
}

/** index 番目の panel を返す (範囲外は nullptr)。 */
AEditorPanel* CEditorWorkspace::GetPanelByIndex(u32 i) const noexcept {
    if (i >= static_cast<u32>(m_Panels.Size())) return nullptr;
    return m_Panels[static_cast<usize>(i)];
}

/** Title が strcmp 一致する panel を返す (なければ nullptr)。 */
AEditorPanel* CEditorWorkspace::FindPanelByTitle(const char* title) const noexcept {
    if (title == nullptr) return nullptr;
    const usize n = m_Panels.Size();
    for (usize i = 0; i < n; ++i) {
        AEditorPanel* p = m_Panels[i];
        if (p == nullptr) continue;
        if (StrEqual(p->Title(), title)) {
            return p;
        }
    }
    return nullptr;
}

/** Title 一致 panel の可視状態を反転する (未発見は no-op)。 */
void CEditorWorkspace::TogglePanelVisible(const char* title) noexcept {
    AEditorPanel* p = FindPanelByTitle(title);
    if (p == nullptr) return;
    p->SetVisible(!p->IsVisible());
}

/** OnFrameBegin → DockSpace → MenuBar → DrawUI の順で 1 フレーム駆動する。 */
void CEditorWorkspace::TickAllPanels(f32 dt) noexcept {
    // 1) OnFrameBegin: 非 visible panel もバックグラウンド処理 (非同期 I/O,
    //    polling, animation timer 等) を進める可能性があるため、visibility を
    //    問わず全 panel に呼ぶ。
    {
        const usize n = m_Panels.Size();
        for (usize i = 0; i < n; ++i) {
            AEditorPanel* p = m_Panels[i];
            if (p != nullptr) {
                p->OnFrameBegin(dt);
            }
        }
    }

    // 2) DockSpace: ImGui ウィンドウより前に central node を確保しておくと、
    //    panel 側で初回 ImGui::Begin した時点で自動 dock 候補に central node が
    //    含まれるようになる。
    if (m_EnableDockspace) {
        DrawDockSpace();
    }

    // 3) MenuBar (MainMenuBar 一段). DockSpace と並んで host 側で抑制できる。
    if (m_EnableMenuBar) {
        DrawMenuBar();
    }

    // 4) DrawUI: 各 panel に描画させる。visibility / ImGui::Begin / End は
    //    派生 panel 側の責務 (本 workspace は呼び出すだけ)。
    {
        const usize n = m_Panels.Size();
        for (usize i = 0; i < n; ++i) {
            AEditorPanel* p = m_Panels[i];
            if (p != nullptr) {
                p->DrawUI();
            }
        }
    }
}

/** main viewport に DockSpace を貼る (docking 非対応 ImGui では no-op)。 */
void CEditorWorkspace::DrawDockSpace() noexcept {
#if ACS_EDITOR_HAS_IMGUI_DOCK
    // ImGui::DockSpaceOverViewport は main viewport の client area 全体に
    // ID 0 / null viewport (= 自動でメインを選択) で DockSpace を貼る。
    // 同 ID で多重呼び出ししても ImGui 側で no-op になるため、TickAllPanels
    // 自動呼出しと host 側手動呼出しの共存は安全。
    ImGuiDockNodeFlags flags = ImGuiDockNodeFlags_PassthruCentralNode;
    if (m_NoDockingInCentralNode) {
        flags |= ImGuiDockNodeFlags_NoDockingInCentralNode;
    }
    // 0 を渡すと ImGui がメインビューポートを選択 (NULL viewport 相当)。
    // window class は default (= 全 panel が共通 dock 候補)。
    ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), flags);
#else
    // docking 非対応 ImGui (master branch) — DockSpace は描画できないため
    // no-op。各 panel は通常の float ImGui window として並ぶ。`m_NoDockingInCentralNode`
    // の値は参照しないが、API シグネチャは docking branch と共通に保つ。
    // ImGui を docking branch に切替えた際に自動有効化される。
#endif
}

/** MainMenuBar に Window (panel toggle) / Layout (save / load) メニューを描画する。 */
void CEditorWorkspace::DrawMenuBar() noexcept {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    // Window メニュー (panel toggle)
    if (ImGui::BeginMenu("Window")) {
        const usize n = m_Panels.Size();
        if (n == 0) {
            ImGui::TextDisabled("(no panels registered)");
        } else {
            for (usize i = 0; i < n; ++i) {
                AEditorPanel* p = m_Panels[i];
                if (p == nullptr) continue;
                const char* title = p->Title();
                if (title == nullptr) continue;
                bool v = p->IsVisible();
                // ImGui::MenuItem に bool* を渡すとチェック表示 + 自動トグル。
                if (ImGui::MenuItem(title, nullptr, &v)) {
                    p->SetVisible(v);
                }
            }
        }
        ImGui::EndMenu();
    }

    // Layout メニュー (save / load default)
    // ファイルダイアログは依存追加を避けるため、固定パスのみ。
    // host 側で「最後のレイアウト」を別パスで保存したい場合は SaveLayout を
    // 直接呼ぶこと (= メニューはあくまで簡易アクセス点)。
    if (ImGui::BeginMenu("Layout")) {
        if (ImGui::MenuItem("Save Default")) {
            SaveLayout(L"editor_default.acslayout");
        }
        if (ImGui::MenuItem("Load Default")) {
            LoadLayout(L"editor_default.acslayout");
        }
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

const char* FEditorWorkspacePersistenceResult::ErrorName(
    EEditorWorkspacePersistenceError error) noexcept {
    switch (error) {
        case EEditorWorkspacePersistenceError::None: return "None";
        case EEditorWorkspacePersistenceError::NullArgument: return "NullArgument";
        case EEditorWorkspacePersistenceError::PathTooLong: return "PathTooLong";
        case EEditorWorkspacePersistenceError::InputTooLarge: return "InputTooLarge";
        case EEditorWorkspacePersistenceError::EmbeddedNul: return "EmbeddedNul";
        case EEditorWorkspacePersistenceError::TooManyLines: return "TooManyLines";
        case EEditorWorkspacePersistenceError::LineTooLong: return "LineTooLong";
        case EEditorWorkspacePersistenceError::BadMagic: return "BadMagic";
        case EEditorWorkspacePersistenceError::UnsupportedVersion: return "UnsupportedVersion";
        case EEditorWorkspacePersistenceError::InvalidSyntax: return "InvalidSyntax";
        case EEditorWorkspacePersistenceError::DuplicateSection: return "DuplicateSection";
        case EEditorWorkspacePersistenceError::DuplicatePanel: return "DuplicatePanel";
        case EEditorWorkspacePersistenceError::TooManyPanels: return "TooManyPanels";
        case EEditorWorkspacePersistenceError::TitleTooLong: return "TitleTooLong";
        case EEditorWorkspacePersistenceError::InvalidTitle: return "InvalidTitle";
        case EEditorWorkspacePersistenceError::IniTooLarge: return "IniTooLarge";
        case EEditorWorkspacePersistenceError::TruncatedIni: return "TruncatedIni";
        case EEditorWorkspacePersistenceError::TrailingData: return "TrailingData";
        case EEditorWorkspacePersistenceError::ImGuiContextMissing: return "ImGuiContextMissing";
        case EEditorWorkspacePersistenceError::AllocationFailure: return "AllocationFailure";
        case EEditorWorkspacePersistenceError::FileNotFound: return "FileNotFound";
        case EEditorWorkspacePersistenceError::FileOpenFailed: return "FileOpenFailed";
        case EEditorWorkspacePersistenceError::FileSizeFailed: return "FileSizeFailed";
        case EEditorWorkspacePersistenceError::FileChanged: return "FileChanged";
        case EEditorWorkspacePersistenceError::FileReadFailed: return "FileReadFailed";
        case EEditorWorkspacePersistenceError::FileWriteFailed: return "FileWriteFailed";
        case EEditorWorkspacePersistenceError::FileFlushFailed: return "FileFlushFailed";
        case EEditorWorkspacePersistenceError::FileCloseFailed: return "FileCloseFailed";
        case EEditorWorkspacePersistenceError::AtomicReplaceFailed: return "AtomicReplaceFailed";
    }
    return "Unknown";
}

FEditorWorkspacePersistenceResult CEditorWorkspace::TryParseLayoutText(
    const char* text, usize text_size) noexcept {
    FEditorWorkspacePersistenceResult result{};
    result.bytes_processed = static_cast<u64>(text_size);
    if (text == nullptr) {
        result.error = EEditorWorkspacePersistenceError::NullArgument;
        return result;
    }
    if (text_size > kMaxLayoutBytes) {
        result.error = EEditorWorkspacePersistenceError::InputTooLarge;
        return result;
    }
    if (std::memchr(text, '\0', text_size) != nullptr) {
        result.error = EEditorWorkspacePersistenceError::EmbeddedNul;
        return result;
    }

    struct FPanelChange {
        AEditorPanel* panel = nullptr;
        bool visible = false;
        bool dock_target = false;
    };
    FPanelChange staged_changes[kMaxPanels]{};
    char seen_titles[kMaxPanels][kMaxPanelTitleBytes + 1u]{};
    u32 panel_entries = 0u;
    usize offset = 0u;
    u32 line_number = 0u;
    const char* ini_data = "";
    usize ini_size = 0u;

    auto fail = [&](EEditorWorkspacePersistenceError error) noexcept {
        result.error = error;
        result.line = line_number;
        result.panel_entries = panel_entries;
        return result;
    };
    auto read_line = [&](const char*& begin, const char*& end) noexcept -> bool {
        if (offset >= text_size) return false;
        if (++line_number > kMaxLayoutLines) return false;
        const usize begin_offset = offset;
        while (offset < text_size && text[offset] != '\n') ++offset;
        usize length = offset - begin_offset;
        if (offset < text_size) ++offset;
        if (length > 0u && text[begin_offset + length - 1u] == '\r') --length;
        begin = text + begin_offset;
        end = begin + length;
        return true;
    };

    const char* line_begin = nullptr;
    const char* line_end = nullptr;
    if (!read_line(line_begin, line_end)) {
        result.line = line_number;
        result.error = line_number > kMaxLayoutLines
            ? EEditorWorkspacePersistenceError::TooManyLines
            : EEditorWorkspacePersistenceError::BadMagic;
        return result;
    }
    if (static_cast<usize>(line_end - line_begin) > kMaxLayoutLineBytes) {
        return fail(EEditorWorkspacePersistenceError::LineTooLong);
    }
    const char* cursor = line_begin;
    FToken magic{};
    FToken version_token{};
    FToken trailing{};
    if (!NextToken(cursor, line_end, magic) ||
        !TokenEquals(magic, kLayoutMagic)) {
        return fail(EEditorWorkspacePersistenceError::BadMagic);
    }
    if (!NextToken(cursor, line_end, version_token) ||
        NextToken(cursor, line_end, trailing)) {
        return fail(EEditorWorkspacePersistenceError::InvalidSyntax);
    }
    usize version = 0u;
    if (!ParseUsizeToken(version_token, version)) {
        return fail(EEditorWorkspacePersistenceError::InvalidSyntax);
    }
    if (version != kLayoutVersion) {
        return fail(EEditorWorkspacePersistenceError::UnsupportedVersion);
    }

    if (!read_line(line_begin, line_end)) {
        result.line = line_number;
        result.error = line_number > kMaxLayoutLines
            ? EEditorWorkspacePersistenceError::TooManyLines
            : EEditorWorkspacePersistenceError::InvalidSyntax;
        return result;
    }
    if (static_cast<usize>(line_end - line_begin) > kMaxLayoutLineBytes) {
        return fail(EEditorWorkspacePersistenceError::LineTooLong);
    }
    cursor = line_begin;
    FToken ini_keyword{};
    FToken ini_size_token{};
    if (!NextToken(cursor, line_end, ini_keyword) ||
        !TokenEquals(ini_keyword, "IMGUI_INI") ||
        !NextToken(cursor, line_end, ini_size_token) ||
        NextToken(cursor, line_end, trailing) ||
        !ParseUsizeToken(ini_size_token, ini_size)) {
        return fail(EEditorWorkspacePersistenceError::InvalidSyntax);
    }
    if (ini_size > kMaxIniBytes) {
        return fail(EEditorWorkspacePersistenceError::IniTooLarge);
    }
    if (ini_size > text_size - offset) {
        return fail(EEditorWorkspacePersistenceError::TruncatedIni);
    }
    ini_data = text + offset;
    offset += ini_size;
    if (ini_size > 0u) {
        if (offset >= text_size) {
            return fail(EEditorWorkspacePersistenceError::TruncatedIni);
        }
        if (text[offset] == '\r') {
            if (offset + 1u >= text_size || text[offset + 1u] != '\n') {
                return fail(EEditorWorkspacePersistenceError::TrailingData);
            }
            offset += 2u;
        } else if (text[offset] == '\n') {
            ++offset;
        } else {
            return fail(EEditorWorkspacePersistenceError::TrailingData);
        }
    }

    while (offset < text_size) {
        if (!read_line(line_begin, line_end)) {
            if (line_number > kMaxLayoutLines) {
                return fail(EEditorWorkspacePersistenceError::TooManyLines);
            }
            break;
        }
        const usize line_length = static_cast<usize>(line_end - line_begin);
        if (line_length > kMaxLayoutLineBytes) {
            return fail(EEditorWorkspacePersistenceError::LineTooLong);
        }
        cursor = line_begin;
        while (cursor < line_end && (*cursor == ' ' || *cursor == '\t')) ++cursor;
        if (cursor == line_end || *cursor == '#') continue;
        FToken keyword{};
        FToken title{};
        FToken visible{};
        FToken dock{};
        if (!NextToken(cursor, line_end, keyword)) {
            return fail(EEditorWorkspacePersistenceError::InvalidSyntax);
        }
        if (TokenEquals(keyword, "IMGUI_INI")) {
            return fail(EEditorWorkspacePersistenceError::DuplicateSection);
        }
        if (!TokenEquals(keyword, "PANEL")) {
            return fail(EEditorWorkspacePersistenceError::TrailingData);
        }

        // v1 の `PANEL <title> <visible> <dock>` を保ったまま、右端 2 token
        // から逆向きに区切る。これにより表示名の内部 ASCII space を保持できる。
        // keyword / title / visible / dock 間の区切りは 1 byte だけ消費し、余分な
        // space/tab は title の先頭・末尾、または空 token として厳格に拒否する。
        if (keyword.end >= line_end ||
            !IsPanelFieldSeparator(*keyword.end) ||
            IsPanelFieldSeparator(*(line_end - 1))) {
            return fail(EEditorWorkspacePersistenceError::InvalidSyntax);
        }
        const char* const title_begin = keyword.end + 1;

        const char* dock_begin = line_end;
        while (dock_begin > title_begin &&
               !IsPanelFieldSeparator(*(dock_begin - 1))) {
            --dock_begin;
        }
        if (dock_begin <= title_begin ||
            !IsPanelFieldSeparator(*(dock_begin - 1))) {
            return fail(EEditorWorkspacePersistenceError::InvalidSyntax);
        }
        dock.begin = dock_begin;
        dock.end = line_end;

        const char* const visible_end = dock_begin - 1;
        const char* visible_begin = visible_end;
        while (visible_begin > title_begin &&
               !IsPanelFieldSeparator(*(visible_begin - 1))) {
            --visible_begin;
        }
        if (visible_begin == visible_end) {
            return fail(EEditorWorkspacePersistenceError::InvalidSyntax);
        }
        if (visible_begin <= title_begin) {
            return fail(EEditorWorkspacePersistenceError::InvalidTitle);
        }
        visible.begin = visible_begin;
        visible.end = visible_end;

        title.begin = title_begin;
        title.end = visible_begin - 1;
        const usize title_length = static_cast<usize>(title.end - title.begin);
        if (title_length > kMaxPanelTitleBytes) {
            return fail(EEditorWorkspacePersistenceError::TitleTooLong);
        }
        if (!IsValidPanelTitleBytes(title.begin, title_length)) {
            return fail(EEditorWorkspacePersistenceError::InvalidTitle);
        }
        if (!((TokenEquals(visible, "0") || TokenEquals(visible, "1")) &&
              (TokenEquals(dock, "0") || TokenEquals(dock, "1")))) {
            return fail(EEditorWorkspacePersistenceError::InvalidSyntax);
        }
        if (panel_entries >= kMaxPanels) {
            return fail(EEditorWorkspacePersistenceError::TooManyPanels);
        }
        for (u32 i = 0u; i < panel_entries; ++i) {
            if (std::strlen(seen_titles[i]) == title_length &&
                std::memcmp(seen_titles[i], title.begin, title_length) == 0) {
                return fail(EEditorWorkspacePersistenceError::DuplicatePanel);
            }
        }
        std::memcpy(seen_titles[panel_entries], title.begin, title_length);
        seen_titles[panel_entries][title_length] = '\0';
        staged_changes[panel_entries].panel =
            FindPanelByTitle(seen_titles[panel_entries]);
        staged_changes[panel_entries].visible = TokenEquals(visible, "1");
        staged_changes[panel_entries].dock_target = TokenEquals(dock, "1");
        ++panel_entries;
    }

    if (ImGui::GetCurrentContext() == nullptr) {
        return fail(EEditorWorkspacePersistenceError::ImGuiContextMissing);
    }
    /** size 0 で ImGui が strlen を選んでも境界外を読まない静的な空文字列。 */
    static constexpr char kEmptyIni[] = "";
    /** 非空入力では検証済み明示長を維持し、空入力だけ終端保証へ切り替える参照。 */
    const char* const imgui_ini_data = ini_size == 0u ? kEmptyIni : ini_data;
    ImGui::LoadIniSettingsFromMemory(imgui_ini_data, ini_size);
    for (u32 i = 0u; i < panel_entries; ++i) {
        if (staged_changes[i].panel == nullptr) continue;
        staged_changes[i].panel->SetVisible(staged_changes[i].visible);
        staged_changes[i].panel->SetDockTarget(staged_changes[i].dock_target);
    }
    result.line = line_number;
    result.panel_entries = panel_entries;
    return result;
}

FEditorWorkspacePersistenceResult CEditorWorkspace::TryLoadLayout(
    const wchar_t* file_path) noexcept {
    FEditorWorkspacePersistenceResult result{};
    usize path_length = 0u;
    if (file_path == nullptr) {
        result.error = EEditorWorkspacePersistenceError::NullArgument;
        return result;
    }
    if (!IsBoundedWidePath(
            file_path, kMaxPersistencePathChars, path_length)) {
        result.error = EEditorWorkspacePersistenceError::PathTooLong;
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
                ? EEditorWorkspacePersistenceError::FileNotFound
                : EEditorWorkspacePersistenceError::FileOpenFailed;
        return result;
    }
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        result.os_error = ::GetLastError();
        (void)::CloseHandle(file);
        result.error = EEditorWorkspacePersistenceError::FileSizeFailed;
        return result;
    }
    if (static_cast<u64>(size.QuadPart) > static_cast<u64>(kMaxLayoutBytes)) {
        (void)::CloseHandle(file);
        result.error = EEditorWorkspacePersistenceError::InputTooLarge;
        return result;
    }
    TArray<char> text;
    if (!text.TryResize(static_cast<usize>(size.QuadPart))) {
        (void)::CloseHandle(file);
        result.error = EEditorWorkspacePersistenceError::AllocationFailure;
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
            result.error = EEditorWorkspacePersistenceError::FileReadFailed;
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
        result.error = EEditorWorkspacePersistenceError::FileReadFailed;
        result.bytes_processed = static_cast<u64>(total);
        return result;
    }
    LARGE_INTEGER final_size{};
    if (probe_read != 0u) {
        (void)::CloseHandle(file);
        result.error = EEditorWorkspacePersistenceError::FileChanged;
        result.bytes_processed = static_cast<u64>(total);
        return result;
    }
    if (!::GetFileSizeEx(file, &final_size)) {
        result.os_error = ::GetLastError();
        (void)::CloseHandle(file);
        result.error = EEditorWorkspacePersistenceError::FileSizeFailed;
        return result;
    }
    if (final_size.QuadPart != size.QuadPart) {
        (void)::CloseHandle(file);
        result.error = EEditorWorkspacePersistenceError::FileChanged;
        result.bytes_processed = static_cast<u64>(total);
        return result;
    }
    if (!::CloseHandle(file)) {
        result.os_error = ::GetLastError();
        result.error = EEditorWorkspacePersistenceError::FileCloseFailed;
        return result;
    }
    result = TryParseLayoutText(
        text.IsEmpty() ? "" : text.Data(), text.Size());
    result.bytes_processed = static_cast<u64>(total);
    return result;
}

FEditorWorkspacePersistenceResult CEditorWorkspace::TrySaveLayout(
    const wchar_t* file_path) noexcept {
    FEditorWorkspacePersistenceResult result{};
    usize path_length = 0u;
    if (file_path == nullptr) {
        result.error = EEditorWorkspacePersistenceError::NullArgument;
        return result;
    }
    if (!IsBoundedWidePath(
            file_path, kMaxPersistencePathChars, path_length)) {
        result.error = EEditorWorkspacePersistenceError::PathTooLong;
        return result;
    }
    if (ImGui::GetCurrentContext() == nullptr) {
        result.error = EEditorWorkspacePersistenceError::ImGuiContextMissing;
        return result;
    }
    usize ini_size = 0u;
    const char* ini_data = ImGui::SaveIniSettingsToMemory(&ini_size);
    if (ini_size > kMaxIniBytes) {
        result.error = EEditorWorkspacePersistenceError::IniTooLarge;
        return result;
    }
    if ((ini_size > 0u && ini_data == nullptr) ||
        (ini_size > 0u && std::memchr(ini_data, '\0', ini_size) != nullptr)) {
        result.error = EEditorWorkspacePersistenceError::EmbeddedNul;
        return result;
    }
    const usize panel_count = m_Panels.Size();
    if (panel_count > kMaxPanels) {
        result.error = EEditorWorkspacePersistenceError::TooManyPanels;
        return result;
    }
    usize title_lengths[kMaxPanels]{};
    for (usize i = 0u; i < panel_count; ++i) {
        const AEditorPanel* panel = m_Panels[i];
        if (panel == nullptr || panel->Title() == nullptr) {
            result.error = EEditorWorkspacePersistenceError::InvalidTitle;
            return result;
        }
        if (!IsValidPanelTitle(panel->Title(), title_lengths[i])) {
            usize bounded_length = 0u;
            while (bounded_length <= kMaxPanelTitleBytes &&
                   panel->Title()[bounded_length] != '\0') {
                ++bounded_length;
            }
            result.error = bounded_length > kMaxPanelTitleBytes
                ? EEditorWorkspacePersistenceError::TitleTooLong
                : EEditorWorkspacePersistenceError::InvalidTitle;
            return result;
        }
        for (usize j = 0u; j < i; ++j) {
            if (title_lengths[j] == title_lengths[i] &&
                std::memcmp(
                    m_Panels[j]->Title(), panel->Title(), title_lengths[i]) == 0) {
                result.error = EEditorWorkspacePersistenceError::DuplicatePanel;
                return result;
            }
        }
    }

    TArray<char> output;
    const usize reserve_size =
        128u + ini_size + panel_count * (kMaxPanelTitleBytes + 32u);
    if (!output.TryReserve(reserve_size)) {
        result.error = EEditorWorkspacePersistenceError::AllocationFailure;
        return result;
    }
    char line[256]{};
    int line_size = std::snprintf(
        line, sizeof(line), "%s %u\nIMGUI_INI %zu\n",
        kLayoutMagic, kLayoutVersion, ini_size);
    if (line_size < 0 || static_cast<usize>(line_size) >= sizeof(line) ||
        !AppendBytes(output, line, static_cast<usize>(line_size)) ||
        (ini_size > 0u && !AppendBytes(output, ini_data, ini_size)) ||
        (ini_size > 0u && !AppendBytes(output, "\n", 1u))) {
        result.error = EEditorWorkspacePersistenceError::AllocationFailure;
        return result;
    }
    for (usize i = 0u; i < panel_count; ++i) {
        const AEditorPanel* panel = m_Panels[i];
        line_size = std::snprintf(
            line, sizeof(line), "PANEL %s %u %u\n",
            panel->Title(), panel->IsVisible() ? 1u : 0u,
            panel->IsDockTarget() ? 1u : 0u);
        if (line_size < 0 || static_cast<usize>(line_size) >= sizeof(line) ||
            !AppendBytes(output, line, static_cast<usize>(line_size))) {
            result.error = EEditorWorkspacePersistenceError::AllocationFailure;
            return result;
        }
    }
    if (output.Size() > kMaxLayoutBytes) {
        result.error = EEditorWorkspacePersistenceError::InputTooLarge;
        return result;
    }

    constexpr usize kTempPathCapacity = kMaxPersistencePathChars + 97u;
    wchar_t temp_path[kTempPathCapacity]{};
    HANDLE temp = INVALID_HANDLE_VALUE;
    for (u32 attempt = 0u; attempt < 8u; ++attempt) {
        if (!BuildUniqueTempPath(
                file_path, path_length, temp_path, kTempPathCapacity, attempt)) {
            result.error = EEditorWorkspacePersistenceError::PathTooLong;
            return result;
        }
        temp = ::CreateFileW(
            temp_path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (temp != INVALID_HANDLE_VALUE) break;
        result.os_error = ::GetLastError();
        if (result.os_error != ERROR_FILE_EXISTS &&
            result.os_error != ERROR_ALREADY_EXISTS) {
            result.error = EEditorWorkspacePersistenceError::FileOpenFailed;
            return result;
        }
    }
    if (temp == INVALID_HANDLE_VALUE) {
        result.error = EEditorWorkspacePersistenceError::FileOpenFailed;
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
            result.error = EEditorWorkspacePersistenceError::FileWriteFailed;
            result.bytes_processed = static_cast<u64>(total);
            return result;
        }
        total += written;
    }
    if (!::FlushFileBuffers(temp)) {
        result.os_error = ::GetLastError();
        (void)::CloseHandle(temp);
        (void)::DeleteFileW(temp_path);
        result.error = EEditorWorkspacePersistenceError::FileFlushFailed;
        return result;
    }
    if (!::CloseHandle(temp)) {
        result.os_error = ::GetLastError();
        (void)::DeleteFileW(temp_path);
        result.error = EEditorWorkspacePersistenceError::FileCloseFailed;
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
            result.error =
                EEditorWorkspacePersistenceError::AtomicReplaceFailed;
            return result;
        }
    }
    result.panel_entries = static_cast<u32>(panel_count);
    result.bytes_processed = static_cast<u64>(total);
    return result;
}

void CEditorWorkspace::SaveLayout(const wchar_t* file_path) noexcept {
    if (file_path == nullptr) return;
    const FEditorWorkspacePersistenceResult result = TrySaveLayout(file_path);
    if (!result.Succeeded()) {
        ACS_LOG_WARN(
            "CEditorWorkspace::SaveLayout: %s (line=%u os=%u)",
            FEditorWorkspacePersistenceResult::ErrorName(result.error),
            result.line, result.os_error);
    }
}

void CEditorWorkspace::LoadLayout(const wchar_t* file_path) noexcept {
    if (file_path == nullptr) return;
    const FEditorWorkspacePersistenceResult result = TryLoadLayout(file_path);
    if (!result.Succeeded() &&
        result.error != EEditorWorkspacePersistenceError::FileNotFound) {
        ACS_LOG_WARN(
            "CEditorWorkspace::LoadLayout: %s (line=%u os=%u)",
            FEditorWorkspacePersistenceResult::ErrorName(result.error),
            result.line, result.os_error);
    }
}

/** CSelectionService 参照を登録 / 解除する。 */
void CEditorWorkspace::SetSelectionService(inspector::CSelectionService* svc) noexcept {
    m_SelectionService = svc;
}

/** 現在登録されている CSelectionService を返す。 */
inspector::CSelectionService* CEditorWorkspace::GetSelectionService() const noexcept {
    return m_SelectionService;
}

/** 全 panel に OnSelectionChanged を fan-out する (未注入時は no-op)。 */
void CEditorWorkspace::BroadcastSelectionChanged() noexcept {
    if (m_SelectionService == nullptr) {
        // panel 側の OnSelectionChanged シグネチャが CSelectionService& 必須なので、
        // 未注入時は呼べない。silent no-op (= editor 起動初期化中の呼出しも安全)。
        return;
    }
    const usize n = m_Panels.Size();
    for (usize i = 0; i < n; ++i) {
        AEditorPanel* p = m_Panels[i];
        if (p != nullptr) {
            p->OnSelectionChanged(*m_SelectionService);
        }
    }
}

/** 全 panel に OnAssetSelected を fan-out する (nullptr は選択解除として伝播)。 */
void CEditorWorkspace::BroadcastAssetSelected(const char* asset_path) noexcept {
    // asset_path == nullptr は「選択解除」として panel に伝播する規約
    // (AEditorPanel.h の OnAssetSelected コメント参照)。null チェックはしない。
    const usize n = m_Panels.Size();
    for (usize i = 0; i < n; ++i) {
        AEditorPanel* p = m_Panels[i];
        if (p != nullptr) {
            p->OnAssetSelected(asset_path);
        }
    }
}

/** panel をポインタ完全一致で探索する (未ヒットは kInvalidIndex)。 */
i32 CEditorWorkspace::FindPanelIndex(const AEditorPanel* panel) const noexcept {
    if (panel == nullptr) return kInvalidIndex;
    const usize n = m_Panels.Size();
    for (usize i = 0; i < n; ++i) {
        if (m_Panels[i] == panel) {
            return static_cast<i32>(i);
        }
    }
    return kInvalidIndex;
}

} // namespace acs::game::editor_core
