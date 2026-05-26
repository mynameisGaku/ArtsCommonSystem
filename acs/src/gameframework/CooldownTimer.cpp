// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R — FCooldownTimer 実装
//
// 設計メモ:
//  ・slot 再利用は線形走査 (FSceneTimer / FCollisionWorld2D と同方針)。アクター 1
//    体あたりの cooldown 数はせいぜい数〜数十、システム全体でも数百以下が想定
//    なので O(N) で十分。
//  ・generation は u8 (0=未使用、1〜255 が有効)。255 で wrap して 1 に戻す
//    (0 にすると IsValid が常に false になり stale 検知不能になるため)。
//  ・charged 遷移は Tick / ForceReady でしか起こさない。TryUse は逆方向
//    (charged=true → false) のみで callback は呼ばない (= プレイヤー入力起因
//    の遷移なので caller 側で既に処理済の想定)。
//  ・Tick 中に ReadyCallback が新規 Register/Unregister/ClearAll を呼ぶケースに
//    備え、index アクセスで回し、毎反復で `_slots.Size()` / active を確認する。
//  ・SetDuration: charged を維持するか reload に戻すかは「remaining と new_duration
//    の比較」で決める。remaining > new_duration の場合は remaining=new_duration に
//    クランプ (= 短縮で即 charge 寸前まで進む)。charged なら charged のまま。
#include "gameframework/CooldownTimer.h"

namespace acs::game {

u32 FCooldownTimer::AcquireSlot() noexcept {
    // 既存の inactive slot を再利用
    const usize n = _slots.Size();
    for (usize i = 0; i < n; ++i) {
        if (!_slots[i].active) {
            return static_cast<u32>(i);
        }
    }
    // 全 slot 使用中 → 末尾に追加。24bit index 上限を守る。
    if (n >= static_cast<usize>(FCooldownId::kMaxIndex)) {
        return FCooldownId::kMaxIndex; // sentinel: caller 側で invalid 扱い
    }
    _slots.PushBack({});
    return static_cast<u32>(_slots.Size()) - 1u;
}

FCooldownId FCooldownTimer::MakeId(u32 index, u8 gen) const noexcept {
    return FCooldownId::Pack(index, gen);
}

FCooldownTimer::FSlot* FCooldownTimer::Resolve(FCooldownId id) noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx >= _slots.Size()) return nullptr;
    FSlot& s = _slots[idx];
    if (!s.active || s.gen != id.Gen()) return nullptr;
    return &s;
}

const FCooldownTimer::FSlot* FCooldownTimer::Resolve(FCooldownId id) const noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx >= _slots.Size()) return nullptr;
    const FSlot& s = _slots[idx];
    if (!s.active || s.gen != id.Gen()) return nullptr;
    return &s;
}

FCooldownId FCooldownTimer::Register(const char* label, f32 duration_sec) noexcept {
    if (duration_sec <= 0.0f) return {};

    const u32 idx = AcquireSlot();
    if (idx >= FCooldownId::kMaxIndex) return {}; // 上限到達

    FSlot& s = _slots[idx];
    // generation を 1 進める (0 は未使用扱いなので必ず 1 以上を保つ)
    u8 new_gen = static_cast<u8>(s.gen + 1u);
    if (new_gen == 0u) new_gen = 1u;

    s.label     = label;
    s.duration  = duration_sec;
    s.remaining = duration_sec; // 登録直後は reload 中扱い
    s.charged   = false;
    s.active    = true;
    s.gen       = new_gen;

    ++_active_count;
    return MakeId(idx, new_gen);
}

void FCooldownTimer::Unregister(FCooldownId id) noexcept {
    FSlot* s = Resolve(id);
    if (s == nullptr) return;

    s->active    = false;
    s->label     = nullptr;
    s->duration  = 0.0f;
    s->remaining = 0.0f;
    s->charged   = false;
    // gen はそのまま残す (= 次 Acquire で +1 される)
    if (_active_count > 0u) --_active_count;
}

bool FCooldownTimer::TryUse(FCooldownId id) noexcept {
    FSlot* s = Resolve(id);
    if (s == nullptr) return false;
    if (!s->charged) return false;

    // 消費: reload 開始
    s->remaining = s->duration;
    s->charged   = false;
    return true;
}

bool FCooldownTimer::IsCharged(FCooldownId id) const noexcept {
    const FSlot* s = Resolve(id);
    return s != nullptr && s->charged;
}

f32 FCooldownTimer::Remaining(FCooldownId id) const noexcept {
    const FSlot* s = Resolve(id);
    if (s == nullptr) return 0.0f;
    if (s->charged) return 0.0f;
    return s->remaining > 0.0f ? s->remaining : 0.0f;
}

f32 FCooldownTimer::Progress(FCooldownId id) const noexcept {
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

void FCooldownTimer::ForceReady(FCooldownId id) noexcept {
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

void FCooldownTimer::Reset(FCooldownId id) noexcept {
    FSlot* s = Resolve(id);
    if (s == nullptr) return;
    s->remaining = s->duration;
    s->charged   = false;
}

void FCooldownTimer::SetDuration(FCooldownId id, f32 new_duration_sec) noexcept {
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

void FCooldownTimer::ClearAll() noexcept {
    const usize n = _slots.Size();
    for (usize i = 0; i < n; ++i) {
        FSlot& s = _slots[i];
        if (!s.active) continue;
        s.active    = false;
        s.label     = nullptr;
        s.duration  = 0.0f;
        s.remaining = 0.0f;
        s.charged   = false;
    }
    _active_count = 0u;
}

void FCooldownTimer::Tick(f32 dt) noexcept {
    if (dt <= 0.0f) return;
    if (_active_count == 0u) return;

    // Tick 中に callback が Register / Unregister / ClearAll を呼んでも安全な
    // ように index アクセスで回す。新規追加された slot は今回の Tick で進行
    // させない (snapshot_size でストップ、登録時の remaining=duration で次 Tick まで保持)。
    const usize snapshot_size = _slots.Size();

    for (usize i = 0; i < snapshot_size; ++i) {
        FSlot& s = _slots[i];
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
