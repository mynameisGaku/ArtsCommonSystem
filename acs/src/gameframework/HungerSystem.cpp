// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R/I — CHungerSystem 実装
//
// slot+gen パターンで複数 survivor の生存統計値 (空腹/喉/疲労/正気/体温/Custom1/2)
// を管理する。Tick で decay + critical 跨ぎ検出 + zero 到達時の HP ダメージ通知を
// 一元的に処理し、コールバックは Tick 内のみで発火する (= 再入安全)。
#include "gameframework/HungerSystem.h"

namespace acs::game {

/** 値を [lo, hi] に制限して返す。 */
f32 CHungerSystem::Clamp(f32 v, f32 lo, f32 hi) noexcept {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/** stat enum を array index に変換する (範囲外は ~0u で哨兵)。 */
u32 CHungerSystem::StatIndex(ESurvivalStat stat) noexcept {
    const u32 idx = static_cast<u32>(stat);
    if (idx >= kSurvivalStatCount) return ~0u;
    return idx;
}

/** survivor handle から slot を解決する (gen 一致 + in_use + 範囲チェック、non-const)。 */
CHungerSystem::FSurvivorSlot* CHungerSystem::ResolveSurvivor(FSurvivorId id) noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx == 0u || idx >= m_Survivors.Size()) return nullptr;
    FSurvivorSlot& s = m_Survivors[idx];
    if (!s.in_use) return nullptr;
    if (s.gen != id.Gen()) return nullptr;
    return &s;
}

/** survivor handle から slot を解決する (gen 一致 + in_use + 範囲チェック、const)。 */
const CHungerSystem::FSurvivorSlot* CHungerSystem::ResolveSurvivor(FSurvivorId id) const noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx == 0u || idx >= m_Survivors.Size()) return nullptr;
    const FSurvivorSlot& s = m_Survivors[idx];
    if (!s.in_use) return nullptr;
    if (s.gen != id.Gen()) return nullptr;
    return &s;
}

/** 既存 survivor / config を全破棄し、7 stat 分の default config と dummy slot を確保する。 */
void CHungerSystem::Init() noexcept {
    // 既存 survivor / config を全破棄して再初期化する。
    m_Survivors.Clear();
    m_SurvivorCount = 0u;
    m_Configs.Clear();

    // 7 stat 分の default config を確保。default 値は StatConfig 既定 (max=100,
    // decay=0, critical=20, zero_dmg=0)。caller が ConfigureStat で上書きする。
    for (u32 i = 0; i < kSurvivalStatCount; ++i) {
        m_Configs.PushBack(FStatConfig{});
    }
    // index 0 dummy slot を確保 (= SurvivorId.m_Packed == 0 を invalid 予約)。
    m_Survivors.PushBack(FSurvivorSlot{});
}

/** stat 種別の config を sanitize (負値 / max 超過を補正) して上書きする。 */
void CHungerSystem::ConfigureStat(ESurvivalStat stat, const FStatConfig& config) noexcept {
    const u32 idx = StatIndex(stat);
    if (idx == ~0u) return;
    if (m_Configs.Size() < kSurvivalStatCount) return;   // Init 未呼び出し防御

    FStatConfig sanitized = config;
    if (sanitized.max_value <= 0.0f) sanitized.max_value = 1.0f;   // 0/負値は防御的に 1.0
    if (sanitized.critical_threshold < 0.0f) sanitized.critical_threshold = 0.0f;
    if (sanitized.critical_threshold > sanitized.max_value) {
        sanitized.critical_threshold = sanitized.max_value;
    }
    // decay_per_sec / zero_damage_per_sec は負値 (= 自然回復 / 回復ダメージ) を
    // 認めない設計。負なら 0 に丸める。
    if (sanitized.decay_per_sec < 0.0f) sanitized.decay_per_sec = 0.0f;
    if (sanitized.zero_damage_per_sec < 0.0f) sanitized.zero_damage_per_sec = 0.0f;

    m_Configs[idx] = sanitized;
}

