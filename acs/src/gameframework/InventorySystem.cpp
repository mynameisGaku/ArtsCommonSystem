// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar O/G — CInventorySystem 実装
//
// 設計上のポイント (ヘッダの設計コメントと対応):
//   ・item_id は const char* per-byte 線形検索 (FEntitlement / CEconomyDirector と同設計)。
//     アイテム種別 (500〜2000 のオーダー) は線形で十分。
//   ・AddItem は積み増し優先 (= 既存 stack で max_stack 未満のものから埋める)。
//     これにより inventory の fragmentation が抑えられ UI のスロットが散らばらない。
//   ・RemoveItem は前方優先 (低 index から減らす)。UI の見え方が自然になる。
//   ・MoveSlot は「同 item ⇒ merge / 別 item ⇒ swap」のドラッグ&ドロップ慣習に合わせる。
//   ・can_drop チェックは DropSlot のみ。MoveSlot / RemoveItem では検査しない
//     (UI の slot 並べ替え / クエスト消費等で意図的に動かせるため)。
//   ・ChangeCallback は変更があった slot ごとに呼ぶ。Add / Remove で複数 slot に
//     波及した場合はそれぞれ呼ぶ。loop 防止のため Init / ClearAll では呼ばない。
//   ・WARN は CEconomyDirector / FEntitlement と同じ Log.h 経由。
#include "gameframework/InventorySystem.h"
#include "foundation/Log.h"

