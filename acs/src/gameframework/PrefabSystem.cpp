// SPDX-License-Identifier: Apache-2.0
// 名前付きprefab factoryをgeneration付きhandleで登録し、ANodeを生成する。
//
// name lookup は const char* 同士の per-byte 比較で行う (STL/<cstring> 禁止)。
// 登録件数は通常 1 セッションで数百件以下なので、線形走査でも実用上問題なし。
// 必要になったらハッシュテーブルへの差し替えを検討。
#include "gameframework/PrefabSystem.h"
#include "gameframework/ANode.h"   // spawn factory と TObjectPtr<ANode> の実体操作に必要

namespace acs::game {

namespace {

/** FPrefabId の low24 に格納できる最大スロット index。 */
constexpr u32 kMaxPrefabIndex = 0x00FFFFFFu;

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

/** 未使用 slot を探して index を返す (容量上限なら 0、index 0 は予約)。 */
u32 CPrefabSystem::AcquireSlot() noexcept {
    // index 0 は invalid 予約。index 1 以降から未使用 slot を探す。
    for (u32 i = 1; i < m_Entries.Num(); ++i) {
        if (!m_Entries[i].active) return i;
    }
    // 全部使用中 → 末尾を 1 つ拡張。
    // 配列がまだ空なら index 0 を dummy で埋めて以降 index 1 から始める。
    if (m_Entries.IsEmpty()) {
        m_Entries.Add(FPrefabEntry{});   // dummy at index 0 (常に inactive)
    }
    // 次に割り当てる index が low24 を超えると FPrefabId で切り詰められるため拒否する。
    if (m_Entries.Num() > kMaxPrefabIndex) return 0u;
    m_Entries.Add(FPrefabEntry{});
    return static_cast<u32>(m_Entries.Num()) - 1u;
}

/** 新規 Prefab を登録して FPrefabId を返す (バリデーション失敗時は invalid)。 */
FPrefabId CPrefabSystem::Register(const char* name, PrefabFactoryFn factory, void* user_data) noexcept {
    // バリデーション: 名前が空 / factory が無いと spawn が無意味なので拒否。
    if (IsEmptyName(name))      return FPrefabId{};
    if (factory == nullptr)     return FPrefabId{};

    const u32 idx = AcquireSlot();
    if (idx == 0u) return FPrefabId{};
    FPrefabEntry& e = m_Entries[idx];
    e.name      = name;
    e.factory   = factory;
    e.user_data = user_data;
    // generation: ラップアラウンドで 0 になると invalid と区別できなくなるので 1 にスナップ。
    e.gen       = static_cast<u8>(e.gen + 1u);
    if (e.gen == 0) e.gen = 1;
    e.active    = true;
    ++m_ActiveCount;
    return FPrefabId{idx, e.gen};
}

/** 名前で active な Prefab を線形探索する (一致しなければ invalid)。 */
FPrefabId CPrefabSystem::FindByName(const char* name) const noexcept {
    if (IsEmptyName(name)) return FPrefabId{};
    const u32 n = static_cast<u32>(m_Entries.Num());
    // index 0 は dummy なので 1 から走査。
    for (u32 i = 1; i < n; ++i) {
        const FPrefabEntry& e = m_Entries[i];
        if (!e.active) continue;
        if (StrEq(e.name, name)) return FPrefabId{i, e.gen};
    }
    return FPrefabId{};
}

/** ID から factory を 1 回呼んで ANode ツリーを生成する (stale handle は弾く)。 */
TObjectPtr<ANode> CPrefabSystem::Spawn(FPrefabId id) noexcept {
    if (!id.IsValid()) return TObjectPtr<ANode>{};
    const u32 idx = id.Index();
    if (idx >= m_Entries.Num()) return TObjectPtr<ANode>{};
    const FPrefabEntry& e = m_Entries[idx];
    // active 検証 + 世代一致 (stale handle を弾く)。
    if (!e.active || e.gen != id.Generation()) return TObjectPtr<ANode>{};
    if (e.factory == nullptr)                  return TObjectPtr<ANode>{};
    return e.factory(e.user_data);
}

/** 名前で検索してから spawn する (FindByName → Spawn)。 */
TObjectPtr<ANode> CPrefabSystem::SpawnByName(const char* name) noexcept {
    return Spawn(FindByName(name));
}

/** 登録を解除し、slot を再利用可能にして既存 handle を stale 化する。 */
bool CPrefabSystem::Unregister(FPrefabId id) noexcept {
    if (!id.IsValid()) return false;
    const u32 idx = id.Index();
    if (idx >= m_Entries.Num()) return false;
    FPrefabEntry& e = m_Entries[idx];
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
const char* CPrefabSystem::GetName(FPrefabId id) const noexcept {
    if (!id.IsValid()) return "(unknown)";
    const u32 idx = id.Index();
    if (idx >= m_Entries.Num()) return "(unknown)";
    const FPrefabEntry& e = m_Entries[idx];
    if (!e.active || e.gen != id.Generation()) return "(unknown)";
    if (e.name == nullptr) return "(unknown)";
    return e.name;
}

/** 全登録を破棄し、既存 ID を stale 化する。 */
void CPrefabSystem::ClearAll() noexcept {
    // 配列を捨てると、次の Register が同じ index / generation を再発行して古い ID が
    // 復活し得る。世代履歴を保持したまま inactive にし、次回再利用時に gen を進める。
    for (u32 i = 1u; i < m_Entries.Num(); ++i) {
        FPrefabEntry& entry = m_Entries[i];
        entry.active = false;
        entry.factory = nullptr;
        entry.user_data = nullptr;
        entry.name = nullptr;
    }
    m_ActiveCount = 0;
}

} // namespace acs::game
