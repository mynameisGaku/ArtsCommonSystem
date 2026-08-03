// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R — CCooldownTimer 実装
//
// 設計メモ:
//  ・slot 再利用は線形走査 (CSceneTimer / CCollisionWorld2D と同方針)。アクター 1
//    体あたりの cooldown 数はせいぜい数〜数十、システム全体でも数百以下が想定
//    なので O(N) で十分。
//  ・generation は u8 (0=未使用、1〜255 が有効)。255 で wrap して 1 に戻す
//    (0 にすると IsValid が常に false になり stale 検知不能になるため)。
//  ・charged 遷移は Tick / ForceReady でしか起こさない。TryUse は逆方向
//    (charged=true → false) のみで callback は呼ばない (= プレイヤー入力起因
//    の遷移なので caller 側で既に処理済の想定)。
//  ・Tick 中に ReadyCallback が新規 Register/Unregister/ClearAll を呼ぶケースに
//    備え、index アクセスで回し、毎反復で `m_Slots.Size()` / active を確認する。
//  ・SetDuration: charged を維持するか reload に戻すかは「remaining と new_duration
//    の比較」で決める。remaining > new_duration の場合は remaining=new_duration に
//    クランプ (= 短縮で即 charge 寸前まで進む)。charged なら charged のまま。
#include "gameframework/CooldownTimer.h"

namespace acs::game {

/** inactive な slot を再利用し、なければ末尾に追加して index を返す。 */
u32 CCooldownTimer::AcquireSlot() noexcept {
    // 既存の inactive slot を再利用
    const usize n = m_Slots.Num();
    for (usize i = 0; i < n; ++i) {
        if (!m_Slots[i].active) {
            return static_cast<u32>(i);
        }
    }
    // 全 slot 使用中 → 末尾に追加。24bit index 上限を守る。
    if (n >= static_cast<usize>(FCooldownId::kMaxIndex)) {
        return FCooldownId::kMaxIndex; // sentinel: caller 側で invalid 扱い
    }
    m_Slots.Add({});
    return static_cast<u32>(m_Slots.Num()) - 1u;
}

/** index と generation を packed して CooldownId を作る。 */
FCooldownId CCooldownTimer::MakeId(u32 index, u8 gen) const noexcept {
    return FCooldownId::Pack(index, gen);
}

/** handle を解決し、slot active + gen 一致なら slot を返す。 */
CCooldownTimer::FSlot* CCooldownTimer::Resolve(FCooldownId id) noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx >= m_Slots.Num()) return nullptr;
    FSlot& s = m_Slots[idx];
    if (!s.active || s.gen != id.Gen()) return nullptr;
    return &s;
}

/** handle を解決し、slot active + gen 一致なら const slot を返す。 */
const CCooldownTimer::FSlot* CCooldownTimer::Resolve(FCooldownId id) const noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx >= m_Slots.Num()) return nullptr;
    const FSlot& s = m_Slots[idx];
    if (!s.active || s.gen != id.Gen()) return nullptr;
    return &s;
}

/** cooldown を登録し、reload 中扱いの slot を確保して handle を返す。 */
FCooldownId CCooldownTimer::Register(const char* label, f32 duration_sec) noexcept {
    if (duration_sec <= 0.0f) return {};

    const u32 idx = AcquireSlot();
    if (idx >= FCooldownId::kMaxIndex) return {}; // 上限到達

    FSlot& s = m_Slots[idx];
    // generation を 1 進める (0 は未使用扱いなので必ず 1 以上を保つ)
    u8 new_gen = static_cast<u8>(s.gen + 1u);
    if (new_gen == 0u) new_gen = 1u;

    s.label     = label;
    s.duration  = duration_sec;
    s.remaining = duration_sec; // 登録直後は reload 中扱い
    s.charged   = false;
    s.active    = true;
    s.gen       = new_gen;

    ++m_ActiveCount;
    return MakeId(idx, new_gen);
}

/** slot をクリアして active 解除する (gen は次 Acquire で進む)。 */
void CCooldownTimer::Unregister(FCooldownId id) noexcept {
    FSlot* s = Resolve(id);
    if (s == nullptr) return;

    s->active    = false;
    s->label     = nullptr;
    s->duration  = 0.0f;
    s->remaining = 0.0f;
    s->charged   = false;
    // gen はそのまま残す (= 次 Acquire で +1 される)
    if (m_ActiveCount > 0u) --m_ActiveCount;
}

/** charged なら remaining を duration に戻して消費し true を返す。 */
bool CCooldownTimer::TryUse(FCooldownId id) noexcept {
    FSlot* s = Resolve(id);
    if (s == nullptr) return false;
    if (!s->charged) return false;

    // 消費: reload 開始
    s->remaining = s->duration;
    s->charged   = false;
    return true;
}

