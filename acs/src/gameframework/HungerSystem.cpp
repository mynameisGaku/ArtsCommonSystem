// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R/I — HungerSystem 実装
//
// slot+gen パターンで複数 survivor の生存統計値 (空腹/喉/疲労/正気/体温/Custom1/2)
// を管理する。Tick で decay + critical 跨ぎ検出 + zero 到達時の HP ダメージ通知を
// 一元的に処理し、コールバックは Tick 内のみで発火する (= 再入安全)。
#include "gameframework/HungerSystem.h"

namespace acs::game {

// ----------------------------------------------------------------------------
// helpers
// ----------------------------------------------------------------------------

f32 HungerSystem::Clamp(f32 v, f32 lo, f32 hi) noexcept {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

u32 HungerSystem::StatIndex(ESurvivalStat stat) noexcept {
    const u32 idx = static_cast<u32>(stat);
    if (idx >= kSurvivalStatCount) return ~0u;
    return idx;
}

HungerSystem::SurvivorSlot* HungerSystem::ResolveSurvivor(SurvivorId id) noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx == 0u || idx >= _survivors.Size()) return nullptr;
    SurvivorSlot& s = _survivors[idx];
    if (!s.in_use) return nullptr;
    if (s.gen != id.Gen()) return nullptr;
    return &s;
}

const HungerSystem::SurvivorSlot* HungerSystem::ResolveSurvivor(SurvivorId id) const noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx == 0u || idx >= _survivors.Size()) return nullptr;
    const SurvivorSlot& s = _survivors[idx];
    if (!s.in_use) return nullptr;
    if (s.gen != id.Gen()) return nullptr;
    return &s;
}

// ----------------------------------------------------------------------------
// 初期化 / config
// ----------------------------------------------------------------------------

void HungerSystem::Init() noexcept {
    // 既存 survivor / config を全破棄して再初期化する。
    _survivors.Clear();
    _survivor_count = 0u;
    _configs.Clear();

    // 7 stat 分の default config を確保。default 値は StatConfig 既定 (max=100,
    // decay=0, critical=20, zero_dmg=0)。caller が ConfigureStat で上書きする。
    for (u32 i = 0; i < kSurvivalStatCount; ++i) {
        _configs.PushBack(StatConfig{});
    }
    // index 0 dummy slot を確保 (= SurvivorId._packed == 0 を invalid 予約)。
    _survivors.PushBack(SurvivorSlot{});
}

void HungerSystem::ConfigureStat(ESurvivalStat stat, const StatConfig& config) noexcept {
    const u32 idx = StatIndex(stat);
    if (idx == ~0u) return;
    if (_configs.Size() < kSurvivalStatCount) return;   // Init 未呼び出し防御

    StatConfig sanitized = config;
    if (sanitized.max_value <= 0.0f) sanitized.max_value = 1.0f;   // 0/負値は防御的に 1.0
    if (sanitized.critical_threshold < 0.0f) sanitized.critical_threshold = 0.0f;
    if (sanitized.critical_threshold > sanitized.max_value) {
        sanitized.critical_threshold = sanitized.max_value;
    }
    // decay_per_sec / zero_damage_per_sec は負値 (= 自然回復 / 回復ダメージ) を
    // 認めない設計。負なら 0 に丸める。
    if (sanitized.decay_per_sec < 0.0f) sanitized.decay_per_sec = 0.0f;
    if (sanitized.zero_damage_per_sec < 0.0f) sanitized.zero_damage_per_sec = 0.0f;

    _configs[idx] = sanitized;
}

// ----------------------------------------------------------------------------
// survivor 管理
// ----------------------------------------------------------------------------

