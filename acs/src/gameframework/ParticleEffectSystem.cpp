// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar I Phase 2 — ParticleEffectSystem 実装
//
// 仕様の意図は ParticleEffectSystem.h を参照。本ファイルでは:
//   ・generational emitter slot の Acquire / Release
//   ・particle pool の `_next_free` カーソル走査
//   ・accumulator 方式の連続放出
//   ・LCG 乱数 + 円周一様な方向ベクトル
//   ・Tick での物理積分 (Euler) + 寿命チェック
// を実装する。すべて noexcept、STL 不使用。
#include "gameframework/ParticleEffectSystem.h"

#include "math/Math.h"   // Sin / Cos / kTwoPi

namespace acs::game {

// =============================================================================
// Init
// -----------------------------------------------------------------------------
// pool 容量を確定し、particle / active flag 配列を default 構築する。
// 二度目以降の呼び出しは no-op (固定容量ポリシー: 走行中の resize は禁止)。
// max_particles == 0 は誤呼出しと見なし 1024 を採用 (silently fail を避ける)。
// =============================================================================
void ParticleEffectSystem::Init(u32 max_particles) noexcept {
    if (_capacity != 0u) return;                  // 既に初期化済み
    if (max_particles == 0u) max_particles = 1024u;

    _particles.Resize(static_cast<usize>(max_particles));
    _particle_active.Resize(static_cast<usize>(max_particles));
    for (usize i = 0; i < _particle_active.Size(); ++i) {
        _particle_active[i] = 0u;
    }
    _capacity         = max_particles;
    _active_particles = 0u;
    _next_free        = 0u;
}

// =============================================================================
// AcquireEmitterSlot
// -----------------------------------------------------------------------------
// 既存の inactive (in_use==false) slot を線形探索で再利用、無ければ末尾追加。
// 24bit index 上限到達時は kInvalidIdx を返す (= caller 側で invalid handle 化)。
// SceneTimer の AcquireSlot と同じ規約。emitter 数は通常 10〜100 程度なので
// 線形探索で十分。
// =============================================================================
u32 ParticleEffectSystem::AcquireEmitterSlot() noexcept {
    const usize n = _emitters.Size();
    for (usize i = 0; i < n; ++i) {
        if (!_emitters[i].in_use) {
            return static_cast<u32>(i);
        }
    }
    if (n >= static_cast<usize>(EmitterHandle::kMaxIndex)) {
        return kInvalidIdx;
    }
    _emitters.PushBack({});
    return static_cast<u32>(_emitters.Size()) - 1u;
}

// =============================================================================
// CreateEmitter
// -----------------------------------------------------------------------------
// 1) lifetime_sec <= 0 の def は無効 (放出しても直ちに死ぬので意味が無い)。
// 2) slot を確保し、def をコピーして active 化。
// 3) gen を 1 進める (0 にラップしたら 1 に戻す。0 は IsValid==false なので
//    handle として配ってはいけない)。
// =============================================================================
EmitterHandle ParticleEffectSystem::CreateEmitter(const ParticleEmitterDef& def, FVec2 pos) noexcept {
    if (def.lifetime_sec <= 0.0f) return {};

    const u32 idx = AcquireEmitterSlot();
    if (idx == kInvalidIdx) return {};

    Emitter& e = _emitters[static_cast<usize>(idx)];

    u8 new_gen = static_cast<u8>(e.gen + 1u);
    if (new_gen == 0u) new_gen = 1u;

    e.def        = def;
    e.pos        = pos;
    e.emit_accum = 0.0f;
    e.gen        = new_gen;
    e.active     = true;
    e.in_use     = true;

    ++_emitter_count;
    return EmitterHandle::Pack(idx, new_gen);
}

// =============================================================================
// SetEmitterPosition / SetEmitterActive
// -----------------------------------------------------------------------------
// invalid / 範囲外 / gen mismatch / in_use==false は no-op。
// gen 一致のチェックで「DestroyEmitter 後に同 slot が別 emitter に化けても
// 古い handle で誤上書きしない」ことを保証する。
// =============================================================================
void ParticleEffectSystem::SetEmitterPosition(EmitterHandle h, FVec2 pos) noexcept {
    if (!h.IsValid()) return;
    const u32 idx = h.Index();
    if (idx >= _emitters.Size()) return;
    Emitter& e = _emitters[static_cast<usize>(idx)];
    if (!e.in_use || e.gen != h.Gen()) return;
    e.pos = pos;
}

void ParticleEffectSystem::SetEmitterActive(EmitterHandle h, bool active) noexcept {
    if (!h.IsValid()) return;
    const u32 idx = h.Index();
    if (idx >= _emitters.Size()) return;
    Emitter& e = _emitters[static_cast<usize>(idx)];
    if (!e.in_use || e.gen != h.Gen()) return;
    e.active     = active;
    // 非 active → active で「emit_accum が大きく残っていて急に大量放出」を
    // 避けるため、active toggling 時は累積をリセットする (UX の素直さ優先)。
    if (!active) {
        e.emit_accum = 0.0f;
    }
}

// =============================================================================
// Burst
// -----------------------------------------------------------------------------
// burst_count を切り捨てで整数化し、その個数を一気に放出。pool 空き数で
// 自然にクランプ (EmitOne 側で満杯時は no-op)。emitter が in_use なら active
// に関わらず発火 (= 「死亡時の最後の一吹き」を非 active 状態でも使えるように)。
// =============================================================================
void ParticleEffectSystem::Burst(EmitterHandle h) noexcept {
    if (!h.IsValid()) return;
    const u32 idx = h.Index();
    if (idx >= _emitters.Size()) return;
    const Emitter& e = _emitters[static_cast<usize>(idx)];
    if (!e.in_use || e.gen != h.Gen()) return;
    if (e.def.burst_count <= 0.0f) return;

    // floor で切り捨て。負値は上で弾いている。
    const u32 n = static_cast<u32>(Floor(e.def.burst_count));
    for (u32 i = 0; i < n; ++i) {
        EmitOne(e);
    }
}

// =============================================================================
// DestroyEmitter
// -----------------------------------------------------------------------------
// emitter slot を空けるだけ。既存の particle はそのまま (寿命まで生きる)。
// gen は維持する (次の AcquireEmitterSlot で +1 して払い出す)。
// =============================================================================
void ParticleEffectSystem::DestroyEmitter(EmitterHandle h) noexcept {
    if (!h.IsValid()) return;
    const u32 idx = h.Index();
    if (idx >= _emitters.Size()) return;
    Emitter& e = _emitters[static_cast<usize>(idx)];
    if (!e.in_use || e.gen != h.Gen()) return;

    e.in_use     = false;
    e.active     = false;
    e.emit_accum = 0.0f;
    if (_emitter_count > 0u) --_emitter_count;
}

// =============================================================================
// AcquireParticleSlot
// -----------------------------------------------------------------------------
// `_next_free` を起点に巡回探索 (modulo capacity)。
//   ・大半のフレームでは 1〜2 step で空きが見つかる (= O(1) 期待)。
//   ・最悪 O(N) だが、_active_particles >= _capacity の早期 return で
//      満杯時は即時 fail する。
//   ・found 時に _next_free を「次の slot」に進めておくと、次回呼出が
//      連続して O(1) になりやすい (= ring buffer 的局所性)。
// =============================================================================
u32 ParticleEffectSystem::AcquireParticleSlot() noexcept {
    if (_capacity == 0u) return kInvalidIdx;
    if (_active_particles >= _capacity) return kInvalidIdx;

    u32 cursor = _next_free;
    for (u32 step = 0u; step < _capacity; ++step) {
        if (cursor >= _capacity) cursor = 0u; // wrap
        if (_particle_active[static_cast<usize>(cursor)] == 0u) {
            // 見つかった
            _particle_active[static_cast<usize>(cursor)] = 1u;
            ++_active_particles;
            // 次回ヒントを 1 進めておく (modulo は次回 wrap で吸収される)
            _next_free = cursor + 1u;
            if (_next_free >= _capacity) _next_free = 0u;
            return cursor;
        }
        ++cursor;
    }
    return kInvalidIdx; // 理論上ここには来ない (上で active < capacity を確認済)
}

// =============================================================================
// EmitOne
// -----------------------------------------------------------------------------
// emitter の def に従って particle を 1 個出生させる:
//   ・方向: [0, 2π) の角度から sin/cos で単位ベクトルを作る (円周一様)。
//   ・速度: [speed_min, speed_max] の一様乱数 (両端 inclusive)。
//   ・position: emitter の pos そのまま (volume / shape 拡張は Phase 3+)。
//   ・age=0、lifetime / color / scale は def をコピー。
// pool 満杯時は何もしない (= 見た目の劣化を許容、フレームを潰さない)。
// =============================================================================
void ParticleEffectSystem::EmitOne(const Emitter& e) noexcept {
    const u32 idx = AcquireParticleSlot();
    if (idx == kInvalidIdx) return;

    Particle& p = _particles[static_cast<usize>(idx)];

    // 方向: 単位円周上の一様サンプル
    const f32 angle = NextRandRange(0.0f, kTwoPi);
    const f32 dx    = Cos(angle);
    const f32 dy    = Sin(angle);

    // 速度の大きさ
    const f32 speed = NextRandRange(e.def.speed_min, e.def.speed_max);

    p.position    = e.pos;
    p.velocity    = FVec2{ dx * speed, dy * speed };
    p.age         = 0.0f;
    p.lifetime    = e.def.lifetime_sec;
    p.color_start = e.def.color_start;
    p.color_end   = e.def.color_end;
    p.scale_start = e.def.scale_start;
    p.scale_end   = e.def.scale_end;
    // gravity を particle 自身に持たせる: emitter 破棄後も物理が正しく動く。
    p.gravity     = e.def.gravity;
}

// =============================================================================
// Tick
// -----------------------------------------------------------------------------
// 1) 各 emitter で連続放出 (active && emit_rate > 0)。accumulator 方式:
//    `emit_accum += rate * dt`、整数部分を 1 個ずつ EmitOne。
// 2) 全 particle を物理積分: pos += vel*dt、vel += gravity*dt (semi-implicit
//    Euler)。これにより gravity 影響下でも数値発散しにくい。
// 3) age += dt し、lifetime を超えたら active=0 に戻して pool に返却。
//
// dt <= 0 は no-op。Init 前 (_capacity == 0) も no-op (defensive)。
// =============================================================================
void ParticleEffectSystem::Tick(f32 dt) noexcept {
    if (dt <= 0.0f) return;
    if (_capacity == 0u) return;

    // ---- 連続放出 ----------------------------------------------------------
    const usize en = _emitters.Size();
    for (usize i = 0; i < en; ++i) {
        Emitter& e = _emitters[i];
        if (!e.in_use || !e.active) continue;
        if (e.def.emit_rate_per_sec <= 0.0f) continue;

        e.emit_accum += e.def.emit_rate_per_sec * dt;
        // 整数部だけ放出。極端な大 dt のときは pool 空き数で自然にクランプされる
        // (EmitOne 内 AcquireParticleSlot が失敗するため emit_accum は減らない =
        // 次フレに繰り越し)。
        while (e.emit_accum >= 1.0f) {
            // 満杯時は early break して残累積はキープ (次フレ放出のチャンスへ)
            const u32 free_slots = (_capacity > _active_particles)
                                 ? (_capacity - _active_particles)
                                 : 0u;
            if (free_slots == 0u) break;

            EmitOne(e);
            e.emit_accum -= 1.0f;
        }
    }

    // ---- 物理積分 + 寿命チェック -----------------------------------------
    // semi-implicit Euler: velocity を gravity で先に更新 → position を更新。
    // 寿命切れは _particle_active[i] を 0 に戻す (slot を pool に返却) し、
    // _next_free を「今解放した index」まで戻して次の EmitOne の局所性を高める。
    const usize pn = _particles.Size();
    for (usize i = 0; i < pn; ++i) {
        if (_particle_active[i] == 0u) continue;

        Particle& p = _particles[i];

        // semi-implicit Euler: v <- v + g*dt, p <- p + v*dt
        p.velocity.x += p.gravity.x * dt;
        p.velocity.y += p.gravity.y * dt;
        p.position.x += p.velocity.x * dt;
        p.position.y += p.velocity.y * dt;

        // 寿命進行
        p.age += dt;
        if (p.age >= p.lifetime) {
            _particle_active[i] = 0u;
            if (_active_particles > 0u) --_active_particles;
            // 今解放した slot を次の EmitOne で即座に使いやすくする
            // (ring buffer ヒントを巻き戻し)。
            if (static_cast<u32>(i) < _next_free) {
                _next_free = static_cast<u32>(i);
            }
        }
    }
}

// =============================================================================
// AllParticles / ClearAll
// =============================================================================
const Particle* ParticleEffectSystem::AllParticles(u32& out_count) const noexcept {
    out_count = _capacity;
    if (_capacity == 0u) return nullptr;
    return _particles.Data();
}

void ParticleEffectSystem::ClearAll() noexcept {
    // emitter slot を全て解放 (gen はそのまま残す = stale handle 検出を維持)
    const usize en = _emitters.Size();
    for (usize i = 0; i < en; ++i) {
        Emitter& e = _emitters[i];
        e.in_use     = false;
        e.active     = false;
        e.emit_accum = 0.0f;
    }
    _emitter_count = 0u;

    // particle pool を全 inactive 化 (中身の値は気にしない、active flag で gate)
    for (usize i = 0; i < _particle_active.Size(); ++i) {
        _particle_active[i] = 0u;
    }
    _active_particles = 0u;
    _next_free        = 0u;
}

// =============================================================================
// NextRandUnit / NextRandRange
// -----------------------------------------------------------------------------
// Numerical Recipes 由来の LCG (a=1664525, c=1013904223, m=2^32)。
// 統計的品質は決して高くないが、particle 用途 (見た目のばらけ) には十分。
// 上位 24bit を仮数に詰めて [0,1) f32 を作る (xoshiro と同じ手口、bias 最小)。
// =============================================================================
f32 ParticleEffectSystem::NextRandUnit() noexcept {
    _rng_state = _rng_state * 1664525u + 1013904223u;
    // 上位 24bit を [0,1) に。0x1p-24 = 1/16777216。
    const u32 bits = _rng_state >> 8;
    return static_cast<f32>(bits) * (1.0f / 16777216.0f);
}

f32 ParticleEffectSystem::NextRandRange(f32 min, f32 max) noexcept {
    if (max < min) {
        const f32 t = min;
        min = max;
        max = t;
    }
    if (min == max) return min; // 退化ケースを早期 return
    return min + (max - min) * NextRandUnit();
}

} // namespace acs::game
