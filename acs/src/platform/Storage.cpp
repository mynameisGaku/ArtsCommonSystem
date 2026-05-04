// Storage 実装（INI 形式 key=value、UTF-8）
#include "platform/Storage.h"
#include "platform/FileSystem.h"
#include "foundation/Compiler.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

#if ACS_PLATFORM_WINDOWS
    #include <Windows.h>
    #include <ShlObj.h>
    // CreateDirectory マクロが FileSystem::CreateDirectory と衝突するので解除
    #ifdef CreateDirectory
        #undef CreateDirectory
    #endif
#elif ACS_PLATFORM_POSIX
    #include <cstdlib>     // getenv
    #include <wchar.h>
#endif

#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace acs {

namespace {

bool StrEq(const char* a, const char* b) noexcept {
    if (!a || !b) return a == b;
    while (*a && *b) { if (*a != *b) return false; ++a; ++b; }
    return *a == 0 && *b == 0;
}

void RTrim(String& s) noexcept {
    while (s.Size() > 0) {
        const char c = s[s.Size() - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            // String には Pop が無いので新しい文字列を作って置換
            String t;
            t.Reserve(s.Size() - 1);
            t.Append(StringView(s.Data(), s.Size() - 1));
            s = Move(t);
        } else {
            break;
        }
    }
}

// 与えられたバッファ内に "<sub>\\<file>" の形でパスを書く
void Concat(wchar_t* out, usize cap, const wchar_t* base,
            const wchar_t* sub, const wchar_t* file) noexcept {
    if (cap == 0) return;
    out[0] = 0;
    auto append = [&](const wchar_t* s) {
        usize w = 0;
        while (out[w]) ++w;
        for (; *s && w + 1 < cap; ++s, ++w) out[w] = *s;
        out[w] = 0;
    };
    append(base);
    append(L"\\");
    append(sub);
    append(L"\\");
    append(file);
}

void DirOf(const wchar_t* path, wchar_t* out, usize cap) noexcept {
    usize len = 0;
    while (path[len] && len + 1 < cap) { out[len] = path[len]; ++len; }
    out[len] = 0;
    for (isize i = static_cast<isize>(len) - 1; i >= 0; --i) {
        if (out[i] == L'\\' || out[i] == L'/') { out[i] = 0; return; }
    }
    out[0] = 0;  // ディレクトリ部なし
}

} // namespace

void Storage::SetString(const char* key, const char* value) noexcept {
    if (!key) return;
    Entry* e = FindEntry(key);
    if (e) {
        e->value = String(value ? value : "");
    } else {
        Entry ne;
        ne.key   = String(key);
        ne.value = String(value ? value : "");
        _entries.PushBack(Move(ne));
    }
}

void Storage::SetInt(const char* key, i64 value) noexcept {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
    SetString(key, buf);
}

void Storage::SetFloat(const char* key, f64 value) noexcept {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", value);
    SetString(key, buf);
}

void Storage::SetBool(const char* key, bool value) noexcept {
    SetString(key, value ? "true" : "false");
}

const char* Storage::GetString(const char* key, const char* default_v) const noexcept {
    const Entry* e = FindEntry(key);
    if (!e) return default_v;
    return e->value.Data();
}

i64 Storage::GetInt(const char* key, i64 default_v) const noexcept {
    const Entry* e = FindEntry(key);
    if (!e) return default_v;
    return std::strtoll(e->value.Data(), nullptr, 10);
}

f64 Storage::GetFloat(const char* key, f64 default_v) const noexcept {
    const Entry* e = FindEntry(key);
    if (!e) return default_v;
    return std::strtod(e->value.Data(), nullptr);
}

bool Storage::GetBool(const char* key, bool default_v) const noexcept {
    const Entry* e = FindEntry(key);
    if (!e) return default_v;
    const char* v = e->value.Data();
    if (StrEq(v, "true") || StrEq(v, "1") || StrEq(v, "yes") || StrEq(v, "on")) return true;
    if (StrEq(v, "false") || StrEq(v, "0") || StrEq(v, "no") || StrEq(v, "off")) return false;
    return default_v;
}

bool Storage::Has(const char* key) const noexcept {
    return FindEntry(key) != nullptr;
}

void Storage::Remove(const char* key) noexcept {
    if (!key) return;
    for (usize i = 0; i < _entries.Size(); ++i) {
        if (StrEq(_entries[i].key.Data(), key)) {
            // 末尾と入替えて Pop（順序は保証しない）
            _entries[i] = Move(_entries[_entries.Size() - 1]);
            _entries.PopBack();
            return;
        }
    }
}

