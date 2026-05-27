// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar J Phase 2 — FPrefabSystem 実装
//
// name lookup は const char* 同士の per-byte 比較で行う (STL/<cstring> 禁止)。
// 登録件数は通常 1 セッションで数百件以下なので、線形走査でも実用上問題なし。
// 必要になったらハッシュテーブルへの差し替えを検討。
#include "gameframework/PrefabSystem.h"
#include "gameframework/Node2D.h"   // UniquePtr<Node2D> の destructor が full type を要求

namespace acs::game {

namespace {

// const char* の安全比較。どちらかが nullptr なら false。
// 終端ヌルまで一致した時のみ true (長さ不一致は終端ズレで検出される)。
// FEntitlement.cpp の StrEq と同じパターン。
bool StrEq(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

// 空文字列も無効扱いにしたいので別ヘルパー。
bool IsEmptyName(const char* s) noexcept {
    return s == nullptr || s[0] == '\0';
}

} // namespace

u32 FPrefabSystem::AcquireSlot() noexcept {
    // index 0 は invalid 予約。index 1 以降から未使用 slot を探す。
    for (u32 i = 1; i < m_Entries.Size(); ++i) {
        if (!m_Entries[i].active) return i;
    }
    // 全部使用中 → 末尾を 1 つ拡張。
    // 配列がまだ空なら index 0 を dummy で埋めて以降 index 1 から始める。
    if (m_Entries.IsEmpty()) {
        m_Entries.PushBack(PrefabEntry{});   // dummy at index 0 (常に inactive)
    }
    m_Entries.PushBack(PrefabEntry{});
    return static_cast<u32>(m_Entries.Size()) - 1u;
}

PrefabId FPrefabSystem::Register(const char* name, PrefabFactoryFn factory, void* user_data) noexcept {
    // バリデーション: 名前が空 / factory が無いと spawn が無意味なので拒否。
    if (IsEmptyName(name))      return PrefabId{};
    if (factory == nullptr)     return PrefabId{};

    const u32 idx = AcquireSlot();
    PrefabEntry& e = m_Entries[idx];
    e.name      = name;
    e.factory   = factory;
    e.user_data = user_data;
    // generation: ラップアラウンドで 0 になると invalid と区別できなくなるので 1 にスナップ。
    e.gen       = static_cast<u8>(e.gen + 1u);
    if (e.gen == 0) e.gen = 1;
    e.active    = true;
    ++m_ActiveCount;
    return PrefabId{idx, e.gen};
}

PrefabId FPrefabSystem::FindByName(const char* name) const noexcept {
    if (IsEmptyName(name)) return PrefabId{};
    const u32 n = static_cast<u32>(m_Entries.Size());
    // index 0 は dummy なので 1 から走査。
    for (u32 i = 1; i < n; ++i) {
        const PrefabEntry& e = m_Entries[i];
        if (!e.active) continue;
        if (StrEq(e.name, name)) return PrefabId{i, e.gen};
    }
    return PrefabId{};
}

TUniquePtr<FNode2D> FPrefabSystem::Spawn(PrefabId id) noexcept {
    if (!id.IsValid()) return TUniquePtr<FNode2D>{};
    const u32 idx = id.Index();
    if (idx >= m_Entries.Size()) return TUniquePtr<FNode2D>{};
    const PrefabEntry& e = m_Entries[idx];
    // active 検証 + 世代一致 (stale handle を弾く)。
    if (!e.active || e.gen != id.Generation()) return TUniquePtr<FNode2D>{};
    if (e.factory == nullptr)                  return TUniquePtr<FNode2D>{};
    return e.factory(e.user_data);
}

TUniquePtr<FNode2D> FPrefabSystem::SpawnByName(const char* name) noexcept {
    return Spawn(FindByName(name));
}

bool FPrefabSystem::Unregister(PrefabId id) noexcept {
    if (!id.IsValid()) return false;
    const u32 idx = id.Index();
    if (idx >= m_Entries.Size()) return false;
    PrefabEntry& e = m_Entries[idx];
    if (!e.active || e.gen != id.Generation()) return false;

    e.active    = false;
    e.factory   = nullptr;
    e.user_data = nullptr;
    e.name      = nullptr;
    // gen はそのまま残す: 次に AcquireSlot で再利用された時に +1 されて再採番される。
    if (m_ActiveCount > 0) --m_ActiveCount;
    return true;
}

const char* FPrefabSystem::GetName(PrefabId id) const noexcept {
    if (!id.IsValid()) return "(unknown)";
    const u32 idx = id.Index();
    if (idx >= m_Entries.Size()) return "(unknown)";
    const PrefabEntry& e = m_Entries[idx];
    if (!e.active || e.gen != id.Generation()) return "(unknown)";
    if (e.name == nullptr) return "(unknown)";
    return e.name;
}

void FPrefabSystem::ClearAll() noexcept {
    // gen を進めず TArray ごと捨てる: 古い ID 経由のアクセスは Index 範囲外
    // または slot 再利用後の gen 不一致のどちらかで弾かれる。
    m_Entries.Clear();
    m_ActiveCount = 0;
}

} // namespace acs::game