SurvivorId HungerSystem::AddSurvivor() noexcept {
    if (_configs.Size() < kSurvivalStatCount) return SurvivorId{};   // Init 未呼び出し

    // 空き slot を線形検索 (index 0 は dummy なので 1 から)。
    u32 found = 0u;
    for (u32 i = 1u; i < _survivors.Size(); ++i) {
        if (!_survivors[i].in_use) { found = i; break; }
    }

    if (found == 0u) {
        // 末尾に新規拡張。24bit index 上限を超えるなら invalid 返却。
        if (_survivors.Size() > SurvivorId::kMaxIndex) return SurvivorId{};
        _survivors.PushBack(SurvivorSlot{});
        found = static_cast<u32>(_survivors.Size() - 1u);
    }

    SurvivorSlot& s = _survivors[found];
    // gen を進める (0 は invalid 予約なので skip)。
    s.gen = static_cast<u8>(s.gen + 1u);
    if (s.gen == 0u) s.gen = 1u;
    s.in_use = true;

    // stat 配列を 7 個用意 (再利用 slot の場合は既存があるので Clear で 0 戻し)。
    s.stats.Clear();
    for (u32 i = 0u; i < kSurvivalStatCount; ++i) {
        const StatConfig& cfg = _configs[i];
        StatState st;
        st.current     = cfg.max_value;     // 全 stat を max でスタート
        st.max         = cfg.max_value;
        st.is_critical = (cfg.max_value < cfg.critical_threshold);  // 通常 false
        s.stats.PushBack(st);
    }

    ++_survivor_count;
    return SurvivorId::Pack(found, s.gen);
}

void HungerSystem::RemoveSurvivor(SurvivorId id) noexcept {
    SurvivorSlot* s = ResolveSurvivor(id);
    if (s == nullptr) return;

    s->in_use = false;
    s->stats.Clear();
    // gen はそのまま (次回 AddSurvivor で +1 されて古い handle 無効化)。

    if (_survivor_count > 0u) --_survivor_count;
}

// ----------------------------------------------------------------------------
// stat 操作
// ----------------------------------------------------------------------------

void HungerSystem::RestoreStat(SurvivorId id, ESurvivalStat stat, f32 amount) noexcept {
    if (amount <= 0.0f) return;
    SurvivorSlot* s = ResolveSurvivor(id);
    if (s == nullptr) return;
    const u32 sidx = StatIndex(stat);
    if (sidx == ~0u || sidx >= s->stats.Size()) return;

    StatState& st = s->stats[sidx];
    st.current = Clamp(st.current + amount, 0.0f, st.max);
}

void HungerSystem::DrainStat(SurvivorId id, ESurvivalStat stat, f32 amount) noexcept {
    if (amount <= 0.0f) return;
    SurvivorSlot* s = ResolveSurvivor(id);
    if (s == nullptr) return;
    const u32 sidx = StatIndex(stat);
    if (sidx == ~0u || sidx >= s->stats.Size()) return;

    StatState& st = s->stats[sidx];
    st.current = Clamp(st.current - amount, 0.0f, st.max);
    // critical / damage callback は Tick で一元管理するため、ここでは発火しない。
}

void HungerSystem::SetStat(SurvivorId id, ESurvivalStat stat, f32 value) noexcept {
    SurvivorSlot* s = ResolveSurvivor(id);
    if (s == nullptr) return;
    const u32 sidx = StatIndex(stat);
    if (sidx == ~0u || sidx >= s->stats.Size()) return;

    StatState& st = s->stats[sidx];
    st.current = Clamp(value, 0.0f, st.max);
}

// ----------------------------------------------------------------------------
// 問い合わせ
// ----------------------------------------------------------------------------

f32 HungerSystem::GetStat(SurvivorId id, ESurvivalStat stat) const noexcept {
    const SurvivorSlot* s = ResolveSurvivor(id);
    if (s == nullptr) return 0.0f;
    const u32 sidx = StatIndex(stat);
    if (sidx == ~0u || sidx >= s->stats.Size()) return 0.0f;
    return s->stats[sidx].current;
}

bool HungerSystem::IsCritical(SurvivorId id, ESurvivalStat stat) const noexcept {
    const SurvivorSlot* s = ResolveSurvivor(id);
    if (s == nullptr) return false;
    const u32 sidx = StatIndex(stat);
    if (sidx == ~0u || sidx >= s->stats.Size()) return false;
    return s->stats[sidx].is_critical;
}

bool HungerSystem::IsAlive(SurvivorId id) const noexcept {
    const SurvivorSlot* s = ResolveSurvivor(id);
    if (s == nullptr) return false;

    // 全 stat が 0 では無い (= どれか 1 個でも 0 なら「生存だが瀕死状態」だが
    // 「生きてはいる」)。仕様文「全 stat が 0 ではない」を素直に解釈し、
    // 「全 stat が 0 だったら死亡」と解釈する (= 全ての生存パラメータが
    // 尽きたら HungerSystem の世界では死亡扱い)。HealthBridge 未実装の現状
    // では stat 部分のみで判定する。
    bool any_nonzero = false;
    const usize n = s->stats.Size();
    for (usize i = 0; i < n; ++i) {
        if (s->stats[i].current > 0.0f) { any_nonzero = true; break; }
    }
    if (!any_nonzero) return false;

    // HealthBridge (= HP > 0 判定の外部関数) は将来の拡張枠。現状は stat 部分
    // が生存していれば true。bridge 未セットの場合は stat 判定のみが採用される。
    return true;
}

