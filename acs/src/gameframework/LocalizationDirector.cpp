// SPDX-License-Identifier: Apache-2.0
// GameFramework — FLocalizationDirector 実装
//
// key 比較は const char* 同士の per-byte 比較 (StrEq 相当を自前で書く)。
// STL 禁止 + <cstring> も避ける方針で、ループを直接書いておく。
// 文字列 ID 件数は通常 100〜2000 のオーダーなので、線形走査で十分。
#include "gameframework/LocalizationDirector.h"

namespace acs::game {

namespace {

/**
 * const char* 同士を終端ヌルまで安全に比較する (`<cstring>` 不使用)。
 *
 * @details 長さ不一致は終端ズレで検出される。
 * @param a 比較する文字列 (nullptr 可)。
 * @param b 比較する文字列 (nullptr 可)。
 * @return 全文字一致なら true (どちらかが nullptr なら false)。
 */
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

void FLocalizationDirector::SetLocale(ELocale loc) noexcept {
    m_Current = loc;
}

ELocale FLocalizationDirector::CurrentLocale() const noexcept {
    return m_Current;
}

void FLocalizationDirector::RegisterString(ELocale loc, const char* key, const char* value) noexcept {
    // key == nullptr は意味を持たないので静かに弾く (PO/CSV からの取り込みで
    // null が混じっても registry を壊さない)。value == nullptr は許容
    // (「存在するが空翻訳」を表現する余地、Get では nullptr のままで返さず
    // 後続の処理で扱う前提)。
    if (key == nullptr) return;
    LocaleEntry e;
    e.locale = loc;
    e.key    = key;
    e.value  = value;
    m_Entries.PushBack(e);
}

const char* FLocalizationDirector::Get(const char* key) const noexcept {
    // key == nullptr は呼び出し側の UI コードで null チェックを省略するため
    // 空文字を返す (nullptr を返すと strlen 等で AV になる)。
    if (key == nullptr) return "";

    // 1) 現 locale で検索
    {
        const isize idx = FindIndex(m_Current, key);
        if (idx >= 0) {
            const char* v = m_Entries[static_cast<usize>(idx)].value;
            // value == nullptr 登録は空文字として返す (Get の戻りに nullptr を
            // 出さない契約)。
            return (v != nullptr) ? v : "";
        }
    }

    // 2) Default(=En) にフォールバック (現 locale が既に Default の場合は同じ
    //    探索になるが、コストは線形 1 回分なので素直に走らせる)
    if (m_Current != ELocale::Default) {
        const isize idx = FindIndex(ELocale::Default, key);
        if (idx >= 0) {
            const char* v = m_Entries[static_cast<usize>(idx)].value;
            return (v != nullptr) ? v : "";
        }
    }

    // 3) どこにも無ければ key 自身を返す (未翻訳箇所を画面で発見しやすくする
    //    意図的な挙動。空文字や nullptr を返すと UI が空欄になり、開発中の
    //    抜けに気付きにくい)。
    return key;
}

const char* FLocalizationDirector::GetForLocale(ELocale loc, const char* key) const noexcept {
    if (key == nullptr) return "";
    const isize idx = FindIndex(loc, key);
    if (idx >= 0) {
        const char* v = m_Entries[static_cast<usize>(idx)].value;
        return (v != nullptr) ? v : "";
    }
    // 明示指定された locale 単独で引いているので、Default フォールバックは
    // 行わず key 自身を返す (呼び出し側が「この locale には無い」を検知できる)。
    return key;
}

bool FLocalizationDirector::Has(const char* key) const noexcept {
    if (key == nullptr) return false;
    return FindIndex(m_Current, key) >= 0;
}

u32 FLocalizationDirector::KeyCount(ELocale loc) const noexcept {
    u32 n = 0;
    const usize total = m_Entries.Size();
    for (usize i = 0; i < total; ++i) {
        if (m_Entries[i].locale == loc) ++n;
    }
    return n;
}

void FLocalizationDirector::Clear() noexcept {
    m_Entries.Clear();
}

void FLocalizationDirector::ClearLocale(ELocale loc) noexcept {
    // 指定 locale 以外を残して再構築。TArray に Erase が無いので、生存要素を
    // 前詰めしてから末尾を切る (RemoveAtSwap だと順序が壊れて Get の先頭一致
    // 挙動が不安定になるため使わない)。
    const usize total = m_Entries.Size();
    usize write = 0;
    for (usize read = 0; read < total; ++read) {
        if (m_Entries[read].locale != loc) {
            if (write != read) {
                m_Entries[write] = m_Entries[read];
            }
            ++write;
        }
    }
    // 末尾を縮める。Resize は LocaleEntry が trivially destructible なので
    // 単純にサイズだけ縮める動きになる (TArray<T>::Resize 実装参照)。
    if (write != total) {
        m_Entries.Resize(write);
    }
}

isize FLocalizationDirector::FindIndex(ELocale loc, const char* key) const noexcept {
    if (key == nullptr) return -1;
    const usize n = m_Entries.Size();
    for (usize i = 0; i < n; ++i) {
        const LocaleEntry& e = m_Entries[i];
        if (e.locale == loc && StrEq(e.key, key)) {
            return static_cast<isize>(i);
        }
    }
    return -1;
}

} // namespace acs::game
