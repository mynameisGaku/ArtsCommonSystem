// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar K (editor_core) — FAssetBrowser 実装
//
// 仕様の意図は FAssetBrowser.h を参照。本ファイルでは:
//   ・FindFirstFileW / FindNextFileW で assets/ 配下を列挙
//   ・拡張子 lookup で EAssetKind 推定
//   ・ImGui を使った左 tree + 右 list の 2 ペインレイアウト
//   ・Drag Source ("ASSET_PATH" payload = wchar_t*) 発行
// を実装する。すべて noexcept、STL 不使用、ImGui include は本 .cpp 限定。
#include "gameframework/tools/editor_core/AssetBrowser.h"

#include "foundation/Platform.h"

// <windows.h> マクロ汚染対策: `SetCurrentDirectory` がデフォルトで
// `SetCurrentDirectoryW` に macro 展開され、FAssetBrowser::SetCurrentDirectory
// メソッドの定義側と衝突する。本 .cpp ではメソッド名を維持するため undef。
#ifdef SetCurrentDirectory
#undef SetCurrentDirectory
#endif

#include <imgui.h>

#include <cstdio>   // snprintf (ラベル整形)
#include <cstring>  // strlen / memcpy (UTF-8 比較用)

namespace acs::game::editor_core {

namespace {

/**
 * wchar_t 列の長さ (終端含まず) を返す。
 *
 * @param s 対象文字列 (nullptr なら 0)。
 * @return 終端を含まない文字数。
 */
usize WLen(const wchar_t* s) noexcept {
    if (s == nullptr) return 0;
    usize n = 0;
    while (s[n] != L'\0') ++n;
    return n;
}

/**
 * wchar_t を ASCII 比較用に小文字化する ('A'〜'Z' のみ対応)。
 *
 * @param c 変換対象の文字。
 * @return 小文字化した文字 ('A'〜'Z' 以外はそのまま)。
 */
wchar_t WLower(wchar_t c) noexcept {
    if (c >= L'A' && c <= L'Z') return static_cast<wchar_t>(c - L'A' + L'a');
    return c;
}

/**
 * path の末尾が ext と一致するか大文字小文字無視で判定する。
 *
 * @param path 判定対象のパス。
 * @param ext 比較する拡張子 (L".png" 形式の終端 0 付き)。
 * @return path の末尾が ext と一致すれば true。
 */
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

/**
 * path 末尾の '\\' / '/' 以降 (= ベース名) へのポインタを返す。
 *
 * @param path 対象パス (nullptr なら nullptr)。
 * @return 最後の区切り以降を指すポインタ (区切りが無ければ path 自身)。
 */
const wchar_t* WBaseName(const wchar_t* path) noexcept {
    if (path == nullptr) return nullptr;
    const wchar_t* last = path;
    for (const wchar_t* p = path; *p != L'\0'; ++p) {
        if (*p == L'\\' || *p == L'/') last = p + 1;
    }
    return last;
}

/**
 * wchar_t 文字列を UTF-8 に変換して out_buf へ書き込む (終端 0 含む)。
 *
 * @details 失敗時は out_buf[0] = '\0' とする。
 * @param src 変換元の wchar_t 文字列。
 * @param out_buf 書き込み先バッファ。
 * @param out_cap out_buf の容量 (バイト)。
 */
void WideToUtf8(const wchar_t* src, char* out_buf, int out_cap) noexcept {
    if (out_buf == nullptr || out_cap <= 0) return;
    out_buf[0] = '\0';
    if (src == nullptr) return;
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, src, -1, out_buf, out_cap, nullptr, nullptr);
    if (n <= 0) out_buf[0] = '\0';
    out_buf[out_cap - 1] = '\0';  // 念のため終端保証
}

/**
 * Win32 FILETIME を u64 (100ns 単位) に変換する。
 *
 * @param ft 変換元の FILETIME。
 * @return high/low を結合した 100ns 単位の時刻値。
 */
u64 FileTimeToU64(const FILETIME& ft) noexcept {
    return (static_cast<u64>(ft.dwHighDateTime) << 32) | static_cast<u64>(ft.dwLowDateTime);
}

/**
 * EAssetKind の表示名を返す (Filter combo 等に使う)。
 *
 * @param k 種別。
 * @return 種別の表示名文字列 (未知は "Unknown")。
 */
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
        case EAssetKind::BehaviorTree: return "Behavior Tree";
        case EAssetKind::Tilemap:      return "Tilemap";
        case EAssetKind::Prefab:       return "Prefab";
        case EAssetKind::Cinematic:    return "Cinematic";
        case EAssetKind::Scene:        return "Scene";
        case EAssetKind::Other:        return "Other";
    }
    return "Unknown";
}

