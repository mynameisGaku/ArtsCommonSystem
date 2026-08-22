// SPDX-License-Identifier: Apache-2.0
#include "asset/SkinnedMesh.h"
#include "test/Expect.h"
#include "test/Test.h"
#include "memory/Memory.h"

#include <cmath>
#include <limits>
#include <type_traits>

using namespace acs;

namespace {

/** 単一animationを持つmeshへplayerを接続し、指定loop状態で再生を始める。 */
bool PreparePlayer(ASkinnedMeshAsset& mesh, CAnimationPlayer& player, f32 duration, bool loop) noexcept
{
    /** テスト対象が参照する単一animation。 */
    FAnimation* const animation = mesh.Animations().TryEmplace();
    if (!animation) return false;
    animation->duration = duration;
    player.SetMesh(&mesh);
    player.Play(0u, loop);
    return true;
}

/** IEEE-754 binary32のbit列からf32をaliasing安全に復元する。 */
f32 FloatFromBits(u32 bits) noexcept
{
    /** 復元するf32値。 */
    f32 value = 0.0f;
    MemCopy(&value, &bits, sizeof(value));
    return value;
}

} // namespace

ACS_TEST(AnimationPlayerUpdateSafety, PublicContractAndTransitionLayoutRemainBounded)
{
    static_assert(std::is_same_v<decltype(&CAnimationPlayer::Play), void (CAnimationPlayer::*)(u32, bool) noexcept>);
    static_assert(std::is_same_v<decltype(&CAnimationPlayer::BlendTo), bool (CAnimationPlayer::*)(u32, f32, bool) noexcept>);
    static_assert(std::is_same_v<decltype(&CAnimationPlayer::Update), void (CAnimationPlayer::*)(f32) noexcept>);
    static_assert(std::is_same_v<decltype(&CAnimationPlayer::WritePalette), u32 (CAnimationPlayer::*)(FMat4*, u32) const noexcept>);
    static_assert(sizeof(CAnimationPlayer) == 32u);
    static_assert(alignof(CAnimationPlayer) == 8u);
    EXPECT_TRUE(true);
}

ACS_TEST(AnimationPlayerUpdateSafety, FiniteLoopWrapsLargeDeltasWithoutIteration)
{
    ASkinnedMeshAsset mesh;
    CAnimationPlayer player;
    const bool prepared = PreparePlayer(mesh, player, std::numeric_limits<f32>::max(), true);
    EXPECT_TRUE(prepared);
    if (!prepared) return;

    /** f32加算では無限大になる正方向の入力。 */
    const f32 maximum = std::numeric_limits<f32>::max();
    player.SetTime(maximum);
    player.Update(maximum);
    EXPECT_NEAR(player.Time(), 0.0f, 0.0f);
    EXPECT_FALSE(std::signbit(player.Time()));
    EXPECT_TRUE(player.IsPlaying());

    /** f32加算では負の無限大になる逆方向の入力。 */
    player.SetTime(-maximum);
    player.Update(-maximum);
    EXPECT_NEAR(player.Time(), 0.0f, 0.0f);
    EXPECT_FALSE(std::signbit(player.Time()));
    EXPECT_TRUE(player.IsPlaying());
}

ACS_TEST(AnimationPlayerUpdateSafety, FiniteLoopPreservesPositiveAndNegativeWrap)
{
    ASkinnedMeshAsset mesh;
    CAnimationPlayer player;
    const bool prepared = PreparePlayer(mesh, player, 2.0f, true);
    EXPECT_TRUE(prepared);
    if (!prepared) return;

    player.SetTime(0.25f);
    player.Update(5.5f);
    EXPECT_NEAR(player.Time(), 1.75f, 1.0e-6f);

    player.SetTime(0.25f);
    player.Update(-0.5f);
    EXPECT_NEAR(player.Time(), 1.75f, 1.0e-6f);
    EXPECT_TRUE(player.IsPlaying());
}

ACS_TEST(AnimationPlayerUpdateSafety, FiniteLoopRoundsNarrowedEndpointToPositiveZero)
{
    ASkinnedMeshAsset mesh;
    CAnimationPlayer player;
    /** f32加算がちょうどdurationへ丸まる再現用duration。 */
    const f32 duration = FloatFromBits(0x3F800000u);
    const bool prepared = PreparePlayer(mesh, player, duration, true);
    EXPECT_TRUE(prepared);
    if (!prepared) return;

    /** 独立reviewで見つかった時刻のbit列。 */
    const f32 starting_time = FloatFromBits(0x3B449BA6u);
    /** 独立reviewで見つかった差分時刻のbit列。 */
    const f32 delta_time = FloatFromBits(0x3F7F3B64u);
    EXPECT_EQ(static_cast<f32>(starting_time + delta_time), duration);

    player.SetTime(starting_time);
    player.Update(delta_time);
    EXPECT_EQ(player.Time(), 0.0f);
    EXPECT_FALSE(std::signbit(player.Time()));
    EXPECT_TRUE(player.Time() >= 0.0f);
    EXPECT_TRUE(player.Time() < duration);
}

ACS_TEST(AnimationPlayerUpdateSafety, FiniteLoopKeepsPositiveSubnormalDurationInRange)
{
    ASkinnedMeshAsset mesh;
    CAnimationPlayer player;
    /** 最小の正subnormal duration。 */
    const f32 duration = FloatFromBits(0x00000001u);
    const bool prepared = PreparePlayer(mesh, player, duration, true);
    EXPECT_TRUE(prepared);
    if (!prepared) return;

    EXPECT_EQ(std::fpclassify(duration), FP_SUBNORMAL);
    player.SetTime(duration);
    player.Update(duration);
    EXPECT_EQ(player.Time(), 0.0f);
    EXPECT_FALSE(std::signbit(player.Time()));
    EXPECT_TRUE(player.Time() >= 0.0f);
    EXPECT_TRUE(player.Time() < duration);
}

