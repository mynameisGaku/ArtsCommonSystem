// SPDX-License-Identifier: Apache-2.0
// 2D パーティクルシステム（CPU プール + FSpriteBatch 描画）
//
// 用途: 火花・煙・爆発・魔法効果など、ゲームの視覚エフェクト全般。
//
// 使い方:
//   FParticleSystem ps;
//   ps.Init(2048);                           // 最大パーティクル数
//   ps.SetTexture(my_circle_tex);            // null なら 1×1 白
//
//   FEmitterDesc d;
//   d.position = FVec2{400, 300};
//   d.velocity = FVec2{0, -120};
//   d.velocity_variance = FVec2{60, 30};
//   d.gravity = FVec2{0, 200};
//   d.size_start = 32; d.size_end = 0;
//   d.color_start = FVec4{1, 0.7f, 0.2f, 1};
//   d.color_end   = FVec4{1, 0.1f, 0.0f, 0};
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
//   ps.SetEmitter(FEmitterDesc::Fire(pos));
//   ps.SetEmitter(FEmitterDesc::Sparks(pos));
//   ps.SetEmitter(FEmitterDesc::Fountain(pos));
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "math/Vec.h"

namespace acs {

class IRhiTexture;
class FSpriteBatch;

// エミッタのパラメータ（粒子の初期分布を決める）
struct FEmitterDesc {
    FVec2  position           = FVec2{0, 0};
    FVec2  spawn_offset_min   = FVec2{0, 0};       // 出現範囲（min..max）
    FVec2  spawn_offset_max   = FVec2{0, 0};
    FVec2  velocity           = FVec2{0, -100};
    FVec2  velocity_variance  = FVec2{30, 30};
    FVec2  gravity            = FVec2{0, 0};
    f32   size_start         = 16.0f;
    f32   size_end           = 0.0f;
    FVec4  color_start        = FVec4{1, 1, 1, 1};
    FVec4  color_end          = FVec4{1, 1, 1, 0};
    f32   life_seconds       = 1.0f;
    f32   life_variance      = 0.2f;            // ±このぶんランダム
    f32   rate_per_sec       = 60.0f;
    f32   rotation_speed     = 0.0f;            // 描画には未使用（v2 拡張用）
    bool  active             = true;             // false で生成停止

    // プリセット ----------------------------------------------------------
    static FEmitterDesc Fire(FVec2 pos)     noexcept;
    static FEmitterDesc Sparks(FVec2 pos)   noexcept;
    static FEmitterDesc Fountain(FVec2 pos) noexcept;
    static FEmitterDesc Smoke(FVec2 pos)    noexcept;
};

class FParticleSystem {
public:
    FParticleSystem() noexcept = default;
    ~FParticleSystem() noexcept;

    FParticleSystem(const FParticleSystem&)            = delete;
    FParticleSystem& operator=(const FParticleSystem&) = delete;

    // max_particles ぶんのプールを確保。テクスチャは後で SetTexture で。
    TResult<void> Init(u32 max_particles = 4096) noexcept;
    void Shutdown() noexcept;

    // null 渡しなら DrawRect 相当の白矩形（FSpriteBatch の内部白テクスチャ）
    void SetTexture(IRhiTexture* tex) noexcept { _tex = tex; }

    // エミッタ設定。中の active=true で連続生成、false で停止のみ。
    void SetEmitter(const FEmitterDesc& d) noexcept { _emitter = d; }
    FEmitterDesc& FEmitter() noexcept { return _emitter; }
    const FEmitterDesc& FEmitter() const noexcept { return _emitter; }

    // 1 フレーム分シミュレーション
    void Update(f32 dt) noexcept;

    // 即座に count 個生成（爆発等）
    void EmitBurst(u32 count) noexcept;

    // 全粒子を即座に消滅
    void Reset() noexcept { _active = 0; _spawn_accum = 0; }

    // FSpriteBatch に粒子を積む（事前に Begin 済みであること）
    void Render(FSpriteBatch& sb) noexcept;

    u32  ActiveCount() const noexcept { return _active; }
    u32  Capacity()    const noexcept { return _capacity; }

private:
    struct FParticle {
        FVec2 pos;
        FVec2 vel;
        f32  age;
        f32  life;
        f32  size_start, size_end;
        FVec4 color_start, color_end;
    };

    void SpawnOne() noexcept;
    f32  RandF() noexcept;       // 0..1
    f32  RandRange(f32 a, f32 b) noexcept;

    FParticle*    _pool         = nullptr;
    u32          _capacity     = 0;
    u32          _active       = 0;
    f32          _spawn_accum  = 0.0f;
    FEmitterDesc  _emitter;
    IRhiTexture* _tex          = nullptr;
    u32          _seed         = 0xC0FFEEu;
};

} // namespace acs
