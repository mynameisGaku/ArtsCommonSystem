// SPDX-License-Identifier: Apache-2.0
#include "asset/SkinnedMesh.h"
#include "test/Expect.h"
#include "test/Test.h"

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

} // namespace

ACS_TEST(AnimationPlayerUpdateSafety, PublicContractAndLayoutRemainStable)
{
    static_assert(std::is_same_v<decltype(&CAnimationPlayer::Play), void (CAnimationPlayer::*)(u32, bool) noexcept>);
    static_assert(std::is_same_v<decltype(&CAnimationPlayer::Update), void (CAnimationPlayer::*)(f32) noexcept>);
    static_assert(std::is_same_v<decltype(&CAnimationPlayer::WritePalette), u32 (CAnimationPlayer::*)(FMat4*, u32) const noexcept>);
    static_assert(sizeof(CAnimationPlayer) == 24u);
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
    player.Update(0.25f);
    EXPECT_TRUE(std::isnan(player.Time()));

    player.SetTime(infinity);
    player.Update(0.25f);
    EXPECT_TRUE(std::isinf(player.Time()));
    EXPECT_FALSE(std::signbit(player.Time()));

    player.SetTime(-infinity);
    player.Update(0.25f);
    EXPECT_TRUE(std::isinf(player.Time()));
    EXPECT_TRUE(std::signbit(player.Time()));
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
