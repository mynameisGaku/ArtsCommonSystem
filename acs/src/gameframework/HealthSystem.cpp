// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R/I — FHealthSystem 実装
//
// slot+gen パターンで複数 entity の HP / 死亡 / 復活 / 無敵時間を管理する。
// DeathCallback は is_alive=false の確定後に発火するため、callback 内で同
// entity を Revive することも可能 (再入安全)。
#include "gameframework/HealthSystem.h"

namespace acs::game {

/** 値を [lo, hi] に clamp する。 */
f32 FHealthSystem::Clamp(f32 v, f32 lo, f32 hi) noexcept {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/** 空き slot を線形検索し、無ければ末尾を拡張してインデックスを返す。 */
u32 FHealthSystem::AcquireSlot() noexcept {
    // index 0 を invalid 予約として残す (dummy slot)。
    for (u32 i = 1; i < m_Slots.Size(); ++i) {
        if (!m_Slots[i].active) return i;
    }
    if (m_Slots.IsEmpty()) {
        m_Slots.PushBack({});   // dummy at index 0
    }
    m_Slots.PushBack({});
    return static_cast<u32>(m_Slots.Size()) - 1u;
}

/** id が有効 (範囲内 + active + generation 一致) なら slot を返す。 */
FHealthSystem::Slot* FHealthSystem::FindSlot(FHealthId id) noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx == 0 || idx >= m_Slots.Size()) return nullptr;
    Slot& s = m_Slots[idx];
    if (!s.active) return nullptr;
    if (s.gen != id.Generation()) return nullptr;
    return &s;
}

/** id が有効なら slot を返す (const 版)。 */
const FHealthSystem::Slot* FHealthSystem::FindSlot(FHealthId id) const noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx == 0 || idx >= m_Slots.Size()) return nullptr;
    const Slot& s = m_Slots[idx];
    if (!s.active) return nullptr;
    if (s.gen != id.Generation()) return nullptr;
    return &s;
}

/** 空き slot を確保し full HP・生存状態で entity を登録して ID を返す。 */
FHealthId FHealthSystem::Spawn(f32 max_hp) noexcept {
    if (max_hp <= 0.0f) max_hp = 1.0f;   // 0 以下は防御的に 1 HP

    const u32 idx = AcquireSlot();
    Slot& s = m_Slots[idx];

    // generation を進める (0 は invalid 扱いなので回避)。
    s.gen = static_cast<u8>(s.gen + 1u);
    if (s.gen == 0) s.gen = 1;

    s.active                = true;
    s.state.current_hp      = max_hp;
    s.state.max_hp          = max_hp;
    s.state.is_alive        = true;
    s.state.invulnerable    = false;
    s.state.invuln_timer    = 0.0f;

    ++m_EntityCount;
    return FHealthId{idx, s.gen};
}

/** slot を active=false にして entity を破棄する (generation は次の Spawn で進む)。 */
void FHealthSystem::Despawn(FHealthId id) noexcept {
    Slot* s = FindSlot(id);
    if (s == nullptr) return;
    // generation はそのままで active=false に。次の Spawn で gen が +1 されて
    // 古い handle が無効化される。
    s->active           = false;
    s->state.current_hp = 0.0f;
    s->state.is_alive   = false;
    s->state.invulnerable = false;
    s->state.invuln_timer = 0.0f;
    if (m_EntityCount > 0) --m_EntityCount;
}

/** HP を減算し、0 以下なら死亡確定 → DeathCallback 発火。致死なら true。 */
bool FHealthSystem::ApplyDamage(FHealthId id, f32 amount, EDamageType type) noexcept {
    Slot* s = FindSlot(id);
    if (s == nullptr) return false;
    if (!s->state.is_alive) return false;   // 既に死亡 → no-op
    if (s->state.invulnerable) return false; // 無敵中 → no-op
    if (amount <= 0.0f) return false;        // 0/負ダメージは無視

    s->state.current_hp -= amount;

    if (s->state.current_hp <= 0.0f) {
        // 致死: state 確定 → callback 発火 の順 (再入安全)。
        s->state.current_hp = 0.0f;
        s->state.is_alive   = false;
        // 死亡時は invuln も解除しておく (Revive 時に明示再付与する設計)。
        s->state.invulnerable = false;
        s->state.invuln_timer = 0.0f;

        if (m_OnDeath != nullptr) {
            m_OnDeath(m_OnDeathUser, id, type);
        }
        return true;
    }
    return false;
}

/** HP を加算し max_hp で clamp する (is_alive は変動させない)。 */
void FHealthSystem::Heal(FHealthId id, f32 amount) noexcept {
    Slot* s = FindSlot(id);
    if (s == nullptr) return;
    if (amount <= 0.0f) return;

    s->state.current_hp += amount;
    if (s->state.current_hp > s->state.max_hp) {
        s->state.current_hp = s->state.max_hp;
    }
    // 死亡中の Heal も HP は加算するが、is_alive は変動しない (Revive 専用)。
}

