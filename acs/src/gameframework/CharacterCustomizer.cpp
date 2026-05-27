// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar O — FCharacterCustomizer 実装
//
// 設計上のポイント:
//   ・id 比較は const char* per-byte 比較。STL <string> / <cstring> 不使用。
//     FEntitlement / FAchievementManager と同じ StrEq を anonymous namespace に再掲。
//   ・cosmetic 件数は通常 200〜1000 のオーダー → 線形走査で十分。
//   ・装着状態は slot indexed 固定長配列 (_equipped_in_slot[kCosmeticSlotCount])
//     で持つため、EquippedInSlot / UnequipSlot は O(1)。
//   ・callback は装着 / 解除のたびに呼ぶ。loop 防止のため ClearAll では呼ばない。
#include "gameframework/CharacterCustomizer.h"

namespace acs::game {

namespace {

// const char* の per-byte 安全比較。FEntitlement.cpp / FAchievementManager.cpp と同設計。
// どちらかが nullptr なら false (StrEq("a", nullptr) は false)。
bool StrEq(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

// 「id 未発見」を表す哨兵値 (FAchievementManager と同じ慣習)。
constexpr u32 kNotFound = ~static_cast<u32>(0);

// ECosmeticSlot を _equipped_in_slot[] のインデックスに変換 (範囲外は kNotFound)。
// 不正値が enum class 経由で渡された場合のガード (将来 slot 追加し忘れ防止)。
u32 SlotIndex(ECosmeticSlot slot) noexcept {
    const u32 idx = static_cast<u32>(slot);
    return (idx < kCosmeticSlotCount) ? idx : kNotFound;
}

} // namespace

// =============================================================================
// 構築
// =============================================================================

FCharacterCustomizer::FCharacterCustomizer() noexcept {
    // 固定長 const char*[] を nullptr で初期化 (= 全 slot 未装着)。
    // TArray<bool> / TArray<CosmeticItem> はデフォルト構築で空。
    for (u32 i = 0; i < kCosmeticSlotCount; ++i) {
        _equipped_in_slot[i] = nullptr;
    }
}

// =============================================================================
// 内部ユーティリティ
// =============================================================================

u32 FCharacterCustomizer::FindIndex(const char* id) const noexcept {
    if (id == nullptr) return kNotFound;
    const usize n = _items.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(_items[i].id, id)) return static_cast<u32>(i);
    }
    return kNotFound;
}

// =============================================================================
// 定義登録
// =============================================================================

void FCharacterCustomizer::RegisterCosmetic(const CosmeticItem& item) noexcept {
    // defensive: id == nullptr は意味を持たないので静かに弾く。
    if (item.id == nullptr) return;

    // 同 id の 2 重登録は no-op (アセット二重ロード保護、FAchievementManager と揃える)。
    if (FindIndex(item.id) != kNotFound) return;

    _items.PushBack(item);
    _unlocked.PushBack(false);  // 登録時は未 unlock がデフォルト
}

// =============================================================================
// Unlock 管理
// =============================================================================

bool FCharacterCustomizer::UnlockCosmetic(const char* id) noexcept {
    const u32 idx = FindIndex(id);
    if (idx == kNotFound) return false;     // 未登録は失敗
    if (_unlocked[idx])    return false;    // 既 unlock は新規でないので false (冪等)

    _unlocked[idx] = true;
    return true;
}

bool FCharacterCustomizer::IsUnlocked(const char* id) const noexcept {
    const u32 idx = FindIndex(id);
    if (idx == kNotFound) return false;
    return _unlocked[idx];
}

// =============================================================================
// 装着 / 解除
// =============================================================================

