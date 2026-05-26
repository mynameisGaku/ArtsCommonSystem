// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar K (editor_core) — AssetBrowser 実装 (Phase 21a)
//
// 仕様の意図は AssetBrowser.h を参照。本ファイルでは:
//   ・FindFirstFileW / FindNextFileW で assets/ 配下を列挙
//   ・拡張子 lookup で EAssetKind 推定
//   ・ImGui を使った左 tree + 右 list の 2 ペインレイアウト
//   ・Drag Source ("ASSET_PATH" payload = wchar_t*) 発行
// を実装する。すべて noexcept、STL 不使用、ImGui include は本 .cpp 限定。
#include "gameframework/tools/editor_core/AssetBrowser.h"

#include "foundation/Platform.h"

// <windows.h> マクロ汚染対策: `SetCurrentDirectory` がデフォルトで
// `SetCurrentDirectoryW` に macro 展開され、AssetBrowser::SetCurrentDirectory
// メソッドの定義側と衝突する。本 .cpp ではメソッド名を維持するため undef。
#ifdef SetCurrentDirectory
#undef SetCurrentDirectory
#endif

#include <imgui.h>

#include <cstdio>   // snprintf (ラベル整形)
#include <cstring>  // strlen / memcpy (UTF-8 比較用)

namespace acs::game::editor_core {

// ===========================================================================
// ローカルヘルパ
// ===========================================================================

namespace {

// wchar_t 列の長さ (終端含まず)。
usize WLen(const wchar_t* s) noexcept {
    if (s == nullptr) return 0;
    usize n = 0;
    while (s[n] != L'\0') ++n;
    return n;
}

// wchar_t を ASCII 比較用に小文字化 (拡張子比較で使う)。'A' 〜 'Z' のみ対応。
wchar_t WLower(wchar_t c) noexcept {
    if (c >= L'A' && c <= L'Z') return static_cast<wchar_t>(c - L'A' + L'a');
    return c;
}

// 拡張子比較 (大文字小文字無視、`ext` は L".png" 形式の終端 0 付き)。
// path の末尾が ext と一致すれば true。
bool EndsWithIgnoreCase(const wchar_t* path, const wchar_t* ext) noexcept {
    const usize p_len = WLen(path);
    const usize e_len = WLen(ext);
    if (e_len > p_len) return false;
    const wchar_t* p_tail = path + (p_len - e_len);
    for (usize i = 0; i < e_len; ++i) {
        if (WLower(p_tail[i]) != WLower(ext[i])) return false;
    }
    return true;
}

// path 末尾の '\\' または '/' 以降を返す (= ベース名のポインタ、wchar_t)。
const wchar_t* WBaseName(const wchar_t* path) noexcept {
    if (path == nullptr) return nullptr;
    const wchar_t* last = path;
    for (const wchar_t* p = path; *p != L'\0'; ++p) {
        if (*p == L'\\' || *p == L'/') last = p + 1;
    }
    return last;
}

// wchar_t* を UTF-8 char* に変換 (out_buf 終端 0 含む)。失敗時 out_buf[0]=0。
void WideToUtf8(const wchar_t* src, char* out_buf, int out_cap) noexcept {
    if (out_buf == nullptr || out_cap <= 0) return;
    out_buf[0] = '\0';
    if (src == nullptr) return;
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, src, -1, out_buf, out_cap, nullptr, nullptr);
    if (n <= 0) out_buf[0] = '\0';
    out_buf[out_cap - 1] = '\0';  // 念のため終端保証
}

// FILETIME -> u64 (100ns 単位)
u64 FileTimeToU64(const FILETIME& ft) noexcept {
    return (static_cast<u64>(ft.dwHighDateTime) << 32) | static_cast<u64>(ft.dwLowDateTime);
}

// "EAssetKind 名前" を返す。UI のタグ ("[TEX ]" 等) や Combo に使う。
const char* KindLabel(EAssetKind k) noexcept {
    switch (k) {
        case EAssetKind::Unknown:      return "Unknown";
        case EAssetKind::Texture:      return "Texture";
        case EAssetKind::Mesh:         return "Mesh";
        case EAssetKind::Font:         return "Font";
        case EAssetKind::Audio:        return "Audio";
        case EAssetKind::Material:     return "Material";
        case EAssetKind::Particle:     return "Particle";
        case EAssetKind::Animation:    return "Animation";
        case EAssetKind::BehaviorTree: return "BehaviorTree";
        case EAssetKind::Tilemap:      return "Tilemap";
        case EAssetKind::Prefab:       return "Prefab";
        case EAssetKind::Cinematic:    return "Cinematic";
        case EAssetKind::Scene:        return "Scene";
        case EAssetKind::Other:        return "Other";
    }
    return "Unknown";
}

// 列表示の左端タグ (4-5 文字、固定幅っぽく)。
const char* KindTag(EAssetKind k) noexcept {
    switch (k) {
        case EAssetKind::Texture:      return "TEX ";
        case EAssetKind::Mesh:         return "MESH";
        case EAssetKind::Font:         return "FONT";
        case EAssetKind::Audio:        return "AUD ";
        case EAssetKind::Material:     return "MAT ";
        case EAssetKind::Particle:     return "FX  ";
        case EAssetKind::Animation:    return "ANIM";
        case EAssetKind::BehaviorTree: return "BT  ";
        case EAssetKind::Tilemap:      return "TILE";
        case EAssetKind::Prefab:       return "PRE ";
        case EAssetKind::Cinematic:    return "CINE";
        case EAssetKind::Scene:        return "SCN ";
        case EAssetKind::Other:        return "OTH ";
        case EAssetKind::Unknown:      return "??? ";
    }
    return "??? ";
}

} // anonymous namespace

