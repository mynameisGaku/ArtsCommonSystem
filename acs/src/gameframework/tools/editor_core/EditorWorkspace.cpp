// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — editor_core / EditorWorkspace 実装 (Phase 21a)
//
// 仕様の意図は EditorWorkspace.h を参照。本ファイルでは:
//   ・panel 登録 / 解除 / 探索 (Array<EditorPanel*> ベース)
//   ・1 フレーム駆動 (OnFrameBegin → DockSpace → MenuBar → DrawUI)
//   ・ImGui DockSpaceOverViewport の生成
//   ・Window / Layout メニューの描画
//   ・`.acslayout` 形式 (テキスト: magic + ImGui ini + per-panel state) の save/load
//   ・SelectionService 参照保管 + Broadcast の fan-out
// を実装する。全 noexcept、STL 不使用、ImGui 依存はこの .cpp に閉じる。

#include "gameframework/tools/editor_core/EditorWorkspace.h"

#include "gameframework/tools/editor_core/EditorPanel.h"
#include "foundation/Log.h"
#include "platform/FileSystem.h"

#include <imgui.h>

#include <cstdio>   // std::snprintf / std::sscanf (layout text の整形 / 解析)
#include <cstring>  // std::strcmp / std::strlen / std::memcpy

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

// =============================================================================
// ローカルヘルパ
// =============================================================================

// panel ポインタが「登録対象として安全か」を判定。null と Title() == nullptr を弾く。
// Title() は ImGui::Begin の id / Find lookup key / Layout シリアライズ key の
// 3 役を兼ねるため、ここで nullptr を必ず弾いて以降の処理を単純化する。
static bool IsRegistrablePanel(const EditorPanel* p) noexcept {
    if (p == nullptr) return false;
    if (p->Title() == nullptr) return false;
    return true;
}

// strcmp の null 安全版。一方でも null なら不一致扱い (両 null も不一致)。
// Title() が nullptr の panel は IsRegistrablePanel で弾かれている前提だが、
// FindPanelByTitle 引数 / Layout load 文字列の null も等しく弾くため共通化。
static bool StrEqual(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    return std::strcmp(a, b) == 0;
}

// =============================================================================
// Init / Shutdown
// =============================================================================
void EditorWorkspace::Init() noexcept {
    // 完全リセット: 登録 panel list を空に、参照 / フラグを default に。
    // 容量は保持 (Clear は size=0 にするだけ、capacity はそのまま)。
    _panels.Clear();

    _selection_service          = nullptr;

    _no_docking_in_central_node = false;
    _enable_dockspace           = true;
    _enable_menu_bar            = true;
}

void EditorWorkspace::Shutdown() noexcept {
    // 登録済み panel に OnShutdown を呼び、list を空にする。
    // 呼び出し順は登録順 (= Init / Tick と同じ順序で逆順にしない)。
    // 「逆順 shutdown」は dependency 解決が必要だが Phase 21a では未対応。
    const usize n = _panels.Size();
    for (usize i = 0; i < n; ++i) {
        EditorPanel* p = _panels[i];
        if (p != nullptr) {
            p->OnShutdown();
        }
    }
    _panels.Clear();

    _selection_service = nullptr;
    // フラグ類は意図的にリセットしない (= Shutdown 後に再 Init で同じ host 設定を
    // 引き継げるよう)。完全 default に戻したい host は Init() を続けて呼ぶこと。
}

// =============================================================================
// panel 登録 / 解除
// =============================================================================
void EditorWorkspace::RegisterPanel(EditorPanel* panel) noexcept {
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
    if (_panels.Size() >= static_cast<usize>(kMaxPanels)) {
        ACS_LOG_WARN("EditorWorkspace::RegisterPanel: panel limit %u reached, ignoring '%s'",
                     static_cast<unsigned>(kMaxPanels),
                     panel->Title());
        return;
    }
    _panels.PushBack(panel);
    // OnInit はリスト登録 **後** に呼ぶ。これにより OnInit 内から
    // `Workspace()->PanelCount()` 等を呼んだ場合に自身も数に含まれる
    // (= 自己参照アクセスが破綻しない)。
    panel->OnInit(*this);
}

