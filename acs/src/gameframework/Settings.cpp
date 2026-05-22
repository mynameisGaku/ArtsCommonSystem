// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar G — Settings 実装
//
// 線形検索 + 同 key 上書き。STL 禁止方針に従い、<cstring> も避けて
// per-byte 比較ループを自前で書く (Entitlement.cpp と同じ StrEq pattern)。
//
// Save / Load は Phase 1 では TODO スタブ。Phase 2 で `acs::Storage` か
// FileSystem 経由で INI 風 (key=value\n + 型 prefix) を実装する。Phase 1 で
// 形だけ Result<void> を返しておくと、呼び出し側 (ゲームの起動/終了処理)
// の構造を先に組めるので、空実装でも Ok() を返す方針にしている。
#include "gameframework/Settings.h"

namespace acs::game {

namespace {

// const char* の per-byte 比較。nullptr 安全。
bool StrEq(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

} // namespace

isize Settings::FindIndex(const char* key) const noexcept {
    if (key == nullptr) return -1;
    const usize n = _entries.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(_entries[i].key, key)) return static_cast<isize>(i);
    }
    return -1;
}

Settings::Entry& Settings::UpsertEntry(const char* key) noexcept {
    // key == nullptr は呼び出し側でガードされている前提だが、二重防御で
    // 末尾を返す代わりに sentinel を立てる必要はない (Set* 側で弾く)。
    const isize idx = FindIndex(key);
    if (idx >= 0) {
        return _entries[static_cast<usize>(idx)];
    }
    Entry e;
    e.key  = key;
    e.kind = SettingKind::None;
    _entries.PushBack(e);
    return _entries[_entries.Size() - 1];
}

// ============================================================================
// 書き込み
// ============================================================================
void Settings::SetF32(const char* key, f32 v) noexcept {
    if (key == nullptr) return;
    Entry& e   = UpsertEntry(key);
    e.kind     = SettingKind::F32;
    e.value.f  = v;
}

void Settings::SetI32(const char* key, i32 v) noexcept {
    if (key == nullptr) return;
    Entry& e   = UpsertEntry(key);
    e.kind     = SettingKind::I32;
    e.value.i  = v;
}

void Settings::SetBool(const char* key, bool v) noexcept {
    if (key == nullptr) return;
    Entry& e   = UpsertEntry(key);
    e.kind     = SettingKind::Bool;
    e.value.b  = v;
}

void Settings::SetString(const char* key, const char* v) noexcept {
    if (key == nullptr) return;
    Entry& e   = UpsertEntry(key);
    e.kind     = SettingKind::String;
    e.value.s  = v;  // 非所有: 呼び出し側が寿命を保証する
}

// ============================================================================
// 読み出し (型不一致 / 未検出は default_value)
// ============================================================================
f32 Settings::GetF32(const char* key, f32 default_value) const noexcept {
    const isize idx = FindIndex(key);
    if (idx < 0) return default_value;
    const Entry& e = _entries[static_cast<usize>(idx)];
    if (e.kind != SettingKind::F32) return default_value;
    return e.value.f;
}

i32 Settings::GetI32(const char* key, i32 default_value) const noexcept {
    const isize idx = FindIndex(key);
    if (idx < 0) return default_value;
    const Entry& e = _entries[static_cast<usize>(idx)];
    if (e.kind != SettingKind::I32) return default_value;
    return e.value.i;
}

bool Settings::GetBool(const char* key, bool default_value) const noexcept {
    const isize idx = FindIndex(key);
    if (idx < 0) return default_value;
    const Entry& e = _entries[static_cast<usize>(idx)];
    if (e.kind != SettingKind::Bool) return default_value;
    return e.value.b;
}

const char* Settings::GetString(const char* key, const char* default_value) const noexcept {
    const isize idx = FindIndex(key);
    if (idx < 0) return default_value;
    const Entry& e = _entries[static_cast<usize>(idx)];
    if (e.kind != SettingKind::String) return default_value;
    return e.value.s;
}

// ============================================================================
// メタ操作
// ============================================================================
bool Settings::Has(const char* key) const noexcept {
    return FindIndex(key) >= 0;
}

void Settings::Remove(const char* key) noexcept {
    const isize idx = FindIndex(key);
    if (idx < 0) return;
    // 順序は保持しなくてよい (key 検索は全件走査するため、index は不変条件にない)。
    _entries.RemoveAtSwap(static_cast<usize>(idx));
}

void Settings::Clear() noexcept {
    _entries.Clear();
}

u32 Settings::Count() const noexcept {
    // 件数は通常 u32 範囲を超えない (UI 設定で数百個が現実的上限)。
    return static_cast<u32>(_entries.Size());
}

// ============================================================================
// 永続化 (Phase 2 で実装)
// ============================================================================
// Phase 1 はスタブ。形だけ Result<void> を返して呼び出し側の構造を
// 先に組めるようにする。Phase 2 で `acs::Storage` か FileSystem 経由の
// atomic write + 読み取り (UTF-8 / LF, `f:key=value` のような型 prefix) を実装。
Result<void> Settings::Save(const wchar_t* file_path) noexcept {
    (void)file_path;
    // TODO(Phase 2): INI 風 key=value テキストを atomic write で書き出す。
    //   ・型 prefix: `f:audio.master=0.8`, `i:display.width=1920`,
    //                `b:display.vsync=true`, `s:locale=ja`
    //   ・改行は LF、エンコードは UTF-8
    //   ・書き込みは tmp + rename で原子的に
    return Ok();
}

Result<void> Settings::Load(const wchar_t* file_path) noexcept {
    (void)file_path;
    // TODO(Phase 2): Save と対称な reader を実装。型 prefix を見て
    //   Set{F32,I32,Bool,String} にディスパッチ。未知の prefix は警告して skip。
    //   ファイル不在は IO カテゴリのエラーで返す (初回起動を区別できる)。
    return Ok();
}

} // namespace acs::game