// ===========================================================================
// Init / Shutdown
// ===========================================================================
void AssetBrowser::Init(const wchar_t* root_directory) noexcept {
    // _root_directory にコピー (空 / nullptr は既定 L"assets")。
    const wchar_t* src = (root_directory != nullptr && root_directory[0] != L'\0')
                            ? root_directory : L"assets";
    usize i = 0;
    for (; src[i] != L'\0' && i + 1 < kMaxPathChars; ++i) {
        _root_directory[i] = src[i];
    }
    _root_directory[i] = L'\0';

    // current_directory は root 直下 (空文字)。
    _current_directory[0] = L'\0';

    _selected_index = -1;
    _filter_kind    = EAssetKind::Unknown;

    // pool に余裕を確保。これで RebuildEntries 中の AppendPathOffset /
    // AppendNameOffset で内部 Grow 頻度を下げる。Grow が走っても offset 基準で
    // 管理しているため pointer 無効化問題は起きない (列挙完走後に 1 度だけ
    // pool.Data() からの絶対 pointer を計算する設計のため)。
    _path_pool.Reserve(kInitialPathPoolBytes / sizeof(wchar_t));
    _name_pool.Reserve(kInitialPathPoolBytes / sizeof(char));

    // 初回 rescan。
    Refresh();
}

void AssetBrowser::Shutdown() noexcept {
    _entries.Clear();
    _path_pool.Clear();
    _name_pool.Clear();
    _root_directory[0]    = L'\0';
    _current_directory[0] = L'\0';
    _selected_index       = -1;
    _filter_kind          = EAssetKind::Unknown;
    // callback は外部所有 (関数ポインタ) なので、明示的に解除する。
    _on_selected_cb         = nullptr;
    _on_selected_user       = nullptr;
    _on_double_clicked_cb   = nullptr;
    _on_double_clicked_user = nullptr;
}

// ===========================================================================
// Refresh / RebuildEntries
// ===========================================================================
void AssetBrowser::Refresh() noexcept {
    RebuildEntries();
}