void EditorWorkspace::UnregisterPanel(EditorPanel* panel) noexcept {
    if (panel == nullptr) return;
    const i32 idx = FindPanelIndex(panel);
    if (idx == kInvalidIndex) return;

    // OnShutdown はリストから外す **前** に呼ぶ。これにより OnShutdown 内から
    // 自身を再 Find しても見つかる状態を保つ (= panel 内 cleanup で workspace
    // 経由の API を呼んでも安全)。
    panel->OnShutdown();

    // 順序保存削除 (shift)。RemoveAtSwap は使わない (= UI 表示順を保ちたい)。
    // Array に Erase API が無いため手書きシフト (= ParticleEditorPanel と同形)。
    const usize sel = static_cast<usize>(idx);
    for (usize i = sel + 1; i < _panels.Size(); ++i) {
        _panels[i - 1] = _panels[i];
    }
    _panels.PopBack();
}

u32 EditorWorkspace::PanelCount() const noexcept {
    return static_cast<u32>(_panels.Size());
}

EditorPanel* EditorWorkspace::GetPanelByIndex(u32 i) const noexcept {
    if (i >= static_cast<u32>(_panels.Size())) return nullptr;
    return _panels[static_cast<usize>(i)];
}

EditorPanel* EditorWorkspace::FindPanelByTitle(const char* title) const noexcept {
    if (title == nullptr) return nullptr;
    const usize n = _panels.Size();
    for (usize i = 0; i < n; ++i) {
        EditorPanel* p = _panels[i];
        if (p == nullptr) continue;
        if (StrEqual(p->Title(), title)) {
            return p;
        }
    }
    return nullptr;
}

void EditorWorkspace::TogglePanelVisible(const char* title) noexcept {
    EditorPanel* p = FindPanelByTitle(title);
    if (p == nullptr) return;
    p->SetVisible(!p->IsVisible());
}

// =============================================================================
// メインループ
// =============================================================================
void EditorWorkspace::TickAllPanels(f32 dt) noexcept {
    // 1) OnFrameBegin: 非 visible panel もバックグラウンド処理 (非同期 I/O,
    //    polling, animation timer 等) を進める可能性があるため、visibility を
    //    問わず全 panel に呼ぶ。
    {
        const usize n = _panels.Size();
        for (usize i = 0; i < n; ++i) {
            EditorPanel* p = _panels[i];
            if (p != nullptr) {
                p->OnFrameBegin(dt);
            }
        }
    }

    // 2) DockSpace: ImGui Window より前に central node を確保しておくと、
    //    panel 側で初回 ImGui::Begin した時点で自動 dock 候補に central node が
    //    含まれるようになる。
    if (_enable_dockspace) {
        DrawDockSpace();
    }

    // 3) MenuBar (MainMenuBar 一段). DockSpace と並んで host 側で抑制できる。
    if (_enable_menu_bar) {
        DrawMenuBar();
    }

    // 4) DrawUI: 各 panel に描画させる。visibility / ImGui::Begin / End は
    //    派生 panel 側の責務 (本 workspace は呼び出すだけ)。
    {
        const usize n = _panels.Size();
        for (usize i = 0; i < n; ++i) {
            EditorPanel* p = _panels[i];
            if (p != nullptr) {
                p->DrawUI();
            }
        }
    }
}

void EditorWorkspace::DrawDockSpace() noexcept {
#if ACS_EDITOR_HAS_IMGUI_DOCK
    // ImGui::DockSpaceOverViewport は main viewport の client area 全体に
    // ID 0 / null viewport (= 自動でメインを選択) で DockSpace を貼る。
    // 同 ID で多重呼び出ししても ImGui 側で no-op になるため、TickAllPanels
    // 自動呼出しと host 側手動呼出しの共存は安全。
    ImGuiDockNodeFlags flags = ImGuiDockNodeFlags_PassthruCentralNode;
    if (_no_docking_in_central_node) {
        flags |= ImGuiDockNodeFlags_NoDockingInCentralNode;
    }
    // 0 を渡すと ImGui がメインビューポートを選択 (NULL viewport 相当)。
    // window class は default (= 全 panel が共通 dock 候補)。
    ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), flags);
#else
    // docking 非対応 ImGui (master branch) — DockSpace は描画できないため
    // no-op。各 panel は通常の float ImGui window として並ぶ。`_no_docking_in_central_node`
    // の値は参照しないが、API シグネチャは docking branch と共通に保つ。
    // Phase 21c 以降で ImGui を docking branch に切替えた際に自動有効化される。
