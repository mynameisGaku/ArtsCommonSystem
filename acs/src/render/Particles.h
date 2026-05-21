// SPDX-License-Identifier: Apache-2.0
// 2D パーティクルシステム（CPU プール + SpriteBatch 描画）
//
// 用途: 火花・煙・爆発・魔法効果など、ゲームの視覚エフェクト全般。
//
// 使い方:
//   ParticleSystem ps;
//   ps.Init(2048);                           // 最大パーティクル数
//   ps.SetTexture(my_circle_tex);            // null なら 1×1 白
//
//   EmitterDesc d;
//   d.position = Vec2{400, 300};
//   d.velocity = Vec2{0, -120};
//   d.velocity_variance = Vec2{60, 30};
//   d.gravity = Vec2{0, 200};
//   d.size_start = 32; d.size_end = 0;
//   d.color_start = Vec4{1, 0.7f, 0.2f, 1};
//   d.color_end   = Vec4{1, 0.1f, 0.0f, 0};
//   d.life_seconds = 1.0f;
//   d.rate_per_sec = 200;
//   ps.SetEmitter(d);
//
//   // 毎フレーム
//   ps.Update(dt);
//   sb.Begin(*cl, sw, sh);
//   ps.Render(sb);                           // バッチ内に追加描画
//   sb.End();
//
// プリセット:
//   ps.SetEmitter(EmitterDesc::Fire(pos));
//   ps.SetEmitter(EmitterDesc::Sparks(pos));
//   ps.SetEmitter(EmitterDesc::Fountain(pos));
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "math/Vec.h"

namespace acs {

class IRhiTexture;
class SpriteBatch;

// エミッタのパラメータ（粒子の初期分布を決める）
struct EmitterDesc {
    Vec2  position           = Vec2{0, 0};
    Vec2  spawn_offset_min   = Vec2{0, 0};       // 出現範囲（min..max）
    Vec2  spawn_offset_max   = Vec2{0, 0};
    Vec2  velocity           = Vec2{0, -100};
    Vec2  velocity_variance  = Vec2{30, 30};
    Vec2  gravity            = Vec2{0, 0};
    f32   size_start         = 16.0f;
    f32   size_end           = 0.0f;
    Vec4  color_start        = Vec4{1, 1, 1, 1};
    Vec4  color_end          = Vec4{1, 1, 1, 0};
    f32   life_seconds       = 1.0f;
    f32   life_variance      = 0.2f;            // ±このぶんランダム
    f32   rate_per_sec       = 60.0f;
    f32   rotation_speed     = 0.0f;            // 描画には未使用（v2 拡張用）
    bool  active             = true;             // false で生成停止

    // プリセット ----------------------------------------------------------
    static EmitterDesc Fire(Vec2 pos)     noexcept;
    static EmitterDesc Sparks(Vec2 pos)   noexcept;
    static EmitterDesc Fountain(Vec2 pos) noexcept;
    static EmitterDesc Smoke(Vec2 pos)    noexcept;
};

class ParticleSystem {
public:
    ParticleSystem() noexcept = default;
    ~ParticleSystem() noexcept;

    ParticleSystem(const ParticleSystem&)            = delete;
    ParticleSystem& operator=(const ParticleSystem&) = delete;

    // max_particles ぶんのプールを確保。テクスチャは後で SetTexture で。
    Result<void> Init(u32 max_particles = 4096) noexcept;
    void Shutdown() noexcept;

    // null 渡しなら DrawRect 相当の白矩形（SpriteBatch の内部白テクスチャ）
    void SetTexture(IRhiTexture* tex) noexcept { _tex = tex; }

    // エミッタ設定。中の active=true で連続生成、false で停止のみ。
    void SetEmitter(const EmitterDesc& d) noexcept { _emitter = d; }
    EmitterDesc& Emitter() noexcept { return _emitter; }
    const EmitterDesc& Emitter() const noexcept { return _emitter; }

    // 1 フレーム分シミュレーション
    void Update(f32 dt) noexcept;

    // 即座に count 個生成（爆発等）
    void EmitBurst(u32 count) noexcept;

    // 全粒子を即座に消滅
    void Reset() noexcept { _active = 0; _spawn_accum = 0; }

    // SpriteBatch に粒子を積む（事前に Begin 済みであること）
    void Render(SpriteBatch& sb) noexcept;

    u32  ActiveCount() const noexcept { return _active; }
    u32  Capacity()    const noexcept { return _capacity; }

private:
    struct Particle {
        Vec2 pos;
        Vec2 vel;
        f32  age;
        f32  life;
        f32  size_start, size_end;
        Vec4 color_start, color_end;
    };

    void SpawnOne() noexcept;
    f32  RandF() noexcept;       // 0..1
    f32  RandRange(f32 a, f32 b) noexcept;

    Particle*    _pool         = nullptr;
    u32          _capacity     = 0;
    u32          _active       = 0;
    f32          _spawn_accum  = 0.0f;
    EmitterDesc  _emitter;
    IRhiTexture* _tex          = nullptr;
    u32          _seed         = 0xC0FFEEu;
};

} // namespace acs