Storage::Entry* Storage::FindEntry(const char* key) noexcept {
    if (!key) return nullptr;
    for (usize i = 0; i < _entries.Size(); ++i) {
        if (StrEq(_entries[i].key.Data(), key)) return &_entries[i];
    }
    return nullptr;
}

const Storage::Entry* Storage::FindEntry(const char* key) const noexcept {
    return const_cast<Storage*>(this)->FindEntry(key);
}

Result<void> Storage::Load(const wchar_t* path) noexcept {
    if (!path) return ACS_ERR(IO, 100, "Storage::Load: null path");
    if (!FileSystem::Exists(path)) {
        // 無ければ空状態のまま成功扱い
        return Ok();
    }
    auto bytes_r = FileSystem::ReadAllBytes(path);
    if (bytes_r.IsErr()) return Err<void>(bytes_r.Error());
    const Array<byte>& bytes = bytes_r.Value();
    return LoadFromBytes(reinterpret_cast<const u8*>(bytes.Data()), bytes.Size());
}

Result<void> Storage::LoadFromBytes(const u8* data, usize size) noexcept {
    _entries.Clear();
    if (!data) return Ok();

    const char* p   = reinterpret_cast<const char*>(data);
    const char* end = p + size;
    // UTF-8 BOM をスキップ
    if (size >= 3 &&
        data[0] == 0xEF &&
        data[1] == 0xBB &&
        data[2] == 0xBF) {
        p += 3;
    }

    while (p < end) {
        const char* line_start = p;
        while (p < end && *p != '\n' && *p != '\r') ++p;
        StringView line{ line_start, static_cast<usize>(p - line_start) };
        // 改行文字をスキップ
        while (p < end && (*p == '\n' || *p == '\r')) ++p;

        // コメント・空行
        if (line.Size() == 0) continue;
        if (line.Data()[0] == '#' || line.Data()[0] == ';') continue;
        if (line.Data()[0] == '[') continue;

        // key=value で分割
        usize eq = 0;
        while (eq < line.Size() && line.Data()[eq] != '=') ++eq;
        if (eq >= line.Size()) continue;

        usize ks = 0; while (ks < eq && (line.Data()[ks] == ' ' || line.Data()[ks] == '\t')) ++ks;
        usize ke = eq; while (ke > ks && (line.Data()[ke - 1] == ' ' || line.Data()[ke - 1] == '\t')) --ke;
        usize vs = eq + 1; while (vs < line.Size() && (line.Data()[vs] == ' ' || line.Data()[vs] == '\t')) ++vs;
        usize ve = line.Size();

        Entry e;
        e.key   = String(StringView(line.Data() + ks, ke - ks));
        e.value = String(StringView(line.Data() + vs, ve - vs));
        _entries.PushBack(Move(e));
    }
    return Ok();
}

Result<void> Storage::Save(const wchar_t* path) noexcept {
    if (!path) return ACS_ERR(IO, 101, "Storage::Save: null path");

    // 親ディレクトリを作成（無ければ）
    wchar_t dir[1024];
    DirOf(path, dir, 1024);
    if (dir[0]) {
        auto mk = FileSystem::CreateDirectory(dir);
        if (mk.IsErr()) {
            // 既存なら成功扱いになっているので、エラーは本当の失敗
            return mk;
        }
    }

    String out;
    out.Reserve(64 * (_entries.Size() + 1));
    out.Append(StringView("# acs Storage\n"));
    for (usize i = 0; i < _entries.Size(); ++i) {
        out.Append(_entries[i].key.View());
        out.Append('=');
        out.Append(_entries[i].value.View());
        out.Append('\n');
    }
    return FileSystem::WriteAllBytes(path,
        reinterpret_cast<const byte*>(out.Data()), out.Size());
}

Result<void> Storage::GetAppDataPath(const wchar_t* sub_dir,
                                     const wchar_t* file_name,
                                     wchar_t* out, usize cap) noexcept {
    if (!out || cap == 0) return ACS_ERR(IO, 110, "GetAppDataPath: bad args");

#if ACS_PLATFORM_WINDOWS
    // Windows: SHGetKnownFolderPath で %APPDATA%（FOLDERID_RoamingAppData）
    PWSTR appdata = nullptr;
    HRESULT hr = ::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata);
    if (FAILED(hr) || !appdata) {
        if (appdata) ::CoTaskMemFree(appdata);
        return ACS_ERR_OS(OS, 111, "SHGetKnownFolderPath failed", static_cast<u32>(hr));
    }
    Concat(out, cap,
           appdata,
           sub_dir   ? sub_dir   : L"acs",
           file_name ? file_name : L"storage.ini");
    ::CoTaskMemFree(appdata);