/** 空き slot を再利用または末尾拡張で確保し、全 stat を max で初期化して handle を返す。 */
FSurvivorId CHungerSystem::AddSurvivor() noexcept {
    if (m_Configs.Size() < kSurvivalStatCount) return FSurvivorId{};   // Init 未呼び出し

    // 空き slot を線形検索 (index 0 は dummy なので 1 から)。
    u32 found = 0u;
    for (u32 i = 1u; i < m_Survivors.Size(); ++i) {
        if (!m_Survivors[i].in_use) { found = i; break; }
    }

    if (found == 0u) {
        // 末尾に新規拡張。24bit index 上限を超えるなら invalid 返却。
        if (m_Survivors.Size() > FSurvivorId::kMaxIndex) return FSurvivorId{};
        m_Survivors.PushBack(FSurvivorSlot{});
        found = static_cast<u32>(m_Survivors.Size() - 1u);
    }

    FSurvivorSlot& s = m_Survivors[found];
    // gen を進める (0 は invalid 予約なので skip)。
    s.gen = static_cast<u8>(s.gen + 1u);
    if (s.gen == 0u) s.gen = 1u;
    s.in_use = true;

    // stat 配列を 7 個用意 (再利用 slot の場合は既存があるので Clear で 0 戻し)。
    s.stats.Clear();
    for (u32 i = 0u; i < kSurvivalStatCount; ++i) {
        const FStatConfig& cfg = m_Configs[i];
        FStatState st;
        st.current     = cfg.max_value;     // 全 stat を max でスタート
        st.max         = cfg.max_value;
        st.is_critical = (cfg.max_value < cfg.critical_threshold);  // 通常 false
        s.stats.PushBack(st);
    }

    ++m_SurvivorCount;
    return FSurvivorId::Pack(found, s.gen);
}

/** slot を解放して in_use=false にし、active 数を 1 減らす (gen は次回確保で進む)。 */
void CHungerSystem::RemoveSurvivor(FSurvivorId id) noexcept {
    FSurvivorSlot* s = ResolveSurvivor(id);
    if (s == nullptr) return;

    s->in_use = false;
    s->stats.Clear();
    // gen はそのまま (次回 AddSurvivor で +1 されて古い handle 無効化)。

    if (m_SurvivorCount > 0u) --m_SurvivorCount;
}

/** stat に amount を加算し clamp(0, max) する。 */
void CHungerSystem::RestoreStat(FSurvivorId id, ESurvivalStat stat, f32 amount) noexcept {
    if (amount <= 0.0f) return;
    FSurvivorSlot* s = ResolveSurvivor(id);
    if (s == nullptr) return;
    const u32 sidx = StatIndex(stat);
    if (sidx == ~0u || sidx >= s->stats.Size()) return;

    FStatState& st = s->stats[sidx];
    st.current = Clamp(st.current + amount, 0.0f, st.max);
}

/** stat から amount を減算し clamp(0, max) する (callback は Tick に委ねる)。 */
void CHungerSystem::DrainStat(FSurvivorId id, ESurvivalStat stat, f32 amount) noexcept {
    if (amount <= 0.0f) return;
    FSurvivorSlot* s = ResolveSurvivor(id);
    if (s == nullptr) return;
    const u32 sidx = StatIndex(stat);
    if (sidx == ~0u || sidx >= s->stats.Size()) return;

    FStatState& st = s->stats[sidx];
    st.current = Clamp(st.current - amount, 0.0f, st.max);
    // critical / damage callback は Tick で一元管理するため、ここでは発火しない。
}

/** stat を value で絶対設定し clamp(0, max) する。 */
void CHungerSystem::SetStat(FSurvivorId id, ESurvivalStat stat, f32 value) noexcept {
    FSurvivorSlot* s = ResolveSurvivor(id);
    if (s == nullptr) return;
    const u32 sidx = StatIndex(stat);
    if (sidx == ~0u || sidx >= s->stats.Size()) return;

    FStatState& st = s->stats[sidx];
    st.current = Clamp(value, 0.0f, st.max);
}

/** stat の現在値を返す (解決失敗 / 範囲外は 0.0f)。 */
f32 CHungerSystem::GetStat(FSurvivorId id, ESurvivalStat stat) const noexcept {
    const FSurvivorSlot* s = ResolveSurvivor(id);
    if (s == nullptr) return 0.0f;
    const u32 sidx = StatIndex(stat);
    if (sidx == ~0u || sidx >= s->stats.Size()) return 0.0f;
    return s->stats[sidx].current;
}

/** stat の is_critical フラグを返す (解決失敗 / 範囲外は false)。 */
bool CHungerSystem::IsCritical(FSurvivorId id, ESurvivalStat stat) const noexcept {
    const FSurvivorSlot* s = ResolveSurvivor(id);
    if (s == nullptr) return false;
    const u32 sidx = StatIndex(stat);
    if (sidx == ~0u || sidx >= s->stats.Size()) return false;
    return s->stats[sidx].is_critical;
}

