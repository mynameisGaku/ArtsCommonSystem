// SPDX-License-Identifier: Apache-2.0
// Storage 実装（INI 形式 key=value、UTF-8）
#include "platform/Storage.h"
#include "platform/FileSystem.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

#include <Windows.h>
#include <ShlObj.h>

// Windows.h で `CreateDirectory` が `CreateDirectoryW` に展開されると
// FileSystem::CreateDirectory もマクロ置換されて衝突する。マクロを取り消す。
#ifdef CreateDirectory
    #undef CreateDirectory
#endif

#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace acs {

namespace {

/**
 * 2 つの NUL 終端文字列が等しいかを返す。
 *
 * @param a 比較する文字列 1 (nullptr 可)。
 * @param b 比較する文字列 2 (nullptr 可)。
 * @return 内容が一致すれば true (両方 nullptr も true)。
 */
bool StrEq(const char* a, const char* b) noexcept {
    if (!a || !b) return a == b;
    while (*a && *b) { if (*a != *b) return false; ++a; ++b; }
    return *a == 0 && *b == 0;
}

/**
 * 文字列末尾の空白・改行類 (' ' '\t' '\r' '\n') を取り除く。
 *
 * @param s 対象の文字列 (その場で短縮される)。
 */
void RTrim(FString& s) noexcept {
    while (s.Size() > 0) {
        const char c = s[s.Size() - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            // FString には Pop が無いので新しい文字列を作って置換
            FString t;
            t.Reserve(s.Size() - 1);
            t.Append(FStringView(s.Data(), s.Size() - 1));
            s = Move(t);
        } else {
            break;
        }
    }
}

/**
 * "<base>\<sub>\<file>" の形でパスを out へ書き込む。
 *
 * @details cap に収まる範囲で連結し、常に NUL 終端する。
 * @param out 書き込み先バッファ。
 * @param cap out の容量 (要素数)。
 * @param base 先頭のベースパス。
 * @param sub サブディレクトリ名。
 * @param file ファイル名。
 */
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

/**
 * パスから末尾要素を除いたディレクトリ部分を out へ書き込む。
 *
 * @details 最後の '\\' または '/' までを切り出す。区切りが無ければ out は空文字列。
 * @param path 元のパス。
 * @param out ディレクトリ部分を書き込む先のバッファ。
 * @param cap out の容量 (要素数)。
 */
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
        e->value = FString(value ? value : "");
    } else {
        Entry ne;
        ne.key   = FString(key);
        ne.value = FString(value ? value : "");
        m_Entries.PushBack(Move(ne));
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
    for (usize i = 0; i < m_Entries.Size(); ++i) {
        if (StrEq(m_Entries[i].key.Data(), key)) {
            // 末尾と入替えて Pop（順序は保証しない）
            m_Entries[i] = Move(m_Entries[m_Entries.Size() - 1]);
            m_Entries.PopBack();
            return;
        }
    }
}

Storage::Entry* Storage::FindEntry(const char* key) noexcept {
    if (!key) return nullptr;
    for (usize i = 0; i < m_Entries.Size(); ++i) {
        if (StrEq(m_Entries[i].key.Data(), key)) return &m_Entries[i];
    }
    return nullptr;
}

const Storage::Entry* Storage::FindEntry(const char* key) const noexcept {
    return const_cast<Storage*>(this)->FindEntry(key);
}

TResult<void> Storage::Load(const wchar_t* path) noexcept {
    if (!path) return ACS_ERR(IO, 100, "Storage::Load: null path");
    if (!FileSystem::Exists(path)) {
        // 無ければ空状態のまま成功扱い
        return Ok();
    }
    auto bytes_r = FileSystem::ReadAllBytes(path);
    if (bytes_r.IsErr()) return Err<void>(bytes_r.Error());
    const TArray<byte>& bytes = bytes_r.Value();
    return LoadFromBytes(reinterpret_cast<const u8*>(bytes.Data()), bytes.Size());
}