namespace acs::game {

namespace {

/**
 * const char* の per-byte 安全比較を行う。
 *
 * @details どちらかが nullptr なら false。
 * @param a 比較する文字列 1。
 * @param b 比較する文字列 2。
 * @return 内容が一致すれば true。
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

} // namespace

u32 CInventorySystem::FindItemSlot(const char* item_id) const noexcept {
    if (item_id == nullptr) return kNotFound;
    const usize n = m_Items.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(m_Items[i].id, item_id)) return static_cast<u32>(i);
    }
    return kNotFound;
}

void CInventorySystem::NotifyChange(u32 slot_index) noexcept {
    if (m_OnChange == nullptr) return;
    // 範囲外は呼ばない (defensive)。slot_index < SlotCount() のときだけ。
    if (slot_index >= static_cast<u32>(m_Slots.Size())) return;
    const FInventorySlot& s = m_Slots[slot_index];
    m_OnChange(m_OnChangeUser, slot_index, s.item_id, s.count);
}

void CInventorySystem::Init(u32 slot_count) noexcept {
    // 0 は 1 にクランプ (defensive — Init(0) しても 1 slot だけ確保しておく)。
    if (slot_count == 0) slot_count = 1;

    // 同じ slot_count なら no-op (再 Init で内容をクリアしないことを保証)。
    if (static_cast<u32>(m_Slots.Size()) == slot_count) return;

    // slot 数が違う場合は内容クリアして resize。
    m_Slots.Clear();
    m_Slots.Resize(slot_count);
    // Resize はデフォルト構築するので item_id = nullptr / count = 0 で初期化済。
}

void CInventorySystem::RegisterItem(const FItemDef& def) noexcept {
    // defensive: id == nullptr は意味を持たないので静かに弾く。
    if (def.id == nullptr) return;

    // 同 id の 2 重登録は no-op (アセット二重ロード保護)。
    if (FindItemSlot(def.id) != kNotFound) {
        ACS_LOG_WARN("FInventorySystem: duplicate item registration ignored ('%s')", def.id);
        return;
    }

    // max_stack == 0 は意味を持たないので 1 にクランプ。
    FItemDef copy = def;
    if (copy.max_stack == 0) copy.max_stack = 1;

    m_Items.PushBack(copy);
}

const FItemDef* CInventorySystem::FindItem(const char* item_id) const noexcept {
    const u32 slot = FindItemSlot(item_id);
    if (slot == kNotFound) return nullptr;
    return &m_Items[slot];
}

u32 CInventorySystem::AddItem(const char* item_id, u32 count) noexcept {
    // 早期 reject — side effect なし。
    if (item_id == nullptr || count == 0) return 0;
    if (m_Slots.Size() == 0)               return 0;  // Init 前

    const u32 def_idx = FindItemSlot(item_id);
    if (def_idx == kNotFound) return 0;  // 未登録 item

    // ItemDef::id をリテラル参照として保存する (slot.item_id は m_Items[].id を指す)。
    const FItemDef& def     = m_Items[def_idx];
    const char*    canon_id = def.id;
    const u32      max_stk  = def.max_stack;
    const u32      n_slots  = static_cast<u32>(m_Slots.Size());

    u32 remaining = count;

    // Pass 1: 既存 stack に積み増し (積み増し優先で fragmentation を抑える)。
    for (u32 i = 0; i < n_slots && remaining > 0; ++i) {
        FInventorySlot& s = m_Slots[i];
        if (s.item_id == nullptr) continue;          // 空 slot は Pass 2 で
        if (!StrEq(s.item_id, canon_id)) continue;   // 別 item
        if (s.count >= max_stk)         continue;    // 既に満杯
        const u32 room = max_stk - s.count;
        const u32 add  = (remaining < room) ? remaining : room;
        s.count    += add;
        remaining  -= add;
        NotifyChange(i);
    }

    // Pass 2: 空 slot を埋める。
    for (u32 i = 0; i < n_slots && remaining > 0; ++i) {
        FInventorySlot& s = m_Slots[i];
        if (s.item_id != nullptr) continue;  // 埋まっている
        const u32 add = (remaining < max_stk) ? remaining : max_stk;
        s.item_id  = canon_id;  // リテラル参照
        s.count    = add;
        remaining -= add;
        NotifyChange(i);
    }

    return count - remaining;
}

u32 CInventorySystem::RemoveItem(const char* item_id, u32 count) noexcept {
    if (item_id == nullptr || count == 0) return 0;
    if (m_Slots.Size() == 0)               return 0;

    const u32 n_slots = static_cast<u32>(m_Slots.Size());
    u32       removed = 0;

    // 前方優先で減らす (UI の見え方を自然にする)。
    for (u32 i = 0; i < n_slots && removed < count; ++i) {
        FInventorySlot& s = m_Slots[i];
        if (s.item_id == nullptr) continue;
        if (!StrEq(s.item_id, item_id)) continue;
        const u32 need = count - removed;
        if (s.count <= need) {
            // この slot を空にする。
            removed   += s.count;
            s.item_id  = nullptr;
            s.count    = 0;
        } else {
            s.count -= need;
            removed += need;
        }
        NotifyChange(i);
    }

    return removed;
}

bool CInventorySystem::HasItem(const char* item_id, u32 min_count) const noexcept {
    // min_count == 0 は「0 個以上 = 常に true」(defensive)。
    if (min_count == 0) return true;
    if (item_id == nullptr) return false;
    return ItemTotal(item_id) >= min_count;
}

u32 CInventorySystem::ItemTotal(const char* item_id) const noexcept {
    if (item_id == nullptr) return 0;
    if (m_Slots.Size() == 0) return 0;

    const u32 n_slots = static_cast<u32>(m_Slots.Size());
    u32       total   = 0;
    for (u32 i = 0; i < n_slots; ++i) {
        const FInventorySlot& s = m_Slots[i];
        if (s.item_id == nullptr) continue;
        if (!StrEq(s.item_id, item_id)) continue;
        // u32 オーバーフローは現実的な inventory サイズでは起きないため
        // クランプは省略 (max_stack * 30 slot ≪ 2^32)。
        total += s.count;
    }
    return total;
}

bool CInventorySystem::MoveSlot(u32 from_index, u32 to_index) noexcept {
    const u32 n_slots = static_cast<u32>(m_Slots.Size());
    if (n_slots == 0) return false;
    if (from_index >= n_slots || to_index >= n_slots) return false;

    // 同 index は no-op (UI のドラッグ操作で「同じ場所に落とした」想定)。
    if (from_index == to_index) return true;

    FInventorySlot& from = m_Slots[from_index];
    FInventorySlot& to   = m_Slots[to_index];

    // from が空なら何もしない (操作としては成功扱い)。
    if (from.item_id == nullptr) return true;

    if (to.item_id == nullptr) {
        // 移動 — to を埋め、from を空にする。
        to.item_id   = from.item_id;
        to.count     = from.count;
        from.item_id = nullptr;
        from.count   = 0;
        NotifyChange(from_index);
        NotifyChange(to_index);
        return true;
    }

    if (StrEq(from.item_id, to.item_id)) {
        // 同 item ⇒ merge (max_stack まで)。残りは from に残す。
        // max_stack は ItemDef から引く必要がある。未登録 (= 移行中の異常 slot) なら 1 扱い。
        const FItemDef* def     = FindItem(to.item_id);
        const u32      max_stk = (def != nullptr) ? def->max_stack : 1;

        if (to.count >= max_stk) {
            // 既に満杯 → swap (= 別 item と同じ扱い)。
            const FInventorySlot tmp = to;
            to   = from;
            from = tmp;
        } else {
            const u32 room = max_stk - to.count;
            const u32 move = (from.count < room) ? from.count : room;
            to.count   += move;
            from.count -= move;
            if (from.count == 0) {
                from.item_id = nullptr;
            }
        }
        NotifyChange(from_index);
        NotifyChange(to_index);
        return true;
    }

    // 別 item ⇒ swap。
    const FInventorySlot tmp = to;
    to   = from;
    from = tmp;
    NotifyChange(from_index);
    NotifyChange(to_index);
    return true;
}

bool CInventorySystem::DropSlot(u32 index) noexcept {
    if (index >= static_cast<u32>(m_Slots.Size())) return false;

    FInventorySlot& s = m_Slots[index];
    if (s.item_id == nullptr) return false;  // 既に空

    // can_drop チェック (quest / key 系を構造的に守る)。
    const FItemDef* def = FindItem(s.item_id);
    if (def != nullptr && !def->can_drop) return false;

    s.item_id = nullptr;
    s.count   = 0;
    NotifyChange(index);
    return true;
}

const FInventorySlot* CInventorySystem::GetSlot(u32 index) const noexcept {
    if (index >= static_cast<u32>(m_Slots.Size())) return nullptr;
    return &m_Slots[index];
}

u32 CInventorySystem::SlotCount() const noexcept {
    return static_cast<u32>(m_Slots.Size());
}

u32 CInventorySystem::EmptySlotCount() const noexcept {
    const u32 n = static_cast<u32>(m_Slots.Size());
    u32       e = 0;
    for (u32 i = 0; i < n; ++i) {
        if (m_Slots[i].item_id == nullptr) ++e;
    }
    return e;
}

void CInventorySystem::SetOnChangeCallback(ChangeCallback cb, void* user) noexcept {
    // nullptr で detach は明示的に許可。
    m_OnChange      = cb;
    m_OnChangeUser = user;
}

void CInventorySystem::ClearAll() noexcept {
    m_Items.Clear();
    m_Slots.Clear();
    m_OnChange      = nullptr;
    m_OnChangeUser = nullptr;
}

} // namespace acs::game