/** cooldown が charged かを返す (stale handle は false)。 */
bool CCooldownTimer::IsCharged(FCooldownId id) const noexcept {
    const FSlot* s = Resolve(id);
    return s != nullptr && s->charged;
}

/** 残り cooldown 秒数を返す (charged / stale は 0)。 */
f32 CCooldownTimer::Remaining(FCooldownId id) const noexcept {
    const FSlot* s = Resolve(id);
    if (s == nullptr) return 0.0f;
    if (s->charged) return 0.0f;
    return s->remaining > 0.0f ? s->remaining : 0.0f;
}

/** cooldown 進行率 [0,1] を返す (charged は 1.0、stale は 0)。 */
f32 CCooldownTimer::Progress(FCooldownId id) const noexcept {
    const FSlot* s = Resolve(id);
    if (s == nullptr) return 0.0f;
    if (s->charged) return 1.0f;
    if (s->duration <= 0.0f) return 1.0f; // 防御 (Register で弾いているはず)
    const f32 elapsed = s->duration - s->remaining;
    const f32 p = elapsed / s->duration;
    if (p < 0.0f) return 0.0f;
    if (p > 1.0f) return 1.0f;
    return p;
}

/** 即時 charged にし、false→true 遷移なら ReadyCallback を発火する。 */
void CCooldownTimer::ForceReady(FCooldownId id) noexcept {
    FSlot* s = Resolve(id);
    if (s == nullptr) return;
    const bool was_charged = s->charged;
    s->remaining = 0.0f;
    s->charged   = true;
    // false → true 遷移なら callback 発火 (Tick と挙動を揃える)
    if (!was_charged && _ready_cb != nullptr) {
        _ready_cb(_ready_user, id, s->label);
    }
}

/** cooldown を duration にリセットして reload 中 (charged=false) に戻す。 */
void CCooldownTimer::Reset(FCooldownId id) noexcept {
    FSlot* s = Resolve(id);
    if (s == nullptr) return;
    s->remaining = s->duration;
    s->charged   = false;
}

/** cooldown 長を変更し、必要なら remaining を new_duration にクランプする。 */
void CCooldownTimer::SetDuration(FCooldownId id, f32 new_duration_sec) noexcept {
    if (new_duration_sec <= 0.0f) return;
    FSlot* s = Resolve(id);
    if (s == nullptr) return;

    s->duration = new_duration_sec;
    // charged は維持。reload 中は remaining > new_duration ならクランプ
    // (= 短縮された場合に「進捗を巻き戻さない」: より早く ready に到達)。
    if (!s->charged && s->remaining > new_duration_sec) {
        s->remaining = new_duration_sec;
    }
}

/** 全 active slot をクリアして active 解除し、件数を 0 にする。 */
void CCooldownTimer::ClearAll() noexcept {
    const usize n = m_Slots.Num();
    for (usize i = 0; i < n; ++i) {
        FSlot& s = m_Slots[i];
        if (!s.active) continue;
        s.active    = false;
        s.label     = nullptr;
        s.duration  = 0.0f;
        s.remaining = 0.0f;
        s.charged   = false;
    }
    m_ActiveCount = 0u;
}

/** 全 cooldown の remaining を進め、0 到達で charged 遷移 + callback 発火。 */
void CCooldownTimer::Tick(f32 dt) noexcept {
    if (dt <= 0.0f) return;
    if (m_ActiveCount == 0u) return;

    // Tick 中に callback が Register / Unregister / ClearAll を呼んでも安全な
    // ように index アクセスで回す。新規追加された slot は今回の Tick で進行
    // させない (snapshot_size でストップ、登録時の remaining=duration で次 Tick まで保持)。
    const usize snapshot_size = m_Slots.Num();

    for (usize i = 0; i < snapshot_size; ++i) {
        FSlot& s = m_Slots[i];
        if (!s.active) continue;
        if (s.charged) continue; // 既に ready、Tick 不要

        // 進行: remaining を減らす
        s.remaining -= dt;
        if (s.remaining > 0.0f) continue;

        // ---- 0 到達: charged 遷移 ----
        s.remaining = 0.0f;
        s.charged   = true;

        // ReadyCallback 発火。生 reference は握らず、callback 前に必要情報を退避。
        // (callback 内で Unregister が呼ばれて gen が進んだ場合に備える)
        const u8         gen_at_fire = s.gen;
        const char* const label_copy = s.label;
        const FCooldownId id          = MakeId(static_cast<u32>(i), gen_at_fire);

        if (_ready_cb != nullptr) {
            _ready_cb(_ready_user, id, label_copy);
        }
        // callback 内で同 slot が Unregister→再 Register されていれば、次回 Tick で
        // 新世代として扱われる。今 iter は break せず次の slot へ。
    }
}

} // namespace acs::game