#elif ACS_PLATFORM_POSIX
    // POSIX: $XDG_DATA_HOME 優先、fallback で $HOME/.local/share (Linux) /
    //        $HOME/Library/Application Support (macOS)
    const char* xdg = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    char base[1024]; base[0] = 0;
    if (xdg && xdg[0]) {
        std::snprintf(base, sizeof(base), "%s", xdg);
    } else if (home && home[0]) {
    #if ACS_PLATFORM_MACOS
        std::snprintf(base, sizeof(base), "%s/Library/Application Support", home);
    #else
        std::snprintf(base, sizeof(base), "%s/.local/share", home);
    #endif
    } else {
        return ACS_ERR(OS, 112, "neither XDG_DATA_HOME nor HOME set");
    }
    // narrow → wide 変換 (簡易、ASCII 範囲は 1:1)
    char fullpath[1024];
    char sub_narrow[256] = "acs";
    char file_narrow[256] = "storage.ini";
    if (sub_dir) {
        std::wcstombs(sub_narrow, sub_dir, sizeof(sub_narrow) - 1);
        sub_narrow[sizeof(sub_narrow) - 1] = 0;
    }
    if (file_name) {
        std::wcstombs(file_narrow, file_name, sizeof(file_narrow) - 1);
        file_narrow[sizeof(file_narrow) - 1] = 0;
    }
    std::snprintf(fullpath, sizeof(fullpath), "%s/%s/%s", base, sub_narrow, file_narrow);
    // narrow → wide で out へ書き戻し
    std::mbstowcs(out, fullpath, cap - 1);
    out[cap - 1] = 0;
#endif

    // 親ディレクトリを事前作成
    wchar_t parent[1024];
    DirOf(out, parent, 1024);
    if (parent[0]) {
        auto mk = FileSystem::CreateDirectory(parent);
        if (mk.IsErr()) return mk;
    }
    return Ok();
}

// ----------------------------------------------------------------------------
// UTF-8 受口版 — wchar_t に変換して既存実装に委譲
// (Linux/macOS では既存実装側もまだ Win32 依存。後続フェーズで完全 portable に)
// ----------------------------------------------------------------------------
namespace {
bool Utf8ToWide(const char* utf8, wchar_t* out, usize cap) noexcept {
    if (!utf8 || !out || cap == 0) return false;
#if ACS_PLATFORM_WINDOWS
    int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out,
                                  static_cast<int>(cap));
    return n > 0;
#else
    // POSIX: mbstowcs (現在ロケールに依存。UTF-8 系ロケールが標準前提)
    usize n = std::mbstowcs(out, utf8, cap - 1);
    if (n == static_cast<usize>(-1)) return false;
    out[n < cap ? n : cap - 1] = 0;
    return true;
#endif
}
bool WideToUtf8(const wchar_t* wide, char* out, usize cap) noexcept {
    if (!wide || !out || cap == 0) return false;
#if ACS_PLATFORM_WINDOWS
    int n = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out,
                                  static_cast<int>(cap), nullptr, nullptr);
    return n > 0;
#else
    usize n = std::wcstombs(out, wide, cap - 1);
    if (n == static_cast<usize>(-1)) return false;
    out[n < cap ? n : cap - 1] = 0;
    return true;
#endif
}
} // namespace

Result<void> Storage::Load(const char* path_utf8) noexcept {
    wchar_t buf[1024];
    if (!Utf8ToWide(path_utf8, buf, 1024)) {
        return ACS_ERR(IO, 120, "Storage::Load(utf8): conversion failed");
    }
    return Load(buf);
}

Result<void> Storage::Save(const char* path_utf8) noexcept {
    wchar_t buf[1024];
    if (!Utf8ToWide(path_utf8, buf, 1024)) {
        return ACS_ERR(IO, 121, "Storage::Save(utf8): conversion failed");
    }
    return Save(buf);
}

Result<void> Storage::GetAppDataPath(const char* sub_dir_utf8,
                                     const char* file_name_utf8,
                                     char* out_utf8, usize cap) noexcept {
    if (!out_utf8 || cap == 0) return ACS_ERR(IO, 122, "bad args");
    wchar_t sub[256], name[256], path[1024];
    if (sub_dir_utf8   && !Utf8ToWide(sub_dir_utf8,   sub,  256)) return ACS_ERR(IO, 123, "sub_dir utf8");
    if (file_name_utf8 && !Utf8ToWide(file_name_utf8, name, 256)) return ACS_ERR(IO, 124, "file_name utf8");
    auto r = GetAppDataPath(sub_dir_utf8 ? sub : nullptr,
                             file_name_utf8 ? name : nullptr,
                             path, 1024);
    if (r.IsErr()) return r;
    if (!WideToUtf8(path, out_utf8, cap)) return ACS_ERR(IO, 125, "result utf8");
    return Ok();
}

} // namespace acs