#endif
}

void EditorWorkspace::DrawMenuBar() noexcept {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    // ----- Window メニュー (panel toggle) -----
    if (ImGui::BeginMenu("Window")) {
        const usize n = _panels.Size();
        if (n == 0) {
            ImGui::TextDisabled("(no panels registered)");
        } else {
            for (usize i = 0; i < n; ++i) {
                EditorPanel* p = _panels[i];
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

    // ----- Layout メニュー (save / load default) -----
    // ファイルダイアログは依存追加を避けるため、Phase 21a では固定パスのみ。
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

// =============================================================================
// レイアウト永続化 (`.acslayout` フォーマット)
// =============================================================================
// テキストフォーマット (詳細は EditorWorkspace.h のコメント参照):
//   ACS_EDLAYOUT 1\n
//   IMGUI_INI <byte_size>\n
//   <raw ini bytes>\n
//   PANEL <title> <visible:0/1> <dock_target:0/1>\n
//   ...
// =============================================================================
void EditorWorkspace::SaveLayout(const wchar_t* file_path) noexcept {
    if (file_path == nullptr) return;

    // ----- ImGui ini 文字列を取得 -----
    // ImGui::SaveIniSettingsToMemory は内部 buffer の生ポインタ + 長さを返す。
    // 戻り値は ImGui owned (次フレームまでは valid) なので即 buffer に積む。
    usize       ini_size_sz = 0;
    const char* ini_ptr     = ImGui::SaveIniSettingsToMemory(&ini_size_sz);
    const u32   ini_size    = (ini_ptr != nullptr) ? static_cast<u32>(ini_size_sz) : 0u;

    // ----- 出力バッファ準備 (Array<char>) -----
    // 概算: ヘッダ 32B + ImGui ini (ini_size) + per-panel 64B × N
    // 確保上限を見積もって PushBack 連発するより、Resize+memcpy の方が
    // 1 度 で済む。
    const usize approx_capacity =
        64u + static_cast<usize>(ini_size) + _panels.Size() * 128u;
    Array<char> out;
    out.Reserve(approx_capacity);

    // 小ヘルパ: char buffer (NUL 終端の有無不問) を末尾に追記。
    auto append_bytes = [&](const char* data, usize bytes) noexcept {
        if (data == nullptr || bytes == 0) return;
        const usize old = out.Size();
        out.Resize(old + bytes);
        std::memcpy(out.Data() + old, data, bytes);
    };
    // 小ヘルパ: NUL 終端文字列を末尾に追記 (strlen 計算)。
    auto append_cstr = [&](const char* s) noexcept {
        if (s == nullptr) return;
        append_bytes(s, std::strlen(s));
    };

    // ----- 1) ヘッダ行 -----
    {
        char header[64] = {};
        const int n = std::snprintf(header, sizeof(header), "%s %u\n",
                                    kLayoutMagic,
                                    static_cast<unsigned>(kLayoutVersion));
        if (n > 0 && static_cast<usize>(n) < sizeof(header)) {
            append_bytes(header, static_cast<usize>(n));
        }
    }

    // ----- 2) ImGui ini ブロック -----
    {
        char header[64] = {};
        const int n = std::snprintf(header, sizeof(header), "IMGUI_INI %u\n",
                                    static_cast<unsigned>(ini_size));
        if (n > 0 && static_cast<usize>(n) < sizeof(header)) {
            append_bytes(header, static_cast<usize>(n));
        }
        if (ini_size > 0 && ini_ptr != nullptr) {
            append_bytes(ini_ptr, static_cast<usize>(ini_size));
            // ini 末尾が改行で終わる保証は無いので separator を挟む。
            append_cstr("\n");
        }
    }

    // ----- 3) PANEL 行群 -----
    {
        const usize n_panels = _panels.Size();
        for (usize i = 0; i < n_panels; ++i) {
            const EditorPanel* p = _panels[i];
            if (p == nullptr) continue;
            const char* title = p->Title();
            if (title == nullptr) continue;
            // title に空白が含まれていると LoadLayout 側の strtok 風 split が
            // 壊れるため、空白を含む title は本 layout フォーマットでは
            // skip する (= panel 側で空白を含めない命名規則を期待)。
            // Phase 21a では検査だけして警告ログを出す (skip しても致命的
            // ではない: 次回 Load で visibility が default のままになるだけ)。
            bool has_space = false;
            for (const char* c = title; *c != '\0'; ++c) {
                if (*c == ' ' || *c == '\t' || *c == '\n') { has_space = true; break; }
            }
            if (has_space) {
                ACS_LOG_WARN("EditorWorkspace::SaveLayout: panel title '%s' contains whitespace, skipping layout entry",
                             title);
                continue;
            }

            char line[256] = {};
            const int n = std::snprintf(line, sizeof(line),
                                        "PANEL %s %d %d\n",
                                        title,
                                        p->IsVisible()    ? 1 : 0,
                                        p->IsDockTarget() ? 1 : 0);
            if (n > 0 && static_cast<usize>(n) < sizeof(line)) {
                append_bytes(line, static_cast<usize>(n));
            }
        }
    }

    // ----- バイト列として書き出し -----
    // WriteAllText は NUL 終端を書かない仕様だが、内部で strlen を呼ぶ可能性が
    // あるため、ini に NUL が含まれる場合に備えて WriteAllBytes を使う。
    auto wr = FileSystem::WriteAllBytes(
        file_path,
        reinterpret_cast<const byte*>(out.Data()),
        out.Size());
    if (wr.IsErr()) {
        // 失敗は silent (致命ではない)。ログのみ。
        ACS_LOG_WARN("EditorWorkspace::SaveLayout: WriteAllBytes failed");
    }
}

void EditorWorkspace::LoadLayout(const wchar_t* file_path) noexcept {
    if (file_path == nullptr) return;
    if (!FileSystem::Exists(file_path)) {
        // 初回起動時は存在しないことが普通なので silent (ログも出さない)。
        return;
    }

    auto rr = FileSystem::ReadAllText(file_path);
    if (rr.IsErr()) {
        ACS_LOG_WARN("EditorWorkspace::LoadLayout: ReadAllText failed");
        return;
    }
    // ReadAllText は末尾 NUL 付きで返す (foundation/Result 経由 Array<char>)。
    Array<char>& text = rr.Value();
    if (text.IsEmpty()) return;

    // ----- 行分割 in-place: '\n' を '\0' に書き換えながら走る -----
    char* const buf      = text.Data();
    const usize buf_size = text.Size();

    // 1) ヘッダ行を検査
    char* line_start = buf;
    char* p          = buf;
    auto next_line = [&]() noexcept -> char* {
        // 現在 line_start の終端を探して '\0' に書き換え、次行の先頭を返す。
        // 戻り値が nullptr の場合は EOF。
        while (p < buf + buf_size && *p != '\0' && *p != '\n') ++p;
        if (p >= buf + buf_size || *p == '\0') {
            return nullptr;  // EOF
        }
        // *p == '\n'
        *p = '\0';
        // \r\n 対策: 直前が '\r' なら 1 文字戻して '\0' で潰す
        if (p > line_start && *(p - 1) == '\r') {
            *(p - 1) = '\0';
        }
        ++p;
        return p;
    };

    // ヘッダ行
    char* next = next_line();
    {
        // 期待 "ACS_EDLAYOUT 1"
        unsigned version = 0;
        char magic[32] = {};
        const int matched = std::sscanf(line_start, "%31s %u", magic, &version);
        if (matched != 2 || std::strcmp(magic, kLayoutMagic) != 0) {
            ACS_LOG_WARN("EditorWorkspace::LoadLayout: bad magic");
            return;
        }
        if (version != kLayoutVersion) {
            ACS_LOG_WARN("EditorWorkspace::LoadLayout: version mismatch (got %u, expect %u)",
                         version, static_cast<unsigned>(kLayoutVersion));
            // 互換性のない version は安全に no-op (Phase 21a では 1 つしかない)。
            return;
        }
    }
    if (next == nullptr) return;
    line_start = next;

    // 2) IMGUI_INI ブロック (1 個まで、optional)
    //    header 行を **peek 解析** してから IMGUI_INI なら本格処理、そうでなければ
    //    line_start を消費せず PANEL ループに渡す。peek は line_start の内容を
    //    壊さないように sscanf 単発のみ。next_line() による '\n' 置換は IMGUI_INI
    //    確定後に行う。
    {
        unsigned ini_bytes = 0;
        char     keyword[32] = {};
        const int matched = std::sscanf(line_start, "%31s %u", keyword, &ini_bytes);
        if (matched == 2 && std::strcmp(keyword, "IMGUI_INI") == 0) {
            // IMGUI_INI 行確定 → next_line() でこの行の '\n' を '\0' に潰し、
            // 直後の raw ini bytes を ImGui に流し込む。
            next = next_line();
            if (next != nullptr && ini_bytes > 0) {
                char* const ini_ptr   = next;       // next は char* (mutable buf 内)
                const usize remaining = buf_size - static_cast<usize>(next - buf);
                usize       actual    = ini_bytes;
                if (actual > remaining) actual = remaining;
                ImGui::LoadIniSettingsFromMemory(ini_ptr, actual);
                // p を ini ブロック直後に進める。next_line による '\n' 置換が
                // ini block の中で起きないように、明示的にスキップする。
                p = ini_ptr + actual;
            }
            // 次の行頭まで進める。ini block の直後は通常 '\n' (SaveLayout 側で
            // 余分な改行を挟んでいる)。p が '\r' / '\n' を指していたら消費する。
            while (p < buf + buf_size && *p == '\r') ++p;
            if (p < buf + buf_size && *p == '\n') {
                *p = '\0';
                ++p;
            }
            line_start = p;
        }
        // IMGUI_INI でなかった場合は line_start を消費せず PANEL ループへ落ちる
        // (= ヘッダ直後がいきなり PANEL 行というレイアウトファイルにも対応)。
    }

    // 3) PANEL 行ループ
    //    "PANEL <title> <visible> <dock>" を順に処理。未登録 title は skip。
    while (line_start != nullptr && line_start < buf + buf_size && *line_start != '\0') {
        next = next_line();

        char keyword[16] = {};
        char title[160]  = {};
        int  visible     = 0;
        int  dock        = 0;
        const int matched = std::sscanf(line_start, "%15s %159s %d %d",
                                        keyword, title, &visible, &dock);
        if (matched == 4 && std::strcmp(keyword, "PANEL") == 0) {
            EditorPanel* panel = FindPanelByTitle(title);
            if (panel != nullptr) {
                panel->SetVisible   (visible != 0);
                panel->SetDockTarget(dock    != 0);
            }
            // 未登録 panel は silent skip (= 後で同名 panel が登録された場合に
            // visibility 復元できないが、これは layout file が古い前提なので OK)。
        }
        // 不明 keyword は silent skip (前方互換性のため)。

        if (next == nullptr) break;
        line_start = next;
    }
}

// =============================================================================
// SelectionService 連携
// =============================================================================
void EditorWorkspace::SetSelectionService(inspector::SelectionService* svc) noexcept {
    _selection_service = svc;
}

inspector::SelectionService* EditorWorkspace::GetSelectionService() const noexcept {
    return _selection_service;
}

void EditorWorkspace::BroadcastSelectionChanged() noexcept {
    if (_selection_service == nullptr) {
        // panel 側の OnSelectionChanged シグネチャが SelectionService& 必須なので、
        // 未注入時は呼べない。silent no-op (= editor 起動初期化中の呼出しも安全)。
        return;
    }
    const usize n = _panels.Size();
    for (usize i = 0; i < n; ++i) {
        EditorPanel* p = _panels[i];
        if (p != nullptr) {
            p->OnSelectionChanged(*_selection_service);
        }
    }
}

void EditorWorkspace::BroadcastAssetSelected(const char* asset_path) noexcept {
    // asset_path == nullptr は「選択解除」として panel に伝播する規約
    // (EditorPanel.h の OnAssetSelected コメント参照)。null チェックはしない。
    const usize n = _panels.Size();
    for (usize i = 0; i < n; ++i) {
        EditorPanel* p = _panels[i];
        if (p != nullptr) {
            p->OnAssetSelected(asset_path);
        }
    }
}

// =============================================================================
// 内部: panel index 探索
// =============================================================================
i32 EditorWorkspace::FindPanelIndex(const EditorPanel* panel) const noexcept {
    if (panel == nullptr) return kInvalidIndex;
    const usize n = _panels.Size();
    for (usize i = 0; i < n; ++i) {
        if (_panels[i] == panel) {
            return static_cast<i32>(i);
        }
    }
    return kInvalidIndex;
}

} // namespace acs::game::editor_core