void AssetBrowser::RebuildEntries() noexcept {
    // 既存 entry / pool を全クリア (容量は維持)。pointer は全て無効化される。
    _entries.Clear();
    _path_pool.Clear();
    _name_pool.Clear();
    _selected_index = -1;  // pool 再構築で path pointer 寿命が切れるため

    // ----- Win32 FindFirstFileW で current_directory 配下を列挙 -----
    wchar_t full_dir[kMaxPathChars] = {};
    BuildFullPath(_current_directory, full_dir, kMaxPathChars);

    // "<dir>\\*" の検索パターン。
    wchar_t pattern[kMaxPathChars] = {};
    usize plen = 0;
    for (; full_dir[plen] != L'\0' && plen + 3 < kMaxPathChars; ++plen) {
        pattern[plen] = full_dir[plen];
    }
    // 末尾に '\\*' 付与 (末尾が '\\' or '/' で終わっていない場合のみ '\\' 追加)。
    if (plen > 0 && pattern[plen - 1] != L'\\' && pattern[plen - 1] != L'/') {
        pattern[plen++] = L'\\';
    }
    pattern[plen++] = L'*';
    pattern[plen]   = L'\0';

    WIN32_FIND_DATAW fd {};
    HANDLE h = ::FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        // ディレクトリ未存在 / アクセス拒否は空ツリーで終了 (= 安全 no-op)。
        return;
    }

    // ----- 重要: pool の Grow による pointer 無効化対策 ---------------
    // 列挙ループ中に AppendPathOffset / AppendNameOffset が複数回呼ばれ、
    // 内部で TArray::Reserve が走ると **既存** pool 内容ごと relocation され、
    // 過去 iteration で取った wchar_t* / char* が無効化される。これを
    // 避けるため、entry には offset を一時保管し、列挙完走後に
    // pool.Data() からの絶対ポインタへ変換する 2 段構えを取る。
    struct PendingEntry {
        usize       path_off  = 0;
        usize       name_off  = 0;
        EAssetKind  kind      = EAssetKind::Unknown;
        u64         size      = 0;
        u64         mtime     = 0;
        bool        is_dir    = false;
    };
    TArray<PendingEntry> pending {};

    do {
        // "." / ".." をスキップ。
        if (fd.cFileName[0] == L'.' && fd.cFileName[1] == L'\0') continue;
        if (fd.cFileName[0] == L'.' && fd.cFileName[1] == L'.'
            && fd.cFileName[2] == L'\0') continue;

        const bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u;

        // "<current_directory>\\<fileName>" の相対パスを組み立てる。
        wchar_t rel_path[kMaxPathChars] = {};
        usize rl = 0;
        for (; _current_directory[rl] != L'\0' && rl + 1 < kMaxPathChars; ++rl) {
            rel_path[rl] = _current_directory[rl];
        }
        // current_directory が空でなければ separator を挟む。
        if (rl > 0 && rel_path[rl - 1] != L'\\' && rel_path[rl - 1] != L'/'
            && rl + 1 < kMaxPathChars) {
            rel_path[rl++] = L'\\';
        }
        for (usize j = 0; fd.cFileName[j] != L'\0' && rl + 1 < kMaxPathChars; ++j) {
            rel_path[rl++] = fd.cFileName[j];
        }
        rel_path[rl] = L'\0';

        // pool への append は offset を覚えて行う (pointer は後で resolve)。
        PendingEntry pe {};
        pe.path_off = AppendPathOffset(rel_path);
        char utf8_name[kMaxPathChars] = {};
        WideToUtf8(fd.cFileName, utf8_name, static_cast<int>(kMaxPathChars));
        pe.name_off = AppendNameOffset(utf8_name);
        pe.is_dir   = is_dir;
        pe.kind     = is_dir ? EAssetKind::Unknown : ClassifyByExtension(rel_path);
        pe.mtime    = FileTimeToU64(fd.ftLastWriteTime);
        if (is_dir) {
            pe.size = 0;
        } else {
            LARGE_INTEGER sz {};
            sz.LowPart  = fd.nFileSizeLow;
            sz.HighPart = static_cast<LONG>(fd.nFileSizeHigh);
            pe.size = static_cast<u64>(sz.QuadPart);
        }
        pending.PushBack(pe);
    } while (::FindNextFileW(h, &fd) != FALSE);

    ::FindClose(h);

    // ---- offset → pointer 解決 (これ以後 pool は触らない) ----
    const wchar_t* path_base = _path_pool.IsEmpty() ? nullptr : &_path_pool[0];
    const char*    name_base = _name_pool.IsEmpty() ? nullptr : &_name_pool[0];
    _entries.Reserve(pending.Size());
    for (usize i = 0; i < pending.Size(); ++i) {
        const PendingEntry& pe = pending[i];
        AssetEntry e {};
        e.path            = (path_base != nullptr) ? (path_base + pe.path_off) : nullptr;
        e.short_name      = (name_base != nullptr) ? (name_base + pe.name_off) : nullptr;
        e.kind            = pe.kind;
        e.file_size_bytes = pe.size;
        e.last_modified   = pe.mtime;
        e.is_directory    = pe.is_dir;
        _entries.PushBack(e);
    }
}

// ===========================================================================
// アクセサ
// ===========================================================================
u32 AssetBrowser::EntryCount() const noexcept {
    return static_cast<u32>(_entries.Size());
}

