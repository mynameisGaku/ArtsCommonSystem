// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar O — FCharacterCustomizer 実装
//
// 設計上のポイント:
//   ・id 比較は const char* per-byte 比較。STL <string> / <cstring> 不使用。
//     FEntitlement / FAchievementManager と同じ StrEq を anonymous namespace に再掲。
//   ・cosmetic 件数は通常 200〜1000 のオーダー → 線形走査で十分。
//   ・装着状態は slot indexed 固定長配列 (m_EquippedInSlot[kCosmeticSlotCount])
//     で持つため、EquippedInSlot / UnequipSlot は O(1)。
//   ・callback は装着 / 解除のたびに呼ぶ。loop 防止のため ClearAll では呼ばない。
#include "gameframework/CharacterCustomizer.h"

namespace acs::game {

namespace {

/**
 * const char* の per-byte 安全比較。
 *
 * @details どちらかが nullptr なら false (StrEq("a", nullptr) は false)。STL <cstring> 不使用。
 * @param a 比較する文字列 A。
 * @param b 比較する文字列 B。
 * @return 両者が同一内容の文字列なら true。
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

/** 「id 未発見」を表す哨兵値。 */
constexpr u32 kNotFound = ~static_cast<u32>(0);

/**
 * ECosmeticSlot を m_EquippedInSlot[] のインデックスに変換する。
 *
 * @details 不正値が enum 経由で渡された場合のガード (将来 slot 追加し忘れ防止)。
 * @param slot 変換する slot 値。
 * @return 0..kCosmeticSlotCount-1 の index (範囲外は kNotFound)。
 */
u32 SlotIndex(ECosmeticSlot slot) noexcept {
    const u32 idx = static_cast<u32>(slot);
    return (idx < kCosmeticSlotCount) ? idx : kNotFound;
}

} // namespace

FCharacterCustomizer::FCharacterCustomizer() noexcept {
    // 固定長 const char*[] を nullptr で初期化 (= 全 slot 未装着)。
    // TArray<bool> / TArray<CosmeticItem> はデフォルト構築で空。
    for (u32 i = 0; i < kCosmeticSlotCount; ++i) {
        m_EquippedInSlot[i] = nullptr;
    }
}

u32 FCharacterCustomizer::FindIndex(const char* id) const noexcept {
    if (id == nullptr) return kNotFound;
    const usize n = m_Items.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(m_Items[i].id, id)) return static_cast<u32>(i);
    }
    return kNotFound;
}

void FCharacterCustomizer::RegisterCosmetic(const FCosmeticItem& item) noexcept {
    // defensive: id == nullptr は意味を持たないので静かに弾く。
    if (item.id == nullptr) return;

    // 同 id の 2 重登録は no-op (アセット二重ロード保護、FAchievementManager と揃える)。
    if (FindIndex(item.id) != kNotFound) return;

    m_Items.PushBack(item);
    m_Unlocked.PushBack(false);  // 登録時は未 unlock がデフォルト
}

bool FCharacterCustomizer::UnlockCosmetic(const char* id) noexcept {
    const u32 idx = FindIndex(id);
    if (idx == kNotFound) return false;     // 未登録は失敗
    if (m_Unlocked[idx])    return false;    // 既 unlock は新規でないので false (冪等)

    m_Unlocked[idx] = true;
    return true;
}

bool FCharacterCustomizer::IsUnlocked(const char* id) const noexcept {
    const u32 idx = FindIndex(id);
    if (idx == kNotFound) return false;
    return m_Unlocked[idx];
}

bool FCharacterCustomizer::EquipCosmetic(const char* id) noexcept {
    const u32 idx = FindIndex(id);
    if (idx == kNotFound)  return false;     // 未登録は失敗
    if (!m_Unlocked[idx])   return false;     // 未 unlock は装着不可 (倫理: ストア未購入を装着しない)

    const FCosmeticItem& item = m_Items[idx];
    const u32 slot_idx = SlotIndex(item.slot);
    if (slot_idx == kNotFound) return false;  // 不正 slot 値 (将来 enum 拡張時の保険)

    // 同 slot に同 id が既装着なら no-op (= 新規装着ではないので false)。
    if (m_EquippedInSlot[slot_idx] != nullptr &&
        StrEq(m_EquippedInSlot[slot_idx], item.id)) {
        return false;
    }

    // 同 slot の既存装着があれば外す (callback で「解除」を通知)。
    // ※ m_EquippedInSlot[] を更新する **前** に通知すると、callback 中の
    //   EquippedInSlot() 照会で「直前の値」が見えてしまうため、先に nullptr 化してから
    //   通知する。FSeasonPass / FProgression の callback と同じ「現状確定 → 通知」順序。
    if (m_EquippedInSlot[slot_idx] != nullptr) {
        m_EquippedInSlot[slot_idx] = nullptr;
        if (m_OnEquip != nullptr) {
            m_OnEquip(m_OnEquipUser, item.slot, nullptr);
        }
    }

    // 新規装着を反映 + 通知。
    m_EquippedInSlot[slot_idx] = item.id;
    if (m_OnEquip != nullptr) {
        m_OnEquip(m_OnEquipUser, item.slot, item.id);
    }
    return true;
}

void FCharacterCustomizer::UnequipSlot(ECosmeticSlot slot) noexcept {
    const u32 slot_idx = SlotIndex(slot);
    if (slot_idx == kNotFound) return;
    if (m_EquippedInSlot[slot_idx] == nullptr) return;  // 既に空 (callback も呼ばない)

    m_EquippedInSlot[slot_idx] = nullptr;
    if (m_OnEquip != nullptr) {
        m_OnEquip(m_OnEquipUser, slot, nullptr);
    }
}

const char* FCharacterCustomizer::EquippedInSlot(ECosmeticSlot slot) const noexcept {
    const u32 slot_idx = SlotIndex(slot);
    if (slot_idx == kNotFound) return nullptr;
    return m_EquippedInSlot[slot_idx];
}

u32 FCharacterCustomizer::CosmeticCount() const noexcept {
    return static_cast<u32>(m_Items.Size());
}

u32 FCharacterCustomizer::UnlockedCount() const noexcept {
    u32 c = 0;
    const usize n = m_Unlocked.Size();
    for (usize i = 0; i < n; ++i) {
        if (m_Unlocked[i]) ++c;
    }
    return c;
}

u32 FCharacterCustomizer::CountInSlot(ECosmeticSlot slot) const noexcept {
    u32 c = 0;
    const usize n = m_Items.Size();
    for (usize i = 0; i < n; ++i) {
        if (m_Items[i].slot == slot) ++c;
    }
    return c;
}

const FCosmeticItem* FCharacterCustomizer::FindCosmetic(const char* id) const noexcept {
    const u32 idx = FindIndex(id);
    if (idx == kNotFound) return nullptr;
    return &m_Items[idx];
}

const FCosmeticItem* FCharacterCustomizer::AllCosmetics(u32& out_count) const noexcept {
    out_count = static_cast<u32>(m_Items.Size());
    return m_Items.Data();
}

void FCharacterCustomizer::SetOnEquipCallback(EquipCallback cb, void* user) noexcept {
    m_OnEquip      = cb;
    m_OnEquipUser = user;
}

void FCharacterCustomizer::ClearAll() noexcept {
    m_Items.Clear();
    m_Unlocked.Clear();
    // loop / ノイズ防止のため callback は呼ばない (装着解除を通知しない)。
    for (u32 i = 0; i < kCosmeticSlotCount; ++i) {
        m_EquippedInSlot[i] = nullptr;
    }
}

} // namespace acs::game