/**
 * list 行の左端に表示する固定幅タグ ("TEX " 等) を返す。
 *
 * @param k 種別。
 * @return 4 文字の種別タグ (未知は "??? ")。
 */
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

/** root_directory を記録し pool を Reserve して初回 Refresh を実行する。 */
void FAssetBrowser::Init(const wchar_t* root_directory) noexcept {
    // m_RootDirectory にコピー (空 / nullptr は既定 L"assets")。
    const wchar_t* src = (root_directory != nullptr && root_directory[0] != L'\0')
                            ? root_directory : L"assets";
    usize i = 0;
    for (; src[i] != L'\0' && i + 1 < kMaxPathChars; ++i) {
        m_RootDirectory[i] = src[i];
    }
    m_RootDirectory[i] = L'\0';

    // current_directory は root 直下 (空文字)。
    m_CurrentDirectory[0] = L'\0';

    m_SelectedIndex = -1;
    m_FilterKind    = EAssetKind::Unknown;

    // pool に余裕を確保。これで RebuildEntries 中の AppendPathOffset /
    // AppendNameOffset で内部 Grow 頻度を下げる。Grow が走っても offset 基準で
    // 管理しているため pointer 無効化問題は起きない (列挙完走後に 1 度だけ
    // pool.Data() からの絶対 pointer を計算する設計のため)。
    m_PathPool.Reserve(kInitialPathPoolBytes / sizeof(wchar_t));
    m_NamePool.Reserve(kInitialPathPoolBytes / sizeof(char));

    // 初回 rescan。
    Refresh();
}

/** pool / TArray / ディレクトリ文字列 / callback を全クリアする。 */
void FAssetBrowser::Shutdown() noexcept {
    m_Entries.Clear();
    m_PathPool.Clear();
    m_NamePool.Clear();
    m_RootDirectory[0]    = L'\0';
    m_CurrentDirectory[0] = L'\0';
    m_SelectedIndex       = -1;
    m_FilterKind          = EAssetKind::Unknown;
    // callback は外部所有 (関数ポインタ) なので、明示的に解除する。
    m_OnSelectedCb         = nullptr;
    m_OnSelectedUser       = nullptr;
    m_OnDoubleClickedCb   = nullptr;
    m_OnDoubleClickedUser = nullptr;
}

/** current_directory 配下を再列挙する (RebuildEntries への委譲)。 */
void FAssetBrowser::Refresh() noexcept {
    RebuildEntries();
}