ACS_TEST(AnimationPlayerUpdateSafety, InvalidFiniteLoopInputsPreserveOldState)
{
    ASkinnedMeshAsset mesh;
    CAnimationPlayer player;
    const bool prepared = PreparePlayer(mesh, player, 2.0f, true);
    EXPECT_TRUE(prepared);
    if (!prepared) return;

    /** 入力異常時に保持される既存時刻。 */
    constexpr f32 sentinel_time = 0.75f;

    /** 実行時にも保持される非数。 */
    const f32 not_a_number = std::numeric_limits<f32>::quiet_NaN();

    /** 正負の無限大。 */
    const f32 infinity = std::numeric_limits<f32>::infinity();

    player.SetTime(sentinel_time);
    player.Update(not_a_number);
    EXPECT_NEAR(player.Time(), sentinel_time, 0.0f);

    player.Update(infinity);
    EXPECT_NEAR(player.Time(), sentinel_time, 0.0f);

    player.Update(-infinity);
    EXPECT_NEAR(player.Time(), sentinel_time, 0.0f);

    player.SetTime(not_a_number);
    EXPECT_NEAR(player.Time(), sentinel_time, 0.0f);

    player.SetTime(infinity);
    EXPECT_NEAR(player.Time(), sentinel_time, 0.0f);

    player.SetTime(-infinity);
    EXPECT_NEAR(player.Time(), sentinel_time, 0.0f);
    EXPECT_TRUE(player.IsPlaying());
}

ACS_TEST(AnimationPlayerUpdateSafety, NegativeZeroIsNormalizedAfterLoopWrap)
{
    ASkinnedMeshAsset mesh;
    CAnimationPlayer player;
    const bool prepared = PreparePlayer(mesh, player, 2.0f, true);
    EXPECT_TRUE(prepared);
    if (!prepared) return;

    player.SetTime(0.0f);
    player.Update(-2.0f);
    EXPECT_NEAR(player.Time(), 0.0f, 0.0f);
    EXPECT_FALSE(std::signbit(player.Time()));
}

ACS_TEST(AnimationPlayerUpdateSafety, NonLoopKeepsStrictGreaterThanEndBehavior)
{
    ASkinnedMeshAsset mesh;
    CAnimationPlayer player;
    const bool prepared = PreparePlayer(mesh, player, 2.0f, false);
    EXPECT_TRUE(prepared);
    if (!prepared) return;

    player.Update(2.0f);
    EXPECT_NEAR(player.Time(), 2.0f, 0.0f);
    EXPECT_TRUE(player.IsPlaying());

    player.Update(0.25f);
    EXPECT_NEAR(player.Time(), 2.0f, 0.0f);
    EXPECT_FALSE(player.IsPlaying());
}

ACS_TEST(AnimationPlayerUpdateSafety, NonPositiveDurationsKeepAdditiveBehavior)
{
    {
        ASkinnedMeshAsset mesh;
        CAnimationPlayer player;
        const bool prepared = PreparePlayer(mesh, player, 0.0f, true);
        EXPECT_TRUE(prepared);
        if (!prepared) return;

        player.SetTime(1.0f);
        player.Update(2.0f);
        EXPECT_NEAR(player.Time(), 3.0f, 0.0f);
        EXPECT_TRUE(player.IsPlaying());
    }

    {
        ASkinnedMeshAsset mesh;
        CAnimationPlayer player;
        const bool prepared = PreparePlayer(mesh, player, -2.0f, false);
        EXPECT_TRUE(prepared);
        if (!prepared) return;

        player.SetTime(1.0f);
        player.Update(2.0f);
        EXPECT_NEAR(player.Time(), 3.0f, 0.0f);
        EXPECT_TRUE(player.IsPlaying());
    }
}

ACS_TEST(AnimationPlayerUpdateSafety, NonFiniteDurationsKeepLegacyBehavior)
{
    {
        ASkinnedMeshAsset mesh;
        CAnimationPlayer player;
        /** 比較がfalseとなる非数duration。 */
        const f32 duration = std::numeric_limits<f32>::quiet_NaN();
        const bool prepared = PreparePlayer(mesh, player, duration, true);
        EXPECT_TRUE(prepared);
        if (!prepared) return;

        player.SetTime(1.0f);
        player.Update(2.0f);
        EXPECT_NEAR(player.Time(), 3.0f, 0.0f);
        EXPECT_TRUE(player.IsPlaying());
    }

    {
        ASkinnedMeshAsset mesh;
        CAnimationPlayer player;
        /** 旧loopの負時刻補正を保つ正無限duration。 */
        const f32 duration = std::numeric_limits<f32>::infinity();
        const bool prepared = PreparePlayer(mesh, player, duration, true);
        EXPECT_TRUE(prepared);
        if (!prepared) return;

        player.SetTime(-1.0f);
        player.Update(0.0f);
        EXPECT_TRUE(std::isinf(player.Time()));
        EXPECT_FALSE(std::signbit(player.Time()));
        EXPECT_TRUE(player.IsPlaying());
    }

    {
        ASkinnedMeshAsset mesh;
        CAnimationPlayer player;
        /** 比較がfalseとなる負無限duration。 */
        const f32 duration = -std::numeric_limits<f32>::infinity();
        const bool prepared = PreparePlayer(mesh, player, duration, false);
        EXPECT_TRUE(prepared);
        if (!prepared) return;

        player.SetTime(1.0f);
        player.Update(2.0f);
        EXPECT_NEAR(player.Time(), 3.0f, 0.0f);
        EXPECT_TRUE(player.IsPlaying());
    }
}