const AssetEntry* AssetBrowser::GetEntry(u32 index) const noexcept {
    if (index >= _entries.Size()) return nullptr;
    return &_entries[static_cast<usize>(index)];
}

const wchar_t* AssetBrowser::CurrentDirectory() const noexcept {
    return _current_directory;
}

void AssetBrowser::SetCurrentDirectory(const wchar_t* path) noexcept {
    // path == nullptr / 空文字 → ルート (current_directory を空文字に)
    if (path == nullptr || path[0] == L'\0') {
        _current_directory[0] = L'\0';
        Refresh();
        return;
    }

    // 候補先のフルパスを組み立てて、ディレクトリ存在チェック。
    wchar_t full[kMaxPathChars] = {};
    BuildFullPath(path, full, kMaxPathChars);

    const DWORD attr = ::GetFileAttributesW(full);
    if (attr == INVALID_FILE_ATTRIBUTES
        || (attr & FILE_ATTRIBUTE_DIRECTORY) == 0u) {
        // 存在しない / ディレクトリでない → no-op (current_directory 維持)。
        return;
    }

    // path を _current_directory にコピー。
    usize i = 0;
    for (; path[i] != L'\0' && i + 1 < kMaxPathChars; ++i) {
        _current_directory[i] = path[i];
    }
    _current_directory[i] = L'\0';

    Refresh();
}

const wchar_t* AssetBrowser::SelectedAssetPath() const noexcept {
    if (_selected_index < 0) return nullptr;
    const u32 idx = static_cast<u32>(_selected_index);
    if (idx >= _entries.Size()) return nullptr;
    return _entries[idx].path;
}

EAssetKind AssetBrowser::SelectedAssetKind() const noexcept {
    if (_selected_index < 0) return EAssetKind::Unknown;
    const u32 idx = static_cast<u32>(_selected_index);
    if (idx >= _entries.Size()) return EAssetKind::Unknown;
    return _entries[idx].kind;
}

void AssetBrowser::SetOnAssetSelectedCallback(AssetSelectedCallback cb, void* user) noexcept {
    _on_selected_cb   = cb;
    _on_selected_user = user;
}

void AssetBrowser::SetOnAssetDoubleClickedCallback(AssetDoubleClickedCallback cb, void* user) noexcept {
    _on_double_clicked_cb   = cb;
    _on_double_clicked_user = user;
}

void AssetBrowser::SetFilterByKind(EAssetKind kind) noexcept {
    _filter_kind = kind;
}

// ===========================================================================
// ClassifyByExtension — 拡張子から EAssetKind を推定
// ===========================================================================
EAssetKind AssetBrowser::ClassifyByExtension(const wchar_t* path) noexcept {
    if (path == nullptr || path[0] == L'\0') return EAssetKind::Unknown;

    // Texture
    if (EndsWithIgnoreCase(path, L".png"))   return EAssetKind::Texture;
    if (EndsWithIgnoreCase(path, L".jpg"))   return EAssetKind::Texture;
    if (EndsWithIgnoreCase(path, L".jpeg"))  return EAssetKind::Texture;
    if (EndsWithIgnoreCase(path, L".tga"))   return EAssetKind::Texture;
    if (EndsWithIgnoreCase(path, L".bmp"))   return EAssetKind::Texture;
    if (EndsWithIgnoreCase(path, L".dds"))   return EAssetKind::Texture;
    if (EndsWithIgnoreCase(path, L".ktx"))   return EAssetKind::Texture;
    if (EndsWithIgnoreCase(path, L".hdr"))   return EAssetKind::Texture;
    // Mesh
    if (EndsWithIgnoreCase(path, L".mdl"))   return EAssetKind::Mesh;
    if (EndsWithIgnoreCase(path, L".fbx"))   return EAssetKind::Mesh;
    if (EndsWithIgnoreCase(path, L".gltf"))  return EAssetKind::Mesh;
    if (EndsWithIgnoreCase(path, L".glb"))   return EAssetKind::Mesh;
    if (EndsWithIgnoreCase(path, L".obj"))   return EAssetKind::Mesh;
    // Font
    if (EndsWithIgnoreCase(path, L".ttf"))   return EAssetKind::Font;
    if (EndsWithIgnoreCase(path, L".otf"))   return EAssetKind::Font;
    // Audio
    if (EndsWithIgnoreCase(path, L".wav"))   return EAssetKind::Audio;
    if (EndsWithIgnoreCase(path, L".ogg"))   return EAssetKind::Audio;
    if (EndsWithIgnoreCase(path, L".mp3"))   return EAssetKind::Audio;
    if (EndsWithIgnoreCase(path, L".flac"))  return EAssetKind::Audio;
    // Material
    if (EndsWithIgnoreCase(path, L".mat"))      return EAssetKind::Material;
    if (EndsWithIgnoreCase(path, L".material")) return EAssetKind::Material;
    // Particle
    if (EndsWithIgnoreCase(path, L".fx"))       return EAssetKind::Particle;
    if (EndsWithIgnoreCase(path, L".particle")) return EAssetKind::Particle;
    // Animation
    if (EndsWithIgnoreCase(path, L".anim"))     return EAssetKind::Animation;
    // BehaviorTree
    if (EndsWithIgnoreCase(path, L".bt"))       return EAssetKind::BehaviorTree;
    // Tilemap
    if (EndsWithIgnoreCase(path, L".tilemap"))  return EAssetKind::Tilemap;
    if (EndsWithIgnoreCase(path, L".tmx"))      return EAssetKind::Tilemap;
    // Prefab
    if (EndsWithIgnoreCase(path, L".prefab"))   return EAssetKind::Prefab;
    // Cinematic
    if (EndsWithIgnoreCase(path, L".cine"))     return EAssetKind::Cinematic;
    // Scene
    if (EndsWithIgnoreCase(path, L".scene"))    return EAssetKind::Scene;

    return EAssetKind::Unknown;
}

