// SPDX-License-Identifier: Apache-2.0
// GameFramework Genre Kit (Platformer) — CheckpointSystem 実装
//
// slot+gen pattern で配置済み checkpoint を管理し、現在 active な 1 つを
// 保持する。one_way / requires_unlock のフラグを Activate 時に評価し、
// 不正な遷移は false 返却で弾く (callback も発火しない)。
//
// 設計補足:
//   ・id 比較は STL <cstring> を避けて per-byte ループを自前で書く
//     (Entitlement / Progression / Settings と同じ StrEq pattern)。
//   ・unlocked リストは const char* の Array (非所有)。文字列比較で線形検索
//     する想定で十分高速 (典型 N <= 数十)。
//   ・AllCheckpoints は穴を詰めるため _scratch を再構築して返す。
//     呼出側は次の Register/Unregister/AllCheckpoints/ClearAll で無効化される
//     ことを了解する契約 (ヘッダにも明記)。
#include "gameframework/CheckpointSystem.h"

namespace acs::game {

namespace {

// const char* の per-byte 比較。nullptr 安全。
// Entitlement.cpp / Progression.cpp / Settings.cpp と同じ実装 (STL <cstring> 禁止)。
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

// =============================================================================
// 内部 slot 取得
// =============================================================================

u32 CheckpointSystem::AcquireSlot() noexcept {
    // index 0 は invalid 予約 (dummy)。1 以上から空き slot を線形検索。
    for (u32 i = 1; i < _slots.Size(); ++i) {
        if (!_slots[i].active) return i;
    }
    // 初回は dummy slot を 0 番に置く (HealthSystem / PickupSystem と同パターン)。
    if (_slots.IsEmpty()) {
        _slots.PushBack({});
    }
    _slots.PushBack({});
    return static_cast<u32>(_slots.Size()) - 1u;
}

// =============================================================================
// 内部検索ヘルパ
// =============================================================================

isize CheckpointSystem::FindIndexById(const char* id) const noexcept {
    if (id == nullptr) return -1;
    const usize n = _slots.Size();
    // index 0 は dummy なので 1 から走査。
    for (usize i = 1; i < n; ++i) {
        const Slot& s = _slots[i];
        if (!s.active) continue;
        if (StrEq(s.info.id, id)) return static_cast<isize>(i);
    }
    return -1;
}

isize CheckpointSystem::FindUnlockedIndex(const char* id) const noexcept {
    if (id == nullptr) return -1;
    const usize n = _unlocked.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(_unlocked[i], id)) return static_cast<isize>(i);
    }
    return -1;
}

CheckpointSystem::Slot* CheckpointSystem::FindSlot(CheckpointId id) noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx == 0 || idx >= _slots.Size()) return nullptr;
    Slot& s = _slots[idx];
    if (!s.active) return nullptr;
    if (s.gen != id.Generation()) return nullptr;
    return &s;
}

const CheckpointSystem::Slot* CheckpointSystem::FindSlot(CheckpointId id) const noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx == 0 || idx >= _slots.Size()) return nullptr;
    const Slot& s = _slots[idx];
    if (!s.active) return nullptr;
    if (s.gen != id.Generation()) return nullptr;
    return &s;
}

// =============================================================================
// Register / Unregister
// =============================================================================

CheckpointId CheckpointSystem::Register(const CheckpointInfo& info) noexcept {
    // id == nullptr は意味を持たないので静かに弾く (アセット欠損時の保険)。
    if (info.id == nullptr) return CheckpointId{};
    // 同 id の 2 重登録は invalid 返却 (Progression / EntitlementRegistry と同様)。
    if (FindIndexById(info.id) >= 0) return CheckpointId{};

    const u32 idx = AcquireSlot();
    Slot& s = _slots[idx];

    // generation を進める (0 は invalid 扱いなので回避)。
    s.gen = static_cast<u8>(s.gen + 1u);
    if (s.gen == 0) s.gen = 1;

    s.active = true;
    s.info   = info;

    ++_checkpoint_count;
    return CheckpointId{idx, s.gen};
}

void CheckpointSystem::Unregister(CheckpointId id) noexcept {
    Slot* s = FindSlot(id);
    if (s == nullptr) return;

    // 現 active が解除対象なら active を invalid 化 (last_* は据置で履歴扱い)。
    if (_current == id) {
        _current = CheckpointId{};
    }

    s->active = false;
    s->info   = CheckpointInfo{};   // info を初期化 (id ポインタも nullptr に戻す)
    // generation はそのまま。次の Register で +1 して古い handle を弾く。

    if (_checkpoint_count > 0) --_checkpoint_count;
}

// =============================================================================
// Activate (現在地点として記録)
// =============================================================================