f32 HungerSystem::OverallSurvivalHealth(SurvivorId id) const noexcept {
    const SurvivorSlot* s = ResolveSurvivor(id);
    if (s == nullptr) return 0.0f;

    const usize n = s->stats.Size();
    if (n == 0) return 0.0f;

    f32 sum_fraction = 0.0f;
    u32 valid_count  = 0u;
    for (usize i = 0; i < n; ++i) {
        const StatState& st = s->stats[i];
        if (st.max <= 0.0f) continue;   // 無効な max は無視
        const f32 frac = st.current / st.max;
        sum_fraction += Clamp(frac, 0.0f, 1.0f);
        ++valid_count;
    }
    if (valid_count == 0u) return 0.0f;
    return sum_fraction / static_cast<f32>(valid_count);
}

// ----------------------------------------------------------------------------
// driver
// ----------------------------------------------------------------------------

void HungerSystem::Tick(f32 dt) noexcept {
    if (dt <= 0.0f) return;
    if (_configs.Size() < kSurvivalStatCount) return;   // Init 未呼び出し防御

    const usize n_surv = _survivors.Size();
    // index 0 は dummy なので 1 から走査。
    for (usize si = 1u; si < n_surv; ++si) {
        SurvivorSlot& slot = _survivors[si];
        if (!slot.in_use) continue;

        const usize n_stat = slot.stats.Size();
        for (usize st_i = 0u; st_i < n_stat && st_i < kSurvivalStatCount; ++st_i) {
            StatState&        st  = slot.stats[st_i];
            const StatConfig& cfg = _configs[st_i];

            // 1) 自然減少を適用。decay_per_sec <= 0 なら不変。
            if (cfg.decay_per_sec > 0.0f) {
                st.current -= cfg.decay_per_sec * dt;
                if (st.current < 0.0f) st.current = 0.0f;
                if (st.current > st.max) st.current = st.max;
            }

            // 2) critical 跨ぎ検出。threshold より下なら critical, 上なら recover。
            const bool was_critical = st.is_critical;
            const bool now_critical = (st.current < cfg.critical_threshold);
            if (now_critical != was_critical) {
                st.is_critical = now_critical;
                if (_on_critical != nullptr) {
                    const SurvivorId id = SurvivorId::Pack(static_cast<u32>(si), slot.gen);
                    _on_critical(_on_critical_user, id,
                                 static_cast<ESurvivalStat>(st_i), now_critical);
                }
            }

            // 3) stat==0 の場合は zero_damage_per_sec * dt を HealthBridge へ通知。
            //    zero_damage_per_sec <= 0 なら通知しない (= 不快なだけで死なない stat)。
            if (st.current <= 0.0f && cfg.zero_damage_per_sec > 0.0f) {
                if (_on_damage != nullptr) {
                    const f32 dmg = cfg.zero_damage_per_sec * dt;
                    const SurvivorId id = SurvivorId::Pack(static_cast<u32>(si), slot.gen);
                    _on_damage(_on_damage_user, id,
                               static_cast<ESurvivalStat>(st_i), dmg);
                }
            }
        }
    }
}

// ----------------------------------------------------------------------------
// callback
// ----------------------------------------------------------------------------

void HungerSystem::SetOnCriticalCallback(CriticalCallback cb, void* user) noexcept {
    _on_critical      = cb;
    _on_critical_user = user;
}

void HungerSystem::SetOnDamageCallback(DamageCallback cb, void* user) noexcept {
    _on_damage      = cb;
    _on_damage_user = user;
}

// ----------------------------------------------------------------------------
// 一括破棄
// ----------------------------------------------------------------------------

void HungerSystem::ClearAll() noexcept {
    // 全 survivor + stat config を default に戻し、index 0 dummy を再確保。
    _survivors.Clear();
    _configs.Clear();
    _survivor_count = 0u;
    for (u32 i = 0u; i < kSurvivalStatCount; ++i) {
        _configs.PushBack(StatConfig{});
    }
    _survivors.PushBack(SurvivorSlot{});
    // callback は保持 (ClearAll は entity 全消去のみ、Director 結線は維持)。
}

} // namespace acs::game