/** 全 stat のうち 1 個でも非 0 なら生存とみなす (HealthBridge は将来拡張)。 */
bool CHungerSystem::IsAlive(FSurvivorId id) const noexcept {
    const FSurvivorSlot* s = ResolveSurvivor(id);
    if (s == nullptr) return false;

    // 全 stat が 0 では無い (= どれか 1 個でも 0 なら「生存だが瀕死状態」だが
    // 「生きてはいる」)。仕様文「全 stat が 0 ではない」を素直に解釈し、
    // 「全 stat が 0 だったら死亡」と解釈する (= 全ての生存パラメータが
    // 尽きたら CHungerSystem の世界では死亡扱い)。HealthBridge 未実装の現状
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

/** 有効 max を持つ全 stat の (current / max) を [0,1] で平均して返す。 */
f32 CHungerSystem::OverallSurvivalHealth(FSurvivorId id) const noexcept {
    const FSurvivorSlot* s = ResolveSurvivor(id);
    if (s == nullptr) return 0.0f;

    const usize n = s->stats.Size();
    if (n == 0) return 0.0f;

    f32 sum_fraction = 0.0f;
    u32 valid_count  = 0u;
    for (usize i = 0; i < n; ++i) {
        const FStatState& st = s->stats[i];
        if (st.max <= 0.0f) continue;   // 無効な max は無視
        const f32 frac = st.current / st.max;
        sum_fraction += Clamp(frac, 0.0f, 1.0f);
        ++valid_count;
    }
    if (valid_count == 0u) return 0.0f;
    return sum_fraction / static_cast<f32>(valid_count);
}

/** 全 survivor × 全 stat に decay を適用し、critical 跨ぎと zero ダメージを callback 通知する。 */
void CHungerSystem::Tick(f32 dt) noexcept {
    if (dt <= 0.0f) return;
    if (m_Configs.Size() < kSurvivalStatCount) return;   // Init 未呼び出し防御

    const usize n_surv = m_Survivors.Size();
    // index 0 は dummy なので 1 から走査。
    for (usize si = 1u; si < n_surv; ++si) {
        FSurvivorSlot& slot = m_Survivors[si];
        if (!slot.in_use) continue;

        const usize n_stat = slot.stats.Size();
        for (usize st_i = 0u; st_i < n_stat && st_i < kSurvivalStatCount; ++st_i) {
            FStatState&        st  = slot.stats[st_i];
            const FStatConfig& cfg = m_Configs[st_i];

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
                if (m_OnCritical != nullptr) {
                    const FSurvivorId id = FSurvivorId::Pack(static_cast<u32>(si), slot.gen);
                    m_OnCritical(m_OnCriticalUser, id,
                                 static_cast<ESurvivalStat>(st_i), now_critical);
                }
            }

            // 3) stat==0 の場合は zero_damage_per_sec * dt を HealthBridge へ通知。
            //    zero_damage_per_sec <= 0 なら通知しない (= 不快なだけで死なない stat)。
            if (st.current <= 0.0f && cfg.zero_damage_per_sec > 0.0f) {
                if (m_OnDamage != nullptr) {
                    const f32 dmg = cfg.zero_damage_per_sec * dt;
                    const FSurvivorId id = FSurvivorId::Pack(static_cast<u32>(si), slot.gen);
                    m_OnDamage(m_OnDamageUser, id,
                               static_cast<ESurvivalStat>(st_i), dmg);
                }
            }
        }
    }
}

/** critical コールバックと user ポインタを設定する。 */
void CHungerSystem::SetOnCriticalCallback(CriticalCallback cb, void* user) noexcept {
    m_OnCritical      = cb;
    m_OnCriticalUser = user;
}

/** damage コールバックと user ポインタを設定する。 */
void CHungerSystem::SetOnDamageCallback(DamageCallback cb, void* user) noexcept {
    m_OnDamage      = cb;
    m_OnDamageUser = user;
}

/** 全 survivor + stat config を default に戻し dummy slot を再確保する (callback は保持)。 */
void CHungerSystem::ClearAll() noexcept {
    // 全 survivor + stat config を default に戻し、index 0 dummy を再確保。
    m_Survivors.Clear();
    m_Configs.Clear();
    m_SurvivorCount = 0u;
    for (u32 i = 0u; i < kSurvivalStatCount; ++i) {
        m_Configs.PushBack(FStatConfig{});
    }
    m_Survivors.PushBack(FSurvivorSlot{});
    // callback は保持 (ClearAll は entity 全消去のみ、Director 結線は維持)。
}

} // namespace acs::game
