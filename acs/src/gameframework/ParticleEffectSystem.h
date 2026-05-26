// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar I Phase 2 — ParticleEffectSystem (軽量 sprite particle)
//
// 2D ゲーム向けの最小コスト sprite パーティクル。`EffectSystem` (Pillar I Phase 1)
// が「Flash / HitStop / Shake のような単発演出」を担うのに対し、本クラスは
// 「emitter で粒子を放出し続ける」連続演出 (炎・煙・スパーク・吹き出しなど) を担う。
//
// 設計選択:
//   ・**emitter は generational handle**: 24bit index + 8bit gen を packed した
//      `EmitterHandle`。`SceneTimer::TimerHandle` / `TriggerWorld2D::TriggerId` と
//      同じ規約。slot 再利用後の stale 参照は IsValid + gen 一致で検出可能。
//   ・**particle pool は固定容量**: `Init(max_particles)` で確保した後はリサイズ
//      しない。リアルタイムループのフレーム落ちを避けるため、最悪ケースを上限と
//      する保守的なポリシー。空き探索は `next_free` カーソル + 線形走査の
//      ハイブリッド (大半のフレームで O(1)、最悪 O(N))。
//   ・**描画は user 側**: 本クラスは描画 API を一切呼ばず、`AllParticles()` で
//      `const Particle*` を渡すのみ。SpriteBatch / DebugDraw / カスタム pipeline
//      など、ユーザー側の描画戦略に依存しない (= テスト容易・headless 動作可)。
//   ・**Random は内部 LCG**: `acs::game::Random` (xoshiro128**) には依存せず、
//      ParticleEffectSystem は単一の `u32` state を持つ簡素な Linear Congruential
//      Generator で十分。理由は (1) particle 用途は統計的品質要求が弱い (見た目で
//      バラけていれば十分), (2) 外部 PRNG 状態と独立にしておくと determinism を
//      議論しやすい (replay 再現で外部 PRNG が動いても particle 配列は影響しない),
//      (3) ヘッダ依存を最小化できる。Numerical Recipes の LCG (a=1664525,
//      c=1013904223) を採用。
//   ・**lifetime 切れは pool に返却**: particle.age >= particle.lifetime で
//      `_active[i] = 0`、`--_active_particles` で実質的にプールへ戻す。slot の
//      物理 index は変えない (= 描画側がフレームをまたぐ参照を握っている可能性は
//      無いが、内部での swap-and-pop も不要にして単純化)。
//   ・**emit_rate は accumulator 方式**: `_emit_accum += emit_rate * dt` を加算し、
//      整数部分だけ放出する。dt が小さくても累積で放出できるし、大 dt のときも
//      欠落しない。ただし上限は「pool 空き数」でクランプ。
//   ・**Burst は emit_rate と独立**: `Burst()` は burst_count 個を一気に放出。
//      連射やヒット時のフラッシュ用。
//   ・**非コピー・非ムーブ**: TPool 同様、ポインタを外部に渡す API があるため
//      コピーで実体が分裂すると未定義動作になりやすい。明示的に削除。
//   ・**全 noexcept**: ACS 規約。失敗は invalid handle / no-op で表現。
//
// 使い方:
//   ParticleEmitterDef def{};
//   def.color_start      = {1.0f, 0.6f, 0.1f};  // 橙
//   def.color_end        = {0.5f, 0.0f, 0.0f};  // 暗赤
//   def.lifetime_sec     = 0.8f;
//   def.emit_rate_per_sec= 30.0f;
//   def.burst_count      = 12.0f;
//   def.speed_min        = 40.0f;
//   def.speed_max        = 80.0f;
//   def.scale_start      = 8.0f;
//   def.scale_end        = 0.0f;
//   def.gravity          = {0.0f, 60.0f};       // y 下方向加速度
//
//   ParticleEffectSystem fx;
//   fx.Init(2048);
//   auto h = fx.CreateEmitter(def, {200.0f, 300.0f});
//
//   // 毎フレーム:
//   fx.Tick(dt);
//
//   // 描画 (ユーザー側):
//   u32 n = 0;
//   const Particle* p = fx.AllParticles(n);
//   for (u32 i = 0; i < n; ++i) {
//       if (!p[i].IsAlive()) continue;
//       // SpriteBatch.Draw(p[i].position, current_scale, current_color);
//   }
//
//   // 爆発:
//   fx.Burst(h);
//
//   // emitter 破棄 (Scene 退場時など):
//   fx.DestroyEmitter(h);
//
// 範囲外 (Phase 3+ で):
//   ・テクスチャ参照 / アニメーション (sprite sheet)
//   ・回転 / 角速度 / 角減衰
//   ・3D 位置 (現状は 2D FVec2)
//   ・curve-based size/color (現状は線形補間のみ)
//   ・GPU particle (現状は CPU)
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game {

// ---------------------------------------------------------------------------
// ParticleEmitterDef — emitter の挙動パラメータ
// ---------------------------------------------------------------------------
// `lifetime_sec`        : 各 particle の寿命 (秒)。<= 0 は emit を行わない。
// `emit_rate_per_sec`   : 毎秒の放出個数 (連続放出)。0 なら自動放出は無し。
// `burst_count`         : Burst() 1 回で放出する個数 (端数は切り捨て)。
// `speed_min/max`       : 初速度の大きさ範囲。方向は円周一様。
// `scale_start/end`     : ピクセル単位の見た目サイズ (描画側が解釈)。
// `gravity`             : particle ごとの定数加速度 (px/sec^2 想定)。
// `color_start/end`     : 線形補間される RGB (0..1)。alpha は描画側で決める。
struct ParticleEmitterDef {
    FVec3 color_start         {1.0f, 1.0f, 1.0f};
    FVec3 color_end           {1.0f, 1.0f, 1.0f};
    f32  lifetime_sec        = 1.0f;
    f32  emit_rate_per_sec   = 0.0f;
    f32  burst_count         = 0.0f;
    f32  speed_min           = 0.0f;
    f32  speed_max           = 0.0f;
    f32  scale_start         = 1.0f;
    f32  scale_end           = 1.0f;
    FVec2 gravity             {0.0f, 0.0f};
};

// ---------------------------------------------------------------------------
// Particle — 個別粒子の生データ
// ---------------------------------------------------------------------------
// 描画側はこの構造体を直接読み取り、age/lifetime から補間係数を計算する。
// `position` / `velocity` 以外は emitter から複製してきた瞬時パラメータ。
// 「emitter の def が後から変わっても、放出済 particle は出生時の値で進む」
// 設計 (= self-contained particle、TPool で扱いやすい)。
struct Particle {
    FVec2 position           {0.0f, 0.0f};
    FVec2 velocity           {0.0f, 0.0f};
    f32  age                = 0.0f;     // 経過秒、0 で出生
    f32  lifetime           = 0.0f;     // 寿命秒
    FVec3 color_start        {1.0f, 1.0f, 1.0f};
    FVec3 color_end          {1.0f, 1.0f, 1.0f};
    f32  scale_start        = 1.0f;
    f32  scale_end          = 1.0f;
    // 出生時に emitter からコピーする加速度。emitter 破棄後も粒子が自分の
    // gravity を持ち回るので per-particle に物理が正しく動く。
    FVec2 gravity            {0.0f, 0.0f};

    // 残寿命比 (0..1)。lifetime <= 0 のときは 0 を返す (defensive)。
    f32 NormalizedAge() const noexcept {
        if (lifetime <= 0.0f) return 0.0f;
        const f32 t = age / lifetime;
        return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    }
    bool IsAlive() const noexcept { return age < lifetime; }
};

// ---------------------------------------------------------------------------
// EmitterHandle — 24bit index + 8bit gen を packed した opaque handle
// ---------------------------------------------------------------------------
// `_packed == 0` を invalid と定義 (gen は常に 1 以上で配る)。`SceneTimer` 等と
// 同一規約。slot 再利用後の stale 参照は IsValid + 内部の gen 一致で検出する。
struct EmitterHandle {
    u32 _packed = 0u;

    bool IsValid() const noexcept { return _packed != 0u; }

    static constexpr u32 kIndexBits = 24u;
    static constexpr u32 kIndexMask = (1u << kIndexBits) - 1u; // 0x00FFFFFF
    static constexpr u32 kMaxIndex  = kIndexMask;              // 16777215

    static EmitterHandle Pack(u32 index, u8 gen) noexcept {
        EmitterHandle h;
        h._packed = (static_cast<u32>(gen) << kIndexBits) | (index & kIndexMask);
        return h;
    }
    u32 Index() const noexcept { return _packed & kIndexMask; }
    u8  Gen()   const noexcept { return static_cast<u8>(_packed >> kIndexBits); }
};

// ---------------------------------------------------------------------------
// ParticleEffectSystem — 複数 emitter + 共有 particle pool
// ---------------------------------------------------------------------------
class ParticleEffectSystem {
public:
    ParticleEffectSystem() noexcept = default;
    ~ParticleEffectSystem() noexcept = default;

    // 非コピー・非ムーブ: AllParticles() が内部 buffer の生ポインタを返すため、
    // ムーブで実体アドレスが変わると外部参照が破綻する。
    ParticleEffectSystem(const ParticleEffectSystem&)            = delete;
    ParticleEffectSystem& operator=(const ParticleEffectSystem&) = delete;
    ParticleEffectSystem(ParticleEffectSystem&&)                 = delete;
    ParticleEffectSystem& operator=(ParticleEffectSystem&&)      = delete;

    // 初期化。max_particles で pool 上限を確定 (再 Init は no-op)。
    // 0 を渡した場合は default の 1024 を採用 (誤呼出し防御)。
    void Init(u32 max_particles = 1024) noexcept;

    // emitter を 1 個作成して handle を返す。pos は world 座標 (描画側解釈)。
    // emitter slot 上限 (24bit) に達した場合や lifetime_sec <= 0 の def は
    // invalid handle を返す。
    EmitterHandle CreateEmitter(const ParticleEmitterDef& def, FVec2 pos) noexcept;

    // 既存 emitter の位置を変更。invalid / stale handle は no-op。
    void SetEmitterPosition(EmitterHandle h, FVec2 pos) noexcept;

    // 既存 emitter の active 状態を変更 (false にすると自動放出を止める)。
    // invalid / stale handle は no-op。Burst は active と無関係に発火する。
    void SetEmitterActive(EmitterHandle h, bool active) noexcept;

    // burst_count 個一気に放出 (pool 空き数で上限クランプ)。
    // invalid / stale handle は no-op。
    void Burst(EmitterHandle h) noexcept;

    // emitter を破棄 (= slot 解放 + gen 進める)。既存の particle は寿命を
    // まっとうするまで生き続ける (emitter とは独立に管理されているため)。
    void DestroyEmitter(EmitterHandle h) noexcept;

    // dt 秒進める。各 emitter で連続放出 → 全 particle の物理更新 → 寿命
    // 切れを pool に返却。dt <= 0 は no-op。
    void Tick(f32 dt) noexcept;

    // 現在 active な particle 数 (描画予定数)。
    u32 ActiveParticleCount() const noexcept { return _active_particles; }

    // 現在 active な emitter 数 (= 自動放出してくる emitter ではなく、生きてる slot 数)。
    u32 EmitterCount() const noexcept { return _emitter_count; }

    // 描画用 raw buffer。out_count には pool 全体サイズが入る (active かどうかは
    // `Particle::IsAlive()` で判定する)。返却ポインタは Init で確定し、Init or
    // クラス破棄まで安定。Init 前は nullptr / 0 を返す。
    const Particle* AllParticles(u32& out_count) const noexcept;

    // 全 emitter + 全 particle を即座にクリア。pool 容量は維持。
    void ClearAll() noexcept;

private:
    // --- emitter slot ---
    // generational handle + 動作パラメータ + 連続放出のための累積器。
    struct Emitter {
        ParticleEmitterDef def        {};
        FVec2               pos        {0.0f, 0.0f};
        f32                emit_accum = 0.0f;     // 放出累積 (>=1 で 1 個出す)
        u8                 gen        = 0u;       // 0 = 未使用、配るときは 1 以上
        bool               active     = false;    // false で自動放出停止
        bool               in_use     = false;    // slot 自体が予約されているか
    };

    // 1 個放出する。pool が満杯なら何もしない (= 空き上限超過で見た目が崩れる
    // のは許容する。代わりにフレームレートは保つ)。
    void EmitOne(const Emitter& e) noexcept;

    // 空き particle slot を 1 個確保し index を返す。満杯なら kInvalidIdx を返す。
    // `_next_free` を起点に巡回探索 (= 大半のフレームで O(1))。
    static constexpr u32 kInvalidIdx = 0xFFFFFFFFu;
    u32 AcquireParticleSlot() noexcept;

    // emitter slot を 1 個確保 (既存の inactive を再利用、無ければ末尾追加)。
    // 24bit index 上限到達時は kInvalidIdx を返す。
    u32 AcquireEmitterSlot() noexcept;

    // 内部 LCG (Numerical Recipes 定数)。閉区間 [0,1) の f32 を返す。
    f32 NextRandUnit() noexcept;
    // [min, max] の f32 (min > max は swap)。
    f32 NextRandRange(f32 min, f32 max) noexcept;

    TArray<Emitter>  _emitters       {};       // emitter slot 配列 (generational)
    TArray<Particle> _particles      {};       // particle pool (固定容量)
    TArray<u8>       _particle_active{};       // _particle_active[i] = 1 で使用中
    u32             _capacity         = 0u;   // particle 上限 (Init で確定)
    u32             _active_particles = 0u;   // 現在使用中 particle 数
    u32             _emitter_count    = 0u;   // 現在予約中 emitter slot 数
    u32             _next_free        = 0u;   // particle 空き探索のヒントカーソル
    u32             _rng_state        = 0x9E3779B9u; // LCG state (golden ratio で初期化)
};

} // namespace acs::game