// ===========================================================================
// 内部ヘルパ
// ===========================================================================
void AssetBrowser::BuildFullPath(const wchar_t* sub, wchar_t* out_buf, usize cap) const noexcept {
    if (out_buf == nullptr || cap == 0) return;
    out_buf[0] = L'\0';

    usize w = 0;
    // root_directory コピー
    for (usize i = 0; _root_directory[i] != L'\0' && w + 1 < cap; ++i, ++w) {
        out_buf[w] = _root_directory[i];
    }
    // sub が空 (= ルート直下) ならここで終了。
    if (sub == nullptr || sub[0] == L'\0') {
        out_buf[w] = L'\0';
        return;
    }
    // separator (root が空でなければ '\\' を 1 つ挟む)。
    if (w > 0 && out_buf[w - 1] != L'\\' && out_buf[w - 1] != L'/' && w + 1 < cap) {
        out_buf[w++] = L'\\';
    }
    // sub コピー
    for (usize i = 0; sub[i] != L'\0' && w + 1 < cap; ++i, ++w) {
        out_buf[w] = sub[i];
    }
    out_buf[w] = L'\0';
}

usize AssetBrowser::AppendPathOffset(const wchar_t* src) noexcept {
    if (src == nullptr) return 0;
    const usize len = WLen(src) + 1u;  // 終端 0 含む

    // 不足時は予防的に余裕を持って Grow させ、書き込み中の inner PushBack
    // が更に relocation を起こさないようにする (= 1 回の Reserve でこの append
    // を完了する分の容量を確保する)。
    const usize need = _path_pool.Size() + len;
    if (need > _path_pool.Capacity()) {
        usize new_cap = _path_pool.Capacity();
        if (new_cap == 0) new_cap = kInitialPathPoolBytes / sizeof(wchar_t);
        while (new_cap < need) new_cap *= 2u;
        _path_pool.Reserve(new_cap);
    }

    const usize start = _path_pool.Size();
    for (usize i = 0; i < len; ++i) {
        _path_pool.PushBack(src[i]);
    }
    return start;
}

usize AssetBrowser::AppendNameOffset(const char* src) noexcept {
    if (src == nullptr) return 0;
    const usize len = std::strlen(src) + 1u;

    const usize need = _name_pool.Size() + len;
    if (need > _name_pool.Capacity()) {
        usize new_cap = _name_pool.Capacity();
        if (new_cap == 0) new_cap = kInitialPathPoolBytes / sizeof(char);
        while (new_cap < need) new_cap *= 2u;
        _name_pool.Reserve(new_cap);
    }

    const usize start = _name_pool.Size();
    for (usize i = 0; i < len; ++i) {
        _name_pool.PushBack(src[i]);
    }
    return start;
}

