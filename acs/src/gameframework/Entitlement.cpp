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

// const char* の安全比較。どちらかが nullptr なら false。
// 終端ヌルまで一致 (長さ不一致は終端ズレで検出される)。
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

void EntitlementRegistry::Add(EntitlementInfo info) noexcept {
    // id == nullptr は意味を持たないので静かに弾く (DLC 一覧取得が失敗した
    // 時のフォールバック流入で nullptr が来ても registry を壊さない)。
    if (info.id == nullptr) return;
    _infos.PushBack(info);
}

bool EntitlementRegistry::Has(const char* id) const noexcept {
    if (id == nullptr) return false;
    const usize n = _infos.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(_infos[i].id, id)) return true;
    }
    return false;
}

bool EntitlementRegistry::IsActive(const char* id) const noexcept {
    if (id == nullptr) return false;
    const usize n = _infos.Size();
    for (usize i = 0; i < n; ++i) {
        const EntitlementInfo& e = _infos[i];
        if (StrEq(e.id, id)) return e.active;
    }
    return false;
}

bool EntitlementRegistry::HasAny(EntitlementKind k) const noexcept {
    const usize n = _infos.Size();
    for (usize i = 0; i < n; ++i) {
        const EntitlementInfo& e = _infos[i];
        if (e.kind == k && e.active) return true;
    }
    return false;
}

void EntitlementRegistry::Clear() noexcept {
    _infos.Clear();
}

u32 EntitlementRegistry::Count() const noexcept {
    // Array<T>::Size() は usize (= size_t) を返す。Pillar O の entitlement 件数は
    // 現実的に u32 範囲を超えないので、上位ビットを切り捨てる cast で十分。
    return static_cast<u32>(_infos.Size());
}

const EntitlementInfo* EntitlementRegistry::AllInfos() const noexcept {
    return _infos.Data();
}

} // namespace acs::game