bool FCharacterCustomizer::EquipCosmetic(const char* id) noexcept {
    const u32 idx = FindIndex(id);
    if (idx == kNotFound)  return false;     // 未登録は失敗
    if (!_unlocked[idx])   return false;     // 未 unlock は装着不可 (倫理: ストア未購入を装着しない)

    const CosmeticItem& item = _items[idx];
    const u32 slot_idx = SlotIndex(item.slot);
    if (slot_idx == kNotFound) return false;  // 不正 slot 値 (将来 enum 拡張時の保険)

    // 同 slot に同 id が既装着なら no-op (= 新規装着ではないので false)。
    if (_equipped_in_slot[slot_idx] != nullptr &&
        StrEq(_equipped_in_slot[slot_idx], item.id)) {
        return false;
    }

    // 同 slot の既存装着があれば外す (callback で「解除」を通知)。
    // ※ _equipped_in_slot[] を更新する **前** に通知すると、callback 中の
    //   EquippedInSlot() 照会で「直前の値」が見えてしまうため、先に nullptr 化してから
    //   通知する。FSeasonPass / FProgression の callback と同じ「現状確定 → 通知」順序。
    if (_equipped_in_slot[slot_idx] != nullptr) {
        _equipped_in_slot[slot_idx] = nullptr;
        if (_on_equip != nullptr) {
            _on_equip(_on_equip_user, item.slot, nullptr);
        }
    }

    // 新規装着を反映 + 通知。
    _equipped_in_slot[slot_idx] = item.id;
    if (_on_equip != nullptr) {
        _on_equip(_on_equip_user, item.slot, item.id);
    }
    return true;
}

void FCharacterCustomizer::UnequipSlot(ECosmeticSlot slot) noexcept {
    const u32 slot_idx = SlotIndex(slot);
    if (slot_idx == kNotFound) return;
    if (_equipped_in_slot[slot_idx] == nullptr) return;  // 既に空 (callback も呼ばない)

    _equipped_in_slot[slot_idx] = nullptr;
    if (_on_equip != nullptr) {
        _on_equip(_on_equip_user, slot, nullptr);
    }
}

const char* FCharacterCustomizer::EquippedInSlot(ECosmeticSlot slot) const noexcept {
    const u32 slot_idx = SlotIndex(slot);
    if (slot_idx == kNotFound) return nullptr;
    return _equipped_in_slot[slot_idx];
}

// =============================================================================
// 照会
// =============================================================================

u32 FCharacterCustomizer::CosmeticCount() const noexcept {
    return static_cast<u32>(_items.Size());
}

u32 FCharacterCustomizer::UnlockedCount() const noexcept {
    u32 c = 0;
    const usize n = _unlocked.Size();
    for (usize i = 0; i < n; ++i) {
        if (_unlocked[i]) ++c;
    }
    return c;
}

u32 FCharacterCustomizer::CountInSlot(ECosmeticSlot slot) const noexcept {
    u32 c = 0;
    const usize n = _items.Size();
    for (usize i = 0; i < n; ++i) {
        if (_items[i].slot == slot) ++c;
    }
    return c;
}

const CosmeticItem* FCharacterCustomizer::FindCosmetic(const char* id) const noexcept {
    const u32 idx = FindIndex(id);
    if (idx == kNotFound) return nullptr;
    return &_items[idx];
}

const CosmeticItem* FCharacterCustomizer::AllCosmetics(u32& out_count) const noexcept {
    out_count = static_cast<u32>(_items.Size());
    return _items.Data();
}

// =============================================================================
// FCallback
// =============================================================================

void FCharacterCustomizer::SetOnEquipCallback(EquipCallback cb, void* user) noexcept {
    _on_equip      = cb;
    _on_equip_user = user;
}

// =============================================================================
// リセット
// =============================================================================

void FCharacterCustomizer::ClearAll() noexcept {
    _items.Clear();
    _unlocked.Clear();
    // loop / ノイズ防止のため callback は呼ばない (装着解除を通知しない)。
    for (u32 i = 0; i < kCosmeticSlotCount; ++i) {
        _equipped_in_slot[i] = nullptr;
    }
}

} // namespace acs::game