/** 無敵タイマを設定する (既存より長い場合のみ延長、0 以下で即解除)。 */
void FHealthSystem::SetInvulnerable(FHealthId id, f32 duration_sec) noexcept {
    Slot* s = FindSlot(id);
    if (s == nullptr) return;
    if (duration_sec <= 0.0f) {
        // 0 以下指定で即解除する用途も想定。
        s->state.invulnerable = false;
        s->state.invuln_timer = 0.0f;
        return;
    }
    // 既に invuln 中なら max(残り, 新規) で延長 (短い無敵で上書きしない)。
    if (s->state.invulnerable && s->state.invuln_timer > duration_sec) {
        // 既存の方が長い → 据置
        return;
    }
    s->state.invulnerable = true;
    s->state.invuln_timer = duration_sec;
}

/** 死亡中の entity を hp_fraction (最低 1 HP) で復活させる。 */
void FHealthSystem::Revive(FHealthId id, f32 hp_fraction) noexcept {
    Slot* s = FindSlot(id);
    if (s == nullptr) return;
    if (s->state.is_alive) return;   // 生存中は no-op

    // hp_fraction を [0, 1] に clamp し、最低 1 HP は確保する。
    const f32 frac = Clamp(hp_fraction, 0.0f, 1.0f);
    f32 hp = s->state.max_hp * frac;
    if (hp < 1.0f) hp = 1.0f;
    if (hp > s->state.max_hp) hp = s->state.max_hp;

    s->state.current_hp   = hp;
    s->state.is_alive     = true;
    // 復活時は invuln フラグはそのまま (caller が SetInvulnerable で別途付与可能)。
}

/** HP 上限を更新し、refill 指定なら current_hp も揃える (is_alive は据置)。 */
void FHealthSystem::SetMaxHp(FHealthId id, f32 new_max, bool refill) noexcept {
    Slot* s = FindSlot(id);
    if (s == nullptr) return;
    if (new_max <= 0.0f) new_max = 1.0f;

    s->state.max_hp = new_max;
    if (refill) {
        s->state.current_hp = new_max;
        // refill した時点で生存判定 (HP > 0 なので is_alive=true へ昇格)。
        // ただし「死亡中の entity を refill だけで復活させない」設計を選び、
        // is_alive は据置とする (= 復活は Revive 経由のみ)。
    } else {
        // current_hp を new_max で clamp。
        if (s->state.current_hp > new_max) {
            s->state.current_hp = new_max;
        }
    }
}

/** id が有効なら HP 状態への const 参照を返す (無効なら nullptr)。 */
const HealthState* FHealthSystem::GetState(FHealthId id) const noexcept {
    const Slot* s = FindSlot(id);
    if (s == nullptr) return nullptr;
    return &s->state;
}

/** 現在 HP を返す (無効 id なら 0.0)。 */
f32 FHealthSystem::GetCurrentHp(FHealthId id) const noexcept {
    const Slot* s = FindSlot(id);
    if (s == nullptr) return 0.0f;
    return s->state.current_hp;
}

/** current_hp / max_hp を返す (無効 id or max_hp<=0 なら 0.0)。 */
f32 FHealthSystem::GetHpFraction(FHealthId id) const noexcept {
    const Slot* s = FindSlot(id);
    if (s == nullptr) return 0.0f;
    if (s->state.max_hp <= 0.0f) return 0.0f;
    return s->state.current_hp / s->state.max_hp;
}

/** 生存中かを返す (無効 id なら false)。 */
bool FHealthSystem::IsAlive(FHealthId id) const noexcept {
    const Slot* s = FindSlot(id);
    if (s == nullptr) return false;
    return s->state.is_alive;
}

/** 無敵中かを返す (無効 id なら false)。 */
bool FHealthSystem::IsInvulnerable(FHealthId id) const noexcept {
    const Slot* s = FindSlot(id);
    if (s == nullptr) return false;
    return s->state.invulnerable;
}

/** active かつ is_alive な entity を数えて返す。 */
u32 FHealthSystem::AliveCount() const noexcept {
    u32 count = 0;
    const usize n = m_Slots.Size();
    // index 0 は dummy なので 1 から走査。
    for (usize i = 1; i < n; ++i) {
        const Slot& s = m_Slots[i];
        if (s.active && s.state.is_alive) ++count;
    }
    return count;
}

/** 全 active entity の invuln_timer を減算し、0 到達で無敵を解除する。 */
void FHealthSystem::Tick(f32 dt) noexcept {
    if (dt <= 0.0f) return;
    const usize n = m_Slots.Size();
    for (usize i = 1; i < n; ++i) {
        Slot& s = m_Slots[i];
        if (!s.active) continue;
        if (!s.state.invulnerable) continue;

        s.state.invuln_timer -= dt;
        if (s.state.invuln_timer <= 0.0f) {
            s.state.invuln_timer = 0.0f;
            s.state.invulnerable = false;
        }
    }
}

/** 死亡時コールバックと user ポインタを設定する。 */
void FHealthSystem::SetOnDeathCallback(DeathCallback cb, void* user) noexcept {
    m_OnDeath      = cb;
    m_OnDeathUser = user;
}

/** slot 配列をクリアして全 entity を破棄する (callback は保持)。 */
void FHealthSystem::ClearAll() noexcept {
    m_Slots.Clear();
    m_EntityCount = 0;
    // callback は保持 (ClearAll は entity 全消去のみ、ディレクタ結線は維持)。
}

} // namespace acs::game
