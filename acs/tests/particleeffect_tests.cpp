// SPDX-License-Identifier: Apache-2.0
// FParticleEffectSystem (Pillar I) の動作確認テスト — 特に Burst の巨大値クランプ
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/ParticleEffectSystem.h"

using namespace acs;
using namespace acs::game;

ACS_TEST(ParticleEffect, BurstEmitsAndPoolClamps) {
    FParticleEffectSystem ps;
    ps.Init(64);

    ParticleEmitterDef def;
    def.lifetime_sec = 1.0f;
    def.burst_count  = 10.0f;
    const FEmitterHandle h = ps.CreateEmitter(def, {0, 0});
    EXPECT_TRUE(h.IsValid());

    ps.Burst(h);
    EXPECT_EQ(ps.ActiveParticleCount(), 10u);

    // 寿命経過で pool へ返却される。
    ps.Tick(2.0f);
    EXPECT_EQ(ps.ActiveParticleCount(), 0u);
}

ACS_TEST(ParticleEffect, HugeBurstCountIsClampedToCapacity) {
    FParticleEffectSystem ps;
    ps.Init(64);

    ParticleEmitterDef def;
    def.lifetime_sec = 1.0f;
    // u32 範囲外の f32。修正前は f32→u32 の範囲外 cast (UB) + 数十億回の
    // 空ループでストールしていた。修正後は容量にクランプされ即完走する。
    def.burst_count  = 1e10f;
    const FEmitterHandle h = ps.CreateEmitter(def, {0, 0});
    EXPECT_TRUE(h.IsValid());

    ps.Burst(h);
    EXPECT_EQ(ps.ActiveParticleCount(), 64u);   // pool 容量ぴったりで停止
}
