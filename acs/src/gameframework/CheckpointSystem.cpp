// SPDX-License-Identifier: Apache-2.0
// GameFramework Genre Kit (Platformer) — FCheckpointSystem 実装
//
// slot+gen pattern で配置済み checkpoint を管理し、現在 active な 1 つを
// 保持する。one_way / requires_unlock のフラグを Activate 時に評価し、
// 不正な遷移は false 返却で弾く (callback も発火しない)。
//
// 設計補足:
//   ・id 比較は STL <cstring> を避けて per-byte ループを自前で書く
//     (FEntitlement / FProgression / FSettings と同じ StrEq pattern)。
//   ・unlocked リストは const char* の TArray (非所有)。文字列比較で線形検索
//     する想定で十分高速 (典型 N <= 数十)。
//   ・AllCheckpoints は穴を詰めるため m_Scratch を再構築して返す。
//     呼出側は次の Register/Unregister/AllCheckpoints/ClearAll で無効化される
//     ことを了解する契約 (ヘッダにも明記)。
#include "gameframework/CheckpointSystem.h"

namespace acs::game {

namespace {

/**
 * const char* の per-byte 比較 (nullptr 安全)。
 *
 * @details どちらかが nullptr なら false。STL <cstring> 不使用。
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

} // namespace

u32 FCheckpointSystem::AcquireSlot() noexcept {
    // index 0 は invalid 予約 (dummy)。1 以上から空き slot を線形検索。
    for (u32 i = 1; i < m_Slots.Size(); ++i) {
        if (!m_Slots[i].active) return i;
    }
    // 初回は dummy slot を 0 番に置く (FHealthSystem / FPickupSystem と同パターン)。
    if (m_Slots.IsEmpty()) {
        m_Slots.PushBack({});
    }
    m_Slots.PushBack({});
    return static_cast<u32>(m_Slots.Size()) - 1u;
}

isize FCheckpointSystem::FindIndexById(const char* id) const noexcept {
    if (id == nullptr) return -1;
    const usize n = m_Slots.Size();
    // index 0 は dummy なので 1 から走査。
    for (usize i = 1; i < n; ++i) {
        const Slot& s = m_Slots[i];
        if (!s.active) continue;
        if (StrEq(s.info.id, id)) return static_cast<isize>(i);
    }
    return -1;
}

isize FCheckpointSystem::FindUnlockedIndex(const char* id) const noexcept {
    if (id == nullptr) return -1;
    const usize n = m_Unlocked.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(m_Unlocked[i], id)) return static_cast<isize>(i);
    }
    return -1;
}

FCheckpointSystem::Slot* FCheckpointSystem::FindSlot(CheckpointId id) noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx == 0 || idx >= m_Slots.Size()) return nullptr;
    Slot& s = m_Slots[idx];
    if (!s.active) return nullptr;
    if (s.gen != id.Generation()) return nullptr;
    return &s;
}

const FCheckpointSystem::Slot* FCheckpointSystem::FindSlot(CheckpointId id) const noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx == 0 || idx >= m_Slots.Size()) return nullptr;
    const Slot& s = m_Slots[idx];
    if (!s.active) return nullptr;
    if (s.gen != id.Generation()) return nullptr;
    return &s;
}

CheckpointId FCheckpointSystem::Register(const CheckpointInfo& info) noexcept {
    // id == nullptr は意味を持たないので静かに弾く (アセット欠損時の保険)。
    if (info.id == nullptr) return CheckpointId{};
    // 同 id の 2 重登録は invalid 返却 (FProgression / EntitlementRegistry と同様)。
    if (FindIndexById(info.id) >= 0) return CheckpointId{};

    const u32 idx = AcquireSlot();
    Slot& s = m_Slots[idx];

    // generation を進める (0 は invalid 扱いなので回避)。
    s.gen = static_cast<u8>(s.gen + 1u);
    if (s.gen == 0) s.gen = 1;

    s.active = true;
    s.info   = info;

    ++m_CheckpointCount;
    return CheckpointId{idx, s.gen};
}

void FCheckpointSystem::Unregister(CheckpointId id) noexcept {
    Slot* s = FindSlot(id);
    if (s == nullptr) return;

    // 現 active が解除対象なら active を invalid 化 (last_* は据置で履歴扱い)。
    if (m_Current == id) {
        m_Current = CheckpointId{};
    }

    s->active = false;
    s->info   = CheckpointInfo{};   // info を初期化 (id ポインタも nullptr に戻す)
    // generation はそのまま。次の Register で +1 して古い handle を弾く。

    if (m_CheckpointCount > 0) --m_CheckpointCount;
}

bool FCheckpointSystem::ActivateInternal(u32 slot_index) noexcept {
    // 前提: 呼出側で slot_index が active な範囲内であることを確認済み。
    Slot& s = m_Slots[slot_index];

    // requires_unlock な場合は unlocked リストを確認。未 unlock なら弾く。
    if (s.info.requires_unlock) {
        if (FindUnlockedIndex(s.info.id) < 0) return false;
    }

    // 現 active の one_way フラグを評価。one_way の場合は sort_order が
    // 戻り方向 (= 対象 < 現 active) への遷移を弾く。
    // 同じ checkpoint への再 Activate は素通し (no-op 成功)。
    if (m_Current.IsValid()) {
        const Slot* cur = FindSlot(m_Current);
        if (cur != nullptr && cur->info.one_way) {
            if (s.info.sort_order < cur->info.sort_order) return false;
        }
    }

    const CheckpointId new_id{slot_index, s.gen};

    // 既に同 checkpoint が active な場合は no-op 成功 (callback 再発火しない)。
    if (m_Current == new_id) return true;

    m_Current          = new_id;
    m_LastSpawnPos   = s.info.spawn_pos;
    m_LastLevelIndex = s.info.level_index;

    if (m_OnActivate != nullptr) {
        m_OnActivate(m_OnActivateUser, s.info.id, s.info.spawn_pos);
    }
    return true;
}

bool FCheckpointSystem::ActivateCheckpoint(const char* checkpoint_id) noexcept {
    const isize idx = FindIndexById(checkpoint_id);
    if (idx < 0) return false;
    return ActivateInternal(static_cast<u32>(idx));
}

bool FCheckpointSystem::ActivateCheckpoint(CheckpointId id) noexcept {
    Slot* s = FindSlot(id);
    if (s == nullptr) return false;
    return ActivateInternal(id.Index());
}

void FCheckpointSystem::UnlockCheckpoint(const char* checkpoint_id) noexcept {
    if (checkpoint_id == nullptr) return;
    // 既に unlocked なら no-op (Save 復元での重複呼出も無害)。
    if (FindUnlockedIndex(checkpoint_id) >= 0) return;
    // 未登録 id でも受け入れる (Save 復元が Register より先に来てもよい設計)。
    m_Unlocked.PushBack(checkpoint_id);
}

bool FCheckpointSystem::IsUnlocked(const char* checkpoint_id) const noexcept {
    if (checkpoint_id == nullptr) return false;
    const isize idx = FindIndexById(checkpoint_id);
    if (idx < 0) return false;
    const Slot& s = m_Slots[static_cast<usize>(idx)];
    // requires_unlock=false な定義なら常に unlocked 扱い (= 常時 available)。
    if (!s.info.requires_unlock) return true;
    return FindUnlockedIndex(checkpoint_id) >= 0;
}

CheckpointId FCheckpointSystem::CurrentCheckpoint() const noexcept {
    return m_Current;
}

FVec2 FCheckpointSystem::CurrentSpawnPos() const noexcept {
    const Slot* s = FindSlot(m_Current);
    if (s == nullptr) return FVec2::Zero();
    return s->info.spawn_pos;
}

u32 FCheckpointSystem::LastSpawnLevelIndex() const noexcept {
    return m_LastLevelIndex;
}

bool FCheckpointSystem::TriggerRespawn(FVec2& out_pos, u32& out_level_index) const noexcept {
    const Slot* s = FindSlot(m_Current);
    if (s == nullptr) return false;

    out_pos         = s->info.spawn_pos;
    out_level_index = s->info.level_index;

    if (m_OnRespawn != nullptr) {
        m_OnRespawn(m_OnRespawnUser, s->info.id, s->info.spawn_pos);
    }
    return true;
}

u32 FCheckpointSystem::CheckpointCount() const noexcept {
    return m_CheckpointCount;
}

u32 FCheckpointSystem::UnlockedCount() const noexcept {
    return static_cast<u32>(m_Unlocked.Size());
}

const CheckpointInfo* FCheckpointSystem::FindCheckpoint(const char* checkpoint_id) const noexcept {
    const isize idx = FindIndexById(checkpoint_id);
    if (idx < 0) return nullptr;
    return &m_Slots[static_cast<usize>(idx)].info;
}

const CheckpointInfo* FCheckpointSystem::AllCheckpoints(u32& out_count) const noexcept {
    // m_Scratch を再構築して穴を詰める。
    m_Scratch.Clear();
    const usize n = m_Slots.Size();
    // index 0 は dummy なので 1 から走査。
    for (usize i = 1; i < n; ++i) {
        const Slot& s = m_Slots[i];
        if (!s.active) continue;
        m_Scratch.PushBack(s.info);
    }
    out_count = static_cast<u32>(m_Scratch.Size());
    return m_Scratch.Data();
}

void FCheckpointSystem::SetOnActivateCallback(ActivateCallback cb, void* user) noexcept {
    m_OnActivate      = cb;
    m_OnActivateUser = user;
}

void FCheckpointSystem::SetOnRespawnCallback(RespawnCallback cb, void* user) noexcept {
    m_OnRespawn      = cb;
    m_OnRespawnUser = user;
}

void FCheckpointSystem::ClearAll() noexcept {
    m_Slots.Clear();
    m_Unlocked.Clear();
    m_Scratch.Clear();
    m_Current          = CheckpointId{};
    m_LastLevelIndex = 0;
    m_LastSpawnPos   = FVec2::Zero();
    m_CheckpointCount = 0;
    // callback 設定は保持 (FProgression::ResetProgress / FHealthSystem::ClearAll と同方針)。
}

} // namespace acs::game