// ===========================================================================
// DrawUI — ImGui window
// ===========================================================================
// レイアウト:
//   ┌─ "Asset Browser" window ───────────────────────────────────────┐
//   │ [Refresh] [Up]  Path: <current>     [Filter: <kind combo>]     │
//   │ ─────────────────────────────────────────────────────────────  │
//   │ ┌─ left (tree) ────┐ ┌─ right (list) ────────────────────────┐ │
//   │ │ ▼ <root>          │ │ [DIR ] subfolder/                       │ │
//   │ │   ▼ subfolder/    │ │ [TEX ] foo.png          12.3 KB         │ │
//   │ │     ...           │ │ ...                                      │ │
//   │ └───────────────────┘ └──────────────────────────────────────────┘ │
//   └────────────────────────────────────────────────────────────────────┘
// ===========================================================================
void AssetBrowser::DrawUI() noexcept {
    if (!ImGui::Begin("Asset Browser")) {
        ImGui::End();
        return;
    }

    // ----- Toolbar -----
    if (ImGui::Button("Refresh")) {
        Refresh();
    }
    ImGui::SameLine();

    const bool at_root = (_current_directory[0] == L'\0');
    if (at_root) ImGui::BeginDisabled();
    if (ImGui::Button("Up")) {
        // current_directory の最後の '\\' / '/' 以降を削る。
        usize len = WLen(_current_directory);
        if (len > 0) {
            isize cut = -1;
            for (isize i = static_cast<isize>(len) - 1; i >= 0; --i) {
                if (_current_directory[i] == L'\\' || _current_directory[i] == L'/') {
                    cut = i;
                    break;
                }
            }
            if (cut >= 0) {
                _current_directory[cut] = L'\0';
            } else {
                _current_directory[0] = L'\0';  // ルート直下
            }
            Refresh();
        }
    }
    if (at_root) ImGui::EndDisabled();

    // current path 表示 (UTF-8 変換)。
    ImGui::SameLine();
    char cur_utf8[kMaxPathChars] = {};
    if (at_root) {
        std::snprintf(cur_utf8, sizeof(cur_utf8), "Path: <root>");
    } else {
        char tmp[kMaxPathChars] = {};
        WideToUtf8(_current_directory, tmp, static_cast<int>(kMaxPathChars));
        std::snprintf(cur_utf8, sizeof(cur_utf8), "Path: %s", tmp);
    }
    ImGui::TextUnformatted(cur_utf8);

    // ----- Filter combo (右端寄せはしない、SameLine で続ける) -----
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    const char* current_filter = KindLabel(_filter_kind);
    if (ImGui::BeginCombo("Filter", current_filter)) {
        static const EAssetKind kAll[] = {
            EAssetKind::Unknown,  // = フィルタ解除
            EAssetKind::Texture, EAssetKind::Mesh, EAssetKind::Font,
            EAssetKind::Audio, EAssetKind::Material, EAssetKind::Particle,
            EAssetKind::Animation, EAssetKind::BehaviorTree, EAssetKind::Tilemap,
            EAssetKind::Prefab, EAssetKind::Cinematic, EAssetKind::Scene,
            EAssetKind::Other,
        };
        for (usize i = 0; i < sizeof(kAll) / sizeof(kAll[0]); ++i) {
            const bool selected = (kAll[i] == _filter_kind);
            const char* lbl = (kAll[i] == EAssetKind::Unknown) ? "(All)" : KindLabel(kAll[i]);
            if (ImGui::Selectable(lbl, selected)) {
                _filter_kind = kAll[i];
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Separator();

    // ----- 2 カラムレイアウト -----
    const float content_w = ImGui::GetContentRegionAvail().x;
    const float left_w    = (content_w > 480.0f) ? 220.0f : content_w * 0.4f;

    // ===== 左カラム: tree (root から再帰) =====
    ImGui::BeginChild("##asset_tree", ImVec2(left_w, 0), true);
    {
        DrawTreeRecursive(L"", 0u);
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ===== 右カラム: current directory の entry リスト =====
    ImGui::BeginChild("##asset_list", ImVec2(0, 0), true);
    {
        DrawList();
    }
    ImGui::EndChild();

    ImGui::End();
}

// ===========================================================================
// DrawTreeRecursive — 左ペインの tree (current_directory の選択 UI)
// ===========================================================================
// 左 tree は「ディレクトリの選択」を担う (= ファイルは表示しない / 表示しても
// 役に立たないので省略)。ノードクリックで SetCurrentDirectory する。
//
// 注意: tree は描画フレームごとにディレクトリ列挙を「ノード展開時のみ」
// 行う ImGui パターンを採用。これでルート以下を毎フレーム全列挙する
// コストを抑える。実装は再帰関数を tree 構造でなく「展開時に都度
// FindFirstFile する」形にする。
// ===========================================================================
void AssetBrowser::DrawTreeRecursive(const wchar_t* rel_dir, u32 depth) noexcept {
    if (depth >= 32u) {
        ImGui::TextDisabled("(depth limit)");
        return;
    }

    // 「自分」のラベル決定。rel_dir 空文字 = ルート (= root_directory の末尾)。
    char label[kMaxPathChars] = {};
    if (rel_dir == nullptr || rel_dir[0] == L'\0') {
        // root のベース名を表示。
        char root_utf8[kMaxPathChars] = {};
        WideToUtf8(WBaseName(_root_directory), root_utf8, static_cast<int>(kMaxPathChars));
        std::snprintf(label, sizeof(label), "%s", root_utf8[0] ? root_utf8 : "assets");
    } else {
        char rel_utf8[kMaxPathChars] = {};
        WideToUtf8(WBaseName(rel_dir), rel_utf8, static_cast<int>(kMaxPathChars));
        std::snprintf(label, sizeof(label), "%s", rel_utf8);
    }

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                             | ImGuiTreeNodeFlags_OpenOnDoubleClick
                             | ImGuiTreeNodeFlags_SpanAvailWidth;
    // current_directory と一致するノードはハイライト。
    const wchar_t* cd = _current_directory;
    bool selected_node = false;
    if ((rel_dir == nullptr || rel_dir[0] == L'\0') && cd[0] == L'\0') {
        selected_node = true;
    } else if (rel_dir != nullptr) {
        // 完全一致比較
        usize i = 0;
        while (rel_dir[i] != L'\0' && cd[i] != L'\0' && rel_dir[i] == cd[i]) ++i;
        if (rel_dir[i] == L'\0' && cd[i] == L'\0') selected_node = true;
    }
    if (selected_node) flags |= ImGuiTreeNodeFlags_Selected;

    // ImGui PushID で衝突回避 (rel_dir ポインタを ID 化)。
    ImGui::PushID(static_cast<const void*>(rel_dir != nullptr ? rel_dir : L""));
    const bool open = ImGui::TreeNodeEx("##at_node", flags, "%s", label);

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)
        && !ImGui::IsItemToggledOpen()) {
        // クリック = current_directory 切替。
        SetCurrentDirectory(rel_dir != nullptr ? rel_dir : L"");
    }

    if (open) {
        // 展開時のみサブディレクトリを列挙する (子のみ ─ ファイルは省略)。
        wchar_t full_dir[kMaxPathChars] = {};
        BuildFullPath(rel_dir, full_dir, kMaxPathChars);
        wchar_t pattern[kMaxPathChars] = {};
        usize plen = 0;
        for (; full_dir[plen] != L'\0' && plen + 3 < kMaxPathChars; ++plen) {
            pattern[plen] = full_dir[plen];
        }
        if (plen > 0 && pattern[plen - 1] != L'\\' && pattern[plen - 1] != L'/') {
            pattern[plen++] = L'\\';
        }
        pattern[plen++] = L'*';
        pattern[plen]   = L'\0';

        WIN32_FIND_DATAW fd {};
        HANDLE h = ::FindFirstFileW(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.cFileName[0] == L'.' && fd.cFileName[1] == L'\0') continue;
                if (fd.cFileName[0] == L'.' && fd.cFileName[1] == L'.'
                    && fd.cFileName[2] == L'\0') continue;
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0u) continue;

                // 子の rel_path = rel_dir + '\\' + fd.cFileName を組み立てる
                // (stack 上に置く → DrawTreeRecursive へ渡す)。
                wchar_t child_rel[kMaxPathChars] = {};
                usize cr = 0;
                if (rel_dir != nullptr) {
                    for (; rel_dir[cr] != L'\0' && cr + 1 < kMaxPathChars; ++cr) {
                        child_rel[cr] = rel_dir[cr];
                    }
                }
                if (cr > 0 && child_rel[cr - 1] != L'\\' && child_rel[cr - 1] != L'/'
                    && cr + 1 < kMaxPathChars) {
                    child_rel[cr++] = L'\\';
                }
                for (usize j = 0; fd.cFileName[j] != L'\0' && cr + 1 < kMaxPathChars; ++j) {
                    child_rel[cr++] = fd.cFileName[j];
                }
                child_rel[cr] = L'\0';

                DrawTreeRecursive(child_rel, depth + 1u);
            } while (::FindNextFileW(h, &fd) != FALSE);
            ::FindClose(h);
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
}

// ===========================================================================
// DrawList — 右ペインの current directory のエントリ一覧
// ===========================================================================
void AssetBrowser::DrawList() noexcept {
    ImGui::Text("Entries: %u%s",
                static_cast<unsigned>(_entries.Size()),
                (_filter_kind == EAssetKind::Unknown) ? "" : " (filtered)");
    ImGui::Separator();

    for (u32 i = 0; i < _entries.Size(); ++i) {
        const AssetEntry& e = _entries[static_cast<usize>(i)];

        // kind フィルタ (ディレクトリは常に表示)。
        if (_filter_kind != EAssetKind::Unknown
            && !e.is_directory
            && e.kind != _filter_kind) {
            continue;
        }

        // 行ラベル: "[TAG ] short_name      <size>"
        char row_label[kMaxPathChars] = {};
        if (e.is_directory) {
            std::snprintf(row_label, sizeof(row_label), "[DIR ] %s/",
                          e.short_name ? e.short_name : "");
        } else {
            // size 表示 (KB 単位、1024 未満は B)。
            char size_buf[32] = {};
            if (e.file_size_bytes < 1024ull) {
                std::snprintf(size_buf, sizeof(size_buf), "%llu B",
                              static_cast<unsigned long long>(e.file_size_bytes));
            } else if (e.file_size_bytes < 1024ull * 1024ull) {
                std::snprintf(size_buf, sizeof(size_buf), "%.1f KB",
                              static_cast<double>(e.file_size_bytes) / 1024.0);
            } else {
                std::snprintf(size_buf, sizeof(size_buf), "%.1f MB",
                              static_cast<double>(e.file_size_bytes) / (1024.0 * 1024.0));
            }
            std::snprintf(row_label, sizeof(row_label), "[%s] %-32s %s",
                          KindTag(e.kind),
                          e.short_name ? e.short_name : "",
                          size_buf);
        }

        const bool selected = (static_cast<i32>(i) == _selected_index);
        ImGui::PushID(static_cast<int>(i));

        if (ImGui::Selectable(row_label, selected,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
            _selected_index = static_cast<i32>(i);
            if (_on_selected_cb != nullptr) {
                _on_selected_cb(_on_selected_user, e.path, e.kind);
            }
            // ダブルクリック検知 (Selectable_AllowDoubleClick が必要)
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (e.is_directory) {
                    // ディレクトリは current_directory として navigate。
                    // SetCurrentDirectory が内部で Refresh して pool を丸ごと
                    // 再構築するため `e.path` は終了後に dangling になる。
                    // 注: callback は **先** に発火させる (= e.path がまだ有効
                    // な間) → その後 navigate して return。
                    if (_on_double_clicked_cb != nullptr) {
                        _on_double_clicked_cb(_on_double_clicked_user, e.path, e.kind);
                    }
                    SetCurrentDirectory(e.path);
                    // この後の _entries 走査は安全に中断する (次フレームで
                    // 再描画され、新しい _entries に対して回る)。
                    ImGui::PopID();
                    return;
                } else {
                    if (_on_double_clicked_cb != nullptr) {
                        _on_double_clicked_cb(_on_double_clicked_user, e.path, e.kind);
                    }
                }
            }
        }

        // ----- Drag source: AssetEntry::path を ASSET_PATH payload に -----
        // ディレクトリも drag source として提供する (panel 側で folder drop を
        // 受け入れたい場合がある = batch import 等)。
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            const wchar_t* p = e.path;  // pointer の寿命は次回 Refresh まで
            ImGui::SetDragDropPayload(kDragDropPayloadId, &p, sizeof(p));
            ImGui::TextUnformatted(row_label);
            ImGui::EndDragDropSource();
        }

        ImGui::PopID();
    }
}

} // namespace acs::game::editor_core