bool CheckpointSystem::ActivateInternal(u32 slot_index) noexcept {
    // 前提: 呼出側で slot_index が active な範囲内であることを確認済み。
    Slot& s = _slots[slot_index];

    // requires_unlock な場合は unlocked リストを確認。未 unlock なら弾く。
    if (s.info.requires_unlock) {
        if (FindUnlockedIndex(s.info.id) < 0) return false;
    }

    // 現 active の one_way フラグを評価。one_way の場合は sort_order が
    // 戻り方向 (= 対象 < 現 active) への遷移を弾く。
    // 同じ checkpoint への再 Activate は素通し (no-op 成功)。
    if (_current.IsValid()) {
        const Slot* cur = FindSlot(_current);
        if (cur != nullptr && cur->info.one_way) {
            if (s.info.sort_order < cur->info.sort_order) return false;
        }
    }

    const CheckpointId new_id{slot_index, s.gen};

    // 既に同 checkpoint が active な場合は no-op 成功 (callback 再発火しない)。
    if (_current == new_id) return true;

    _current          = new_id;
    _last_spawn_pos   = s.info.spawn_pos;
    _last_level_index = s.info.level_index;

    if (_on_activate != nullptr) {
        _on_activate(_on_activate_user, s.info.id, s.info.spawn_pos);
    }
    return true;
}

bool CheckpointSystem::ActivateCheckpoint(const char* checkpoint_id) noexcept {
    const isize idx = FindIndexById(checkpoint_id);
    if (idx < 0) return false;
    return ActivateInternal(static_cast<u32>(idx));
}

bool CheckpointSystem::ActivateCheckpoint(CheckpointId id) noexcept {
    Slot* s = FindSlot(id);
    if (s == nullptr) return false;
    return ActivateInternal(id.Index());
}

// =============================================================================
// Unlock (隠し / DLC ones)
// =============================================================================

void CheckpointSystem::UnlockCheckpoint(const char* checkpoint_id) noexcept {
    if (checkpoint_id == nullptr) return;
    // 既に unlocked なら no-op (Save 復元での重複呼出も無害)。
    if (FindUnlockedIndex(checkpoint_id) >= 0) return;
    // 未登録 id でも受け入れる (Save 復元が Register より先に来てもよい設計)。
    _unlocked.PushBack(checkpoint_id);
}

bool CheckpointSystem::IsUnlocked(const char* checkpoint_id) const noexcept {
    if (checkpoint_id == nullptr) return false;
    const isize idx = FindIndexById(checkpoint_id);
    if (idx < 0) return false;
    const Slot& s = _slots[static_cast<usize>(idx)];
    // requires_unlock=false な定義なら常に unlocked 扱い (= 常時 available)。
    if (!s.info.requires_unlock) return true;
    return FindUnlockedIndex(checkpoint_id) >= 0;
}

// =============================================================================
// 現在状態の照会
// =============================================================================

CheckpointId CheckpointSystem::CurrentCheckpoint() const noexcept {
    return _current;
}

Vec2 CheckpointSystem::CurrentSpawnPos() const noexcept {
    const Slot* s = FindSlot(_current);
    if (s == nullptr) return Vec2::Zero();
    return s->info.spawn_pos;
}

u32 CheckpointSystem::LastSpawnLevelIndex() const noexcept {
    return _last_level_index;
}

bool CheckpointSystem::TriggerRespawn(Vec2& out_pos, u32& out_level_index) const noexcept {
    const Slot* s = FindSlot(_current);
    if (s == nullptr) return false;

    out_pos         = s->info.spawn_pos;
    out_level_index = s->info.level_index;

    if (_on_respawn != nullptr) {
        _on_respawn(_on_respawn_user, s->info.id, s->info.spawn_pos);
    }
    return true;
}

// =============================================================================
// 件数照会
// =============================================================================

u32 CheckpointSystem::CheckpointCount() const noexcept {
    return _checkpoint_count;
}

u32 CheckpointSystem::UnlockedCount() const noexcept {
    return static_cast<u32>(_unlocked.Size());
}

const CheckpointInfo* CheckpointSystem::FindCheckpoint(const char* checkpoint_id) const noexcept {
    const isize idx = FindIndexById(checkpoint_id);
    if (idx < 0) return nullptr;
    return &_slots[static_cast<usize>(idx)].info;
}

const CheckpointInfo* CheckpointSystem::AllCheckpoints(u32& out_count) const noexcept {
    // _scratch を再構築して穴を詰める。
    _scratch.Clear();
    const usize n = _slots.Size();
    // index 0 は dummy なので 1 から走査。
    for (usize i = 1; i < n; ++i) {
        const Slot& s = _slots[i];
        if (!s.active) continue;
        _scratch.PushBack(s.info);
    }
    out_count = static_cast<u32>(_scratch.Size());
    return _scratch.Data();
}

// =============================================================================
// Callback
// =============================================================================

void CheckpointSystem::SetOnActivateCallback(ActivateCallback cb, void* user) noexcept {
    _on_activate      = cb;
    _on_activate_user = user;
}

void CheckpointSystem::SetOnRespawnCallback(RespawnCallback cb, void* user) noexcept {
    _on_respawn      = cb;
    _on_respawn_user = user;
}

// =============================================================================
// 一括破棄
// =============================================================================

void CheckpointSystem::ClearAll() noexcept {
    _slots.Clear();
    _unlocked.Clear();
    _scratch.Clear();
    _current          = CheckpointId{};
    _last_level_index = 0;
    _last_spawn_pos   = Vec2::Zero();
    _checkpoint_count = 0;
    // callback 設定は保持 (Progression::ResetProgress / HealthSystem::ClearAll と同方針)。
}

} // namespace acs::game
