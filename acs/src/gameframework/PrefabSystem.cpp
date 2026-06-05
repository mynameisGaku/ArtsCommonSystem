// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar J — FPrefabSystem 実装
//
// name lookup は const char* 同士の per-byte 比較で行う (STL/<cstring> 禁止)。
// 登録件数は通常 1 セッションで数百件以下なので、線形走査でも実用上問題なし。
// 必要になったらハッシュテーブルへの差し替えを検討。
#include "gameframework/PrefabSystem.h"
#include "gameframework/Node2D.h"   // UniquePtr<Node2D> の destructor が full type を要求

namespace acs::game {

namespace {

/**
 * const char* 同士を nullptr 安全に比較する。
 *
 * @details
 * どちらかが nullptr なら false。終端ヌルまで一致した時のみ true (長さ不一致は終端ズレで
 * 検出される)。STL <cstring> 不使用のため per-byte で実装する。
 * @param a 比較対象の文字列 A。
 * @param b 比較対象の文字列 B。
 * @return 内容が完全一致すれば true。
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

/**
 * 名前が空 (nullptr または長さ 0) かを返す。
 *
 * @param s 判定対象の文字列。
 * @return nullptr または空文字列なら true。
 */
bool IsEmptyName(const char* s) noexcept {
    return s == nullptr || s[0] == '\0';
}

} // namespace

/** 未使用 slot を探して index を返す (空きが無ければ末尾を拡張、index 0 は予約)。 */
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

/** 新規 Prefab を登録して PrefabId を返す (バリデーション失敗時は invalid)。 */
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

/** 名前で active な Prefab を線形探索する (一致しなければ invalid)。 */
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

/** ID から factory を 1 回呼んで FNode2D ツリーを生成する (stale handle は弾く)。 */
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

/** 名前で検索してから spawn する (FindByName → Spawn)。 */
TUniquePtr<FNode2D> FPrefabSystem::SpawnByName(const char* name) noexcept {
    return Spawn(FindByName(name));
}

/** 登録を解除し、slot を再利用可能にして既存 handle を stale 化する。 */
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

/** デバッグ用に Prefab 名を返す (invalid / stale なら "(unknown)")。 */
const char* FPrefabSystem::GetName(PrefabId id) const noexcept {
    if (!id.IsValid()) return "(unknown)";
    const u32 idx = id.Index();
    if (idx >= m_Entries.Size()) return "(unknown)";
    const PrefabEntry& e = m_Entries[idx];
    if (!e.active || e.gen != id.Generation()) return "(unknown)";
    if (e.name == nullptr) return "(unknown)";
    return e.name;
}

/** 全登録を破棄する (既存 ID は範囲外または gen 不一致で弾かれる)。 */
void FPrefabSystem::ClearAll() noexcept {
    // gen を進めず TArray ごと捨てる: 古い ID 経由のアクセスは Index 範囲外
    // または slot 再利用後の gen 不一致のどちらかで弾かれる。
    m_Entries.Clear();
    m_ActiveCount = 0;
}

} // namespace acs::game
