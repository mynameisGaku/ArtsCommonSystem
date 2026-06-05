// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar O — EntitlementRegistry 実装
//
// id 比較は const char* 同士の per-byte 比較 (StrEqual 相当を自前で書く)。
// STL 禁止 + <cstring> も避ける方針で、ループを直接書いておく。
// entitlement 件数はゲーム 1 セッションで通常 10〜100 のオーダーなので、
// 線形走査で十分。ハッシュテーブル化は将来必要になったら検討。
#include "gameframework/Entitlement.h"

namespace acs::game {

namespace {

/**
 * const char* 同士を per-byte で比較する (StrEqual 相当の自前実装)。
 *
 * @details どちらかが nullptr なら false。終端ヌルまで一致を確認する
 * (長さ不一致は終端ズレで検出される)。STL / <cstring> を使わない方針で手書きする。
 * @param a 比較する一方の文字列。
 * @param b 比較する他方の文字列。
 * @return 両者が終端まで一致すれば true。
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

/** entitlement を登録する (id == nullptr は静かに弾く)。 */
void EntitlementRegistry::Add(EntitlementInfo info) noexcept {
    // id == nullptr は意味を持たないので静かに弾く (DLC 一覧取得が失敗した
    // 時のフォールバック流入で nullptr が来ても registry を壊さない)。
    if (info.id == nullptr) return;
    m_Infos.PushBack(info);
}

/** 指定 id の entitlement が登録済みか (active は問わない) を返す。 */
bool EntitlementRegistry::Has(const char* id) const noexcept {
    if (id == nullptr) return false;
    const usize n = m_Infos.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(m_Infos[i].id, id)) return true;
    }
    return false;
}

/** 指定 id の entitlement が登録済みかつ active かを返す。 */
bool EntitlementRegistry::IsActive(const char* id) const noexcept {
    if (id == nullptr) return false;
    const usize n = m_Infos.Size();
    for (usize i = 0; i < n; ++i) {
        const EntitlementInfo& e = m_Infos[i];
        if (StrEq(e.id, id)) return e.active;
    }
    return false;
}

/** 指定 kind の active な entitlement を 1 件でも持っているかを返す。 */
bool EntitlementRegistry::HasAny(EntitlementKind k) const noexcept {
    const usize n = m_Infos.Size();
    for (usize i = 0; i < n; ++i) {
        const EntitlementInfo& e = m_Infos[i];
        if (e.kind == k && e.active) return true;
    }
    return false;
}

/** 登録済み entitlement をすべて破棄する。 */
void EntitlementRegistry::Clear() noexcept {
    m_Infos.Clear();
}

/** 登録済み entitlement の件数を返す。 */
u32 EntitlementRegistry::Count() const noexcept {
    // TArray<T>::Size() は usize (= size_t) を返す。entitlement 件数は
    // 現実的に u32 範囲を超えないので、上位ビットを切り捨てる cast で十分。
    return static_cast<u32>(m_Infos.Size());
}

/** 登録済み entitlement の生バッファ先頭を返す (Count() 件の連続領域)。 */
const EntitlementInfo* EntitlementRegistry::AllInfos() const noexcept {
    return m_Infos.Data();
}

} // namespace acs::game