/** Win32 列挙で m_Entries / pool を作り直し、offset を絶対 pointer へ解決する。 */
void FAssetBrowser::RebuildEntries() noexcept {
    // 既存 entry / pool を全クリア (容量は維持)。pointer は全て無効化される。
    m_Entries.Clear();
    m_PathPool.Clear();
    m_NamePool.Clear();
    m_SelectedIndex = -1;  // pool 再構築で path pointer 寿命が切れるため

    // Win32 FindFirstFileW で current_directory 配下を列挙。
    wchar_t full_dir[kMaxPathChars] = {};
    BuildFullPath(m_CurrentDirectory, full_dir, kMaxPathChars);

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

    // 重要: pool の Grow による pointer 無効化対策。
    // 列挙ループ中に AppendPathOffset / AppendNameOffset が複数回呼ばれ、
    // 内部で TArray::Reserve が走ると **既存** pool 内容ごと relocation され、
    // 過去 iteration で取った wchar_t* / char* が無効化される。これを
    // 避けるため、entry には offset を一時保管し、列挙完走後に
    // pool.Data() からの絶対ポインタへ変換する 2 段構えを取る。
    struct FPendingEntry {
        usize       path_off  = 0;
        usize       name_off  = 0;
        EAssetKind  kind      = EAssetKind::Unknown;
        u64         size      = 0;
        u64         mtime     = 0;
        bool        is_dir    = false;
    };
    TArray<FPendingEntry> pending {};

    do {
        // "." / ".." をスキップ。
        if (fd.cFileName[0] == L'.' && fd.cFileName[1] == L'\0') continue;
        if (fd.cFileName[0] == L'.' && fd.cFileName[1] == L'.'
            && fd.cFileName[2] == L'\0') continue;

        const bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u;

        // "<current_directory>\\<fileName>" の相対パスを組み立てる。
        wchar_t rel_path[kMaxPathChars] = {};
        usize rl = 0;
        for (; m_CurrentDirectory[rl] != L'\0' && rl + 1 < kMaxPathChars; ++rl) {
            rel_path[rl] = m_CurrentDirectory[rl];
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
        FPendingEntry pe {};
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

    // offset → pointer 解決 (これ以後 pool は触らない)。
    const wchar_t* path_base = m_PathPool.IsEmpty() ? nullptr : &m_PathPool[0];
    const char*    name_base = m_NamePool.IsEmpty() ? nullptr : &m_NamePool[0];
    m_Entries.Reserve(pending.Size());
    for (usize i = 0; i < pending.Size(); ++i) {
        const FPendingEntry& pe = pending[i];
        FAssetEntry e {};
        e.path            = (path_base != nullptr) ? (path_base + pe.path_off) : nullptr;
        e.short_name      = (name_base != nullptr) ? (name_base + pe.name_off) : nullptr;
        e.kind            = pe.kind;
        e.file_size_bytes = pe.size;
        e.last_modified   = pe.mtime;
        e.is_directory    = pe.is_dir;
        m_Entries.PushBack(e);
    }
}

/** current directory の entry 件数を返す。 */
u32 FAssetBrowser::EntryCount() const noexcept {
    return static_cast<u32>(m_Entries.Size());
}

/** index 番目の entry を返す (範囲外は nullptr)。 */
const FAssetEntry* FAssetBrowser::GetEntry(u32 index) const noexcept {
    if (index >= m_Entries.Size()) return nullptr;
    return &m_Entries[static_cast<usize>(index)];
}

/** 現在表示中のディレクトリ (root 相対) を返す。 */
const wchar_t* FAssetBrowser::CurrentDirectory() const noexcept {
    return m_CurrentDirectory;
}

/** path がディレクトリとして存在すれば current_directory を切り替えて Refresh する。 */
void FAssetBrowser::SetCurrentDirectory(const wchar_t* path) noexcept {
    // path == nullptr / 空文字 → ルート (current_directory を空文字に)
    if (path == nullptr || path[0] == L'\0') {
        m_CurrentDirectory[0] = L'\0';
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

    // path を m_CurrentDirectory にコピー。
    usize i = 0;
    for (; path[i] != L'\0' && i + 1 < kMaxPathChars; ++i) {
        m_CurrentDirectory[i] = path[i];
    }
    m_CurrentDirectory[i] = L'\0';

    Refresh();
}

/** 選択中 entry の path を返す (未選択 / 範囲外は nullptr)。 */
const wchar_t* FAssetBrowser::SelectedAssetPath() const noexcept {
    if (m_SelectedIndex < 0) return nullptr;
    const u32 idx = static_cast<u32>(m_SelectedIndex);
    if (idx >= m_Entries.Size()) return nullptr;
    return m_Entries[idx].path;
}

/** 選択中 entry の kind を返す (未選択 / 範囲外は EAssetKind::Unknown)。 */
EAssetKind FAssetBrowser::SelectedAssetKind() const noexcept {
    if (m_SelectedIndex < 0) return EAssetKind::Unknown;
    const u32 idx = static_cast<u32>(m_SelectedIndex);
    if (idx >= m_Entries.Size()) return EAssetKind::Unknown;
    return m_Entries[idx].kind;
}

/** 選択変更通知 callback と user ポインタを登録する。 */
void FAssetBrowser::SetOnAssetSelectedCallback(AssetSelectedCallback cb, void* user) noexcept {
    m_OnSelectedCb   = cb;
    m_OnSelectedUser = user;
}

/** ダブルクリック通知 callback と user ポインタを登録する。 */
void FAssetBrowser::SetOnAssetDoubleClickedCallback(AssetDoubleClickedCallback cb, void* user) noexcept {
    m_OnDoubleClickedCb   = cb;
    m_OnDoubleClickedUser = user;
}

/** kind フィルタを設定する (Unknown でフィルタ解除)。 */
void FAssetBrowser::SetFilterByKind(EAssetKind kind) noexcept {
    m_FilterKind = kind;
}

/** 拡張子を順に照合して EAssetKind を推定する (大文字小文字無視)。 */
EAssetKind FAssetBrowser::ClassifyByExtension(const wchar_t* path) noexcept {
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
    // FFont
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
    // FAnimation
    if (EndsWithIgnoreCase(path, L".anim"))     return EAssetKind::Animation;
    // FBehaviorTree
    if (EndsWithIgnoreCase(path, L".bt"))       return EAssetKind::BehaviorTree;
    // FTilemap
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

/** root_directory に sub を結合して out_buf へ書き込む (separator を補う)。 */
void FAssetBrowser::BuildFullPath(const wchar_t* sub, wchar_t* out_buf, usize cap) const noexcept {
    if (out_buf == nullptr || cap == 0) return;
    out_buf[0] = L'\0';

    usize w = 0;
    // root_directory コピー
    for (usize i = 0; m_RootDirectory[i] != L'\0' && w + 1 < cap; ++i, ++w) {
        out_buf[w] = m_RootDirectory[i];
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

/** m_PathPool に wchar_t 文字列を追記し、必要なら先に Reserve して offset を返す。 */
usize FAssetBrowser::AppendPathOffset(const wchar_t* src) noexcept {
    if (src == nullptr) return 0;
    const usize len = WLen(src) + 1u;  // 終端 0 含む

    // 不足時は予防的に余裕を持って Grow させ、書き込み中の inner PushBack
    // が更に relocation を起こさないようにする (= 1 回の Reserve でこの append
    // を完了する分の容量を確保する)。
    const usize need = m_PathPool.Size() + len;
    if (need > m_PathPool.Capacity()) {
        usize new_cap = m_PathPool.Capacity();
        if (new_cap == 0) new_cap = kInitialPathPoolBytes / sizeof(wchar_t);
        while (new_cap < need) new_cap *= 2u;
        m_PathPool.Reserve(new_cap);
    }

    const usize start = m_PathPool.Size();
    for (usize i = 0; i < len; ++i) {
        m_PathPool.PushBack(src[i]);
    }
    return start;
}

/** m_NamePool に UTF-8 文字列を追記し、必要なら先に Reserve して offset を返す。 */
usize FAssetBrowser::AppendNameOffset(const char* src) noexcept {
    if (src == nullptr) return 0;
    const usize len = std::strlen(src) + 1u;

    const usize need = m_NamePool.Size() + len;
    if (need > m_NamePool.Capacity()) {
        usize new_cap = m_NamePool.Capacity();
        if (new_cap == 0) new_cap = kInitialPathPoolBytes / sizeof(char);
        while (new_cap < need) new_cap *= 2u;
        m_NamePool.Reserve(new_cap);
    }

    const usize start = m_NamePool.Size();
    for (usize i = 0; i < len; ++i) {
        m_NamePool.PushBack(src[i]);
    }
    return start;
}

/** ツールバー + 左 tree + 右 list を 1 つの ImGui window に描画する。 */
void FAssetBrowser::DrawUI() noexcept {
    if (!ImGui::Begin("Asset Browser")) {
        ImGui::End();
        return;
    }

    // Toolbar
    if (ImGui::Button("Refresh")) {
        Refresh();
    }
    ImGui::SameLine();

    const bool at_root = (m_CurrentDirectory[0] == L'\0');
    if (at_root) ImGui::BeginDisabled();
    if (ImGui::Button("Up")) {
        // current_directory の最後の '\\' / '/' 以降を削る。
        const usize len = WLen(m_CurrentDirectory);
        if (len > 0) {
            isize cut = -1;
            for (isize i = static_cast<isize>(len) - 1; i >= 0; --i) {
                if (m_CurrentDirectory[i] == L'\\' || m_CurrentDirectory[i] == L'/') {
                    cut = i;
                    break;
                }
            }
            if (cut >= 0) {
                m_CurrentDirectory[cut] = L'\0';
            } else {
                m_CurrentDirectory[0] = L'\0';  // ルート直下
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
        WideToUtf8(m_CurrentDirectory, tmp, static_cast<int>(kMaxPathChars));
        std::snprintf(cur_utf8, sizeof(cur_utf8), "Path: %s", tmp);
    }
    ImGui::TextUnformatted(cur_utf8);

    // Filter combo (右端寄せはしない、SameLine で続ける)。
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    const char* current_filter = KindLabel(m_FilterKind);
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
            const bool selected = (kAll[i] == m_FilterKind);
            const char* lbl = (kAll[i] == EAssetKind::Unknown) ? "(All)" : KindLabel(kAll[i]);
            if (ImGui::Selectable(lbl, selected)) {
                m_FilterKind = kAll[i];
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Separator();

    // 2 カラムレイアウト。
    const float content_w = ImGui::GetContentRegionAvail().x;
    const float left_w    = (content_w > 480.0f) ? 220.0f : content_w * 0.4f;

    // 左カラム: tree (root から再帰)。
    ImGui::BeginChild("##asset_tree", ImVec2(left_w, 0), true);
    {
        DrawTreeRecursive(L"", 0u);
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // 右カラム: current directory の entry リスト。
    ImGui::BeginChild("##asset_list", ImVec2(0, 0), true);
    {
        DrawList();
    }
    ImGui::EndChild();

    ImGui::End();
}

/**
 * 左ペインの tree を再帰描画し、ノードクリックで current_directory を切り替える。
 *
 * @details
 * 左 tree はディレクトリの選択のみを担い (ファイルは省略)、サブディレクトリ列挙はノード
 * 展開時にのみ FindFirstFile で行う ImGui パターンで、毎フレーム全列挙のコストを抑える。
 * @param rel_dir assets/ ルートからの相対パス (空文字でルート)。
 * @param depth 現在の再帰深度 (32 で打ち切り)。
 */
void FAssetBrowser::DrawTreeRecursive(const wchar_t* rel_dir, u32 depth) noexcept {
    if (depth >= 32u) {
        ImGui::TextDisabled("(depth limit)");
        return;
    }

    // 「自分」のラベル決定。rel_dir 空文字 = ルート (= root_directory の末尾)。
    char label[kMaxPathChars] = {};
    if (rel_dir == nullptr || rel_dir[0] == L'\0') {
        // root のベース名を表示。
        char root_utf8[kMaxPathChars] = {};
        WideToUtf8(WBaseName(m_RootDirectory), root_utf8, static_cast<int>(kMaxPathChars));
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
    const wchar_t* cd = m_CurrentDirectory;
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
        // 展開時のみサブディレクトリを列挙する (子のみ。ファイルは省略)。
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

/** current directory の各 entry を kind フィルタ越しに行描画し、選択 / DnD / ダブルクリックを処理する。 */
void FAssetBrowser::DrawList() noexcept {
    ImGui::Text("Entries: %u%s",
                static_cast<unsigned>(m_Entries.Size()),
                (m_FilterKind == EAssetKind::Unknown) ? "" : " (filtered)");
    ImGui::Separator();

    for (u32 i = 0; i < m_Entries.Size(); ++i) {
        const FAssetEntry& e = m_Entries[static_cast<usize>(i)];

        // kind フィルタ (ディレクトリは常に表示)。
        if (m_FilterKind != EAssetKind::Unknown
            && !e.is_directory
            && e.kind != m_FilterKind) {
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

        const bool selected = (static_cast<i32>(i) == m_SelectedIndex);
        ImGui::PushID(static_cast<int>(i));

        if (ImGui::Selectable(row_label, selected,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
            m_SelectedIndex = static_cast<i32>(i);
            if (m_OnSelectedCb != nullptr) {
                m_OnSelectedCb(m_OnSelectedUser, e.path, e.kind);
            }
            // ダブルクリック検知 (Selectable_AllowDoubleClick が必要)
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (e.is_directory) {
                    // ディレクトリは current_directory として navigate。
                    // SetCurrentDirectory が内部で Refresh して pool を丸ごと
                    // 再構築するため `e.path` は終了後に dangling になる。
                    // 注: callback は **先** に発火させる (= e.path がまだ有効
                    // な間) → その後 navigate して return。
                    if (m_OnDoubleClickedCb != nullptr) {
                        m_OnDoubleClickedCb(m_OnDoubleClickedUser, e.path, e.kind);
                    }
                    SetCurrentDirectory(e.path);
                    // この後の m_Entries 走査は安全に中断する (次フレームで
                    // 再描画され、新しい m_Entries に対して回る)。
                    ImGui::PopID();
                    return;
                } else {
                    if (m_OnDoubleClickedCb != nullptr) {
                        m_OnDoubleClickedCb(m_OnDoubleClickedUser, e.path, e.kind);
                    }
                }
            }
        }

        // Drag source: AssetEntry::path を ASSET_PATH payload に載せる。
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