TResult<void> Storage::LoadFromBytes(const u8* data, usize size) noexcept {
    m_Entries.Clear();
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
        FStringView line{ line_start, static_cast<usize>(p - line_start) };
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
        const usize ve = line.Size();

        Entry e;
        e.key   = FString(FStringView(line.Data() + ks, ke - ks));
        e.value = FString(FStringView(line.Data() + vs, ve - vs));
        m_Entries.PushBack(Move(e));
    }
    return Ok();
}

TResult<void> Storage::Save(const wchar_t* path) noexcept {
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

    FString out;
    out.Reserve(64 * (m_Entries.Size() + 1));
    out.Append(FStringView("# acs Storage\n"));
    for (usize i = 0; i < m_Entries.Size(); ++i) {
        out.Append(m_Entries[i].key.View());
        out.Append('=');
        out.Append(m_Entries[i].value.View());
        out.Append('\n');
    }
    return FileSystem::WriteAllBytes(path,
        reinterpret_cast<const byte*>(out.Data()), out.Size());
}

TResult<void> Storage::GetAppDataPath(const wchar_t* sub_dir,
                                     const wchar_t* file_name,
                                     wchar_t* out, usize cap) noexcept {
    if (!out || cap == 0) return ACS_ERR(IO, 110, "GetAppDataPath: bad args");

    // %APPDATA%（FOLDERID_RoamingAppData）を取得
    PWSTR appdata = nullptr;
    const HRESULT hr = ::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata);
    if (FAILED(hr) || !appdata) {
        if (appdata) ::CoTaskMemFree(appdata);
        return ACS_ERR_OS(OS, 111, "SHGetKnownFolderPath failed", static_cast<u32>(hr));
    }

    Concat(out, cap,
           appdata,
           sub_dir   ? sub_dir   : L"acs",
           file_name ? file_name : L"storage.ini");
    ::CoTaskMemFree(appdata);

    // 親ディレクトリを事前作成（Save で再度作るが、Load 前に呼ぶケースもあるので）
    wchar_t parent[1024];
    DirOf(out, parent, 1024);
    if (parent[0]) {
        auto mk = FileSystem::CreateDirectory(parent);
        if (mk.IsErr()) return mk;
    }
    return Ok();
}

namespace {

/**
 * UTF-8 文字列を wchar_t 文字列へ変換する。
 *
 * @param utf8 変換元の NUL 終端 UTF-8 文字列。
 * @param out 変換結果を書き込む先のバッファ。
 * @param cap out の容量 (要素数)。
 * @return 変換に成功すれば true。
 */
bool Utf8ToWide(const char* utf8, wchar_t* out, usize cap) noexcept {
    if (!utf8 || !out || cap == 0) return false;
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out,
                                  static_cast<int>(cap));
    return n > 0;
}

/**
 * wchar_t 文字列を UTF-8 文字列へ変換する。
 *
 * @param wide 変換元の NUL 終端 wchar_t 文字列。
 * @param out 変換結果を書き込む先のバッファ。
 * @param cap out の容量 (バイト数)。
 * @return 変換に成功すれば true。
 */
bool WideToUtf8(const wchar_t* wide, char* out, usize cap) noexcept {
    if (!wide || !out || cap == 0) return false;
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out,
                                  static_cast<int>(cap), nullptr, nullptr);
    return n > 0;
}

} // namespace

TResult<void> Storage::Load(const char* path_utf8) noexcept {
    wchar_t buf[1024];
    if (!Utf8ToWide(path_utf8, buf, 1024)) {
        return ACS_ERR(IO, 120, "Storage::Load(utf8): conversion failed");
    }
    return Load(buf);
}

TResult<void> Storage::Save(const char* path_utf8) noexcept {
    wchar_t buf[1024];
    if (!Utf8ToWide(path_utf8, buf, 1024)) {
        return ACS_ERR(IO, 121, "Storage::Save(utf8): conversion failed");
    }
    return Save(buf);
}

TResult<void> Storage::GetAppDataPath(const char* sub_dir_utf8,
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
