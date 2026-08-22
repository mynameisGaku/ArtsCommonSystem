// SPDX-License-Identifier: Apache-2.0
#include "asset/SkinnedMesh.h"
#include "gameframework/SkinnedMeshComponent3D.h"
#include "math/Math.h"
#include "memory/Memory.h"
#include "test/Expect.h"
#include "test/Test.h"

#include <cmath>
#include <limits>
#include <type_traits>

using namespace acs;
using namespace acs::game;

namespace {

/** 1本のboneと、時間変化する切替元・切替先clipを持つmeshを作る。 */
bool BuildBlendMesh(ASkinnedMeshAsset& mesh) noexcept
{
    /** 単位inverse-bindを持つroot bone。 */
    FBone* const bone = mesh.Bones().TryEmplace();
    if (bone == nullptr) return false;
    bone->name = FString("Root");

    /** rootの子として階層合成を検証するbone。 */
    FBone* const child = mesh.Bones().TryEmplace();
    if (child == nullptr) return false;
    child->name = FString("Child");
    child->parent = 0;

    /** rootとchildに2keyずつ持つclipを追加する局所処理。 */
    auto add_clip = [&mesh](const char* name, FAnimationKey first, FAnimationKey second, FVec3 child_translation) noexcept -> bool {
        FAnimation* const animation = mesh.Animations().TryEmplace();
        if (animation == nullptr) return false;
        animation->name = FString(name);
        animation->duration = 2.0f;
        FAnimationChannel* const channel = animation->channels.TryEmplace();
        if (channel == nullptr) return false;
        channel->bone_index = 0;
        if (!channel->keys.TryAdd(first)) return false;
        if (!channel->keys.TryAdd(second)) return false;

        FAnimationChannel* const child_channel = animation->channels.TryEmplace();
        if (child_channel == nullptr) return false;
        child_channel->bone_index = 1;
        FAnimationKey child_first;
        child_first.time = 0.0f;
        child_first.translation = child_translation;
        FAnimationKey child_second = child_first;
        child_second.time = 1.0f;
        if (!child_channel->keys.TryAdd(child_first)) return false;
        return child_channel->keys.TryAdd(child_second);
    };

    /** 切替元の先頭key。 */
    FAnimationKey source_first;
    source_first.time = 0.0f;
    source_first.translation = FVec3{0.0f, 0.0f, 0.0f};
    source_first.rotation = FQuat{};
    source_first.scale = FVec3{1.0f, 1.0f, 1.0f};

    /** 切替元の1秒key。 */
    FAnimationKey source_second;
    source_second.time = 1.0f;
    source_second.translation = FVec3{2.0f, 0.0f, 0.0f};
    source_second.rotation = FQuat{};
    source_second.scale = FVec3{2.0f, 2.0f, 2.0f};

    /** 切替先で一定となる120度回転。 */
    const FQuat target_rotation = FQuat::AxisAngle(FVec3::UnitZ(), 2.0f * kPi / 3.0f);

    /** 切替先の先頭key。 */
    FAnimationKey target_first;
    target_first.time = 0.0f;
    target_first.translation = FVec3{10.0f, 0.0f, 0.0f};
    target_first.rotation = target_rotation;
    target_first.scale = FVec3{3.0f, 3.0f, 3.0f};

    /** 切替先の1秒key。 */
    FAnimationKey target_second;
    target_second.time = 1.0f;
    target_second.translation = FVec3{14.0f, 0.0f, 0.0f};
    target_second.rotation = target_rotation;
    target_second.scale = FVec3{5.0f, 5.0f, 5.0f};

    return add_clip("Source", source_first, source_second, FVec3{0.0f, 2.0f, 0.0f}) &&
           add_clip("Target", target_first, target_second, FVec3{0.0f, 4.0f, 0.0f});
}

/** 行列16成分が許容誤差内で一致することを検証する。 */
void ExpectMatrixNear(const FMat4& actual, const FMat4& expected, f32 tolerance) noexcept
{
    for (u32 row = 0u; row < 4u; ++row) {
        for (u32 column = 0u; column < 4u; ++column) {
            EXPECT_NEAR(actual.m[row][column], expected.m[row][column], tolerance);
        }
    }
}

/** playerの現在姿勢からroot boneのpalette行列を得る。 */
FMat4 ReadRootPalette(const CAnimationPlayer& player) noexcept
{
    FMat4 palette = FMat4::Identity();
    EXPECT_EQ(player.WritePalette(&palette, 1u), 1u);
    return palette;
}

/** playerの現在姿勢からrootとchildのpalette行列を得る。 */
void ReadTwoBonePalette(const CAnimationPlayer& player, FMat4 (&out_palette)[2]) noexcept
{
    EXPECT_EQ(player.WritePalette(out_palette, 2u), 2u);
}

} // namespace

ACS_TEST(SkinnedMeshAnimationBlend, PublicEntryPointsHaveExpectedTypes)
{
    static_assert(std::is_same_v<decltype(&CAnimationPlayer::BlendTo), bool (CAnimationPlayer::*)(u32, f32, bool) noexcept>);
    static_assert(std::is_same_v<decltype(&ASkinnedMeshComponent3D::BlendTo), bool (ASkinnedMeshComponent3D::*)(u32, f32, bool) noexcept>);
    static_assert(std::is_same_v<decltype(&ASkinnedMeshComponent3D::BlendToByName), bool (ASkinnedMeshComponent3D::*)(FStringView, f32, bool) noexcept>);
    EXPECT_TRUE(true);
}

ACS_TEST(SkinnedMeshAnimationBlend, MidpointBlendsAdvancedLocalTrsBeforeHierarchy)
{
    ASkinnedMeshAsset mesh;
    const bool built = BuildBlendMesh(mesh);
    EXPECT_TRUE(built);
    if (!built) return;

    CAnimationPlayer player;
    player.SetMesh(&mesh);
    player.Play(0u, true);
    player.SetTime(0.25f);
    EXPECT_TRUE(player.BlendTo(1u, 1.0f, true));
    player.Update(0.5f);
    EXPECT_NEAR(player.Time(), 0.5f, 1.0e-6f);

    // sourceは0.75秒、targetは0.5秒へ進み、その両姿勢をalpha=0.5で混ぜる。
    const FMat4 expected_root = FMat4::Scale(FVec3{2.875f, 2.875f, 2.875f}) *
        FMat4::RotationZ(kPi / 3.0f) * FMat4::Translation(FVec3{6.75f, 0.0f, 0.0f});
    const FMat4 expected_child = FMat4::Translation(FVec3{0.0f, 3.0f, 0.0f}) * expected_root;
    FMat4 palette[2];
    ReadTwoBonePalette(player, palette);
    ExpectMatrixNear(palette[0], expected_root, 1.0e-4f);
    ExpectMatrixNear(palette[1], expected_child, 1.0e-4f);
}

ACS_TEST(SkinnedMeshAnimationBlend, CompletionKeepsOnlyAdvancedTargetPose)
{
    ASkinnedMeshAsset mesh;
    const bool built = BuildBlendMesh(mesh);
    EXPECT_TRUE(built);
    if (!built) return;

    CAnimationPlayer player;
    player.SetMesh(&mesh);
    player.Play(0u, true);
    player.SetTime(0.25f);
    EXPECT_TRUE(player.BlendTo(1u, 1.0f, false));
    // blend終端を0.25秒越えるdtでも、余剰時間をtarget clipへ反映する。
    player.Update(1.25f);
    EXPECT_NEAR(player.Time(), 1.25f, 1.0e-6f);

    const FMat4 expected_root = FMat4::Scale(FVec3{5.0f, 5.0f, 5.0f}) *
        FMat4::RotationZ(2.0f * kPi / 3.0f) *
        FMat4::Translation(FVec3{14.0f, 0.0f, 0.0f});
    const FMat4 expected_child = FMat4::Translation(FVec3{0.0f, 4.0f, 0.0f}) * expected_root;
    FMat4 palette[2];
    ReadTwoBonePalette(player, palette);
    ExpectMatrixNear(palette[0], expected_root, 1.0e-4f);
    ExpectMatrixNear(palette[1], expected_child, 1.0e-4f);

    player.Update(0.25f);
    EXPECT_NEAR(player.Time(), 1.5f, 1.0e-6f);
    ReadTwoBonePalette(player, palette);
    ExpectMatrixNear(palette[0], expected_root, 1.0e-4f);
    ExpectMatrixNear(palette[1], expected_child, 1.0e-4f);
}

ACS_TEST(SkinnedMeshAnimationBlend, ActiveTransitionRejectsReplacementWithoutPosePop)
{
    ASkinnedMeshAsset mesh;
    const bool built = BuildBlendMesh(mesh);
    EXPECT_TRUE(built);
    if (!built) return;

    CAnimationPlayer player;
    player.SetMesh(&mesh);
    player.Play(0u, true);
    player.SetTime(0.25f);
    EXPECT_TRUE(player.BlendTo(1u, 1.0f, false));
    player.Update(0.5f);

    u8 state_before[sizeof(CAnimationPlayer)];
    MemCopy(state_before, &player, sizeof(player));
    const f32 time_before = player.Time();
    const bool playing_before = player.IsPlaying();
    FMat4 palette_before[2];
    ReadTwoBonePalette(player, palette_before);

    EXPECT_FALSE(player.BlendTo(0u, 0.25f, true));
    EXPECT_TRUE(MemCmp(state_before, &player, sizeof(player)) == 0);
    EXPECT_NEAR(player.Time(), time_before, 0.0f);
    EXPECT_EQ(player.IsPlaying(), playing_before);

    FMat4 palette_after[2];
    ReadTwoBonePalette(player, palette_after);
    ExpectMatrixNear(palette_after[0], palette_before[0], 0.0f);
    ExpectMatrixNear(palette_after[1], palette_before[1], 0.0f);
}

ACS_TEST(SkinnedMeshAnimationBlend, InvalidInputsAndNonFiniteUpdatesPreserveTransition)
{
    ASkinnedMeshAsset mesh;
    const bool built = BuildBlendMesh(mesh);
    EXPECT_TRUE(built);
    if (!built) return;

    CAnimationPlayer empty;
    EXPECT_FALSE(empty.BlendTo(0u, 0.5f, true));

    CAnimationPlayer player;
    player.SetMesh(&mesh);
    player.Play(0u, true);
    player.SetTime(0.25f);
    const FMat4 original = ReadRootPalette(player);
    EXPECT_FALSE(player.BlendTo(8u, 0.5f, true));
    EXPECT_FALSE(player.BlendTo(1u, -0.1f, true));
    EXPECT_FALSE(player.BlendTo(1u, std::numeric_limits<f32>::quiet_NaN(), true));
    EXPECT_FALSE(player.BlendTo(1u, std::numeric_limits<f32>::infinity(), true));
    EXPECT_NEAR(player.Time(), 0.25f, 0.0f);
    ExpectMatrixNear(ReadRootPalette(player), original, 0.0f);

    player.Update(std::numeric_limits<f32>::quiet_NaN());
    player.Update(std::numeric_limits<f32>::infinity());
    player.SetTime(std::numeric_limits<f32>::quiet_NaN());
    player.SetTime(-std::numeric_limits<f32>::infinity());
    EXPECT_NEAR(player.Time(), 0.25f, 0.0f);
    ExpectMatrixNear(ReadRootPalette(player), original, 0.0f);

    mesh.Animations()[1].duration = std::numeric_limits<f32>::infinity();
    EXPECT_FALSE(player.BlendTo(1u, 0.5f, true));
    EXPECT_NEAR(player.Time(), 0.25f, 0.0f);
    mesh.Animations()[1].duration = 2.0f;

    EXPECT_TRUE(player.BlendTo(1u, 1.0f, true));
    player.Update(0.25f);
    const f32 transition_time = player.Time();
    const FMat4 transition_pose = ReadRootPalette(player);
    player.Update(std::numeric_limits<f32>::quiet_NaN());
    player.Update(std::numeric_limits<f32>::infinity());
    player.SetTime(std::numeric_limits<f32>::quiet_NaN());
    EXPECT_NEAR(player.Time(), transition_time, 0.0f);
    ExpectMatrixNear(ReadRootPalette(player), transition_pose, 0.0f);
}

ACS_TEST(SkinnedMeshAnimationBlend, ZeroDurationAndPlayRemainImmediate)
{
    ASkinnedMeshAsset mesh;
    const bool built = BuildBlendMesh(mesh);
    EXPECT_TRUE(built);
    if (!built) return;

    CAnimationPlayer blended;
    blended.SetMesh(&mesh);
    blended.Play(0u, true);
    EXPECT_TRUE(blended.BlendTo(1u, 0.0f, false));

    CAnimationPlayer played;
    played.SetMesh(&mesh);
    played.Play(1u, false);
    EXPECT_NEAR(blended.Time(), played.Time(), 0.0f);
    ExpectMatrixNear(ReadRootPalette(blended), ReadRootPalette(played), 0.0f);

    EXPECT_TRUE(blended.BlendTo(0u, 1.0f, true));
    blended.Update(0.5f);
    blended.Play(1u, false);
    EXPECT_NEAR(blended.Time(), 0.0f, 0.0f);
    ExpectMatrixNear(ReadRootPalette(blended), ReadRootPalette(played), 0.0f);
}

ACS_TEST(SkinnedMeshAnimationBlend, ComponentSupportsIndexAndNameWithoutInvalidMutation)
{
    TSharedPtr<ASkinnedMeshAsset> mesh = MakeShared<ASkinnedMeshAsset>();
    EXPECT_TRUE(static_cast<bool>(mesh));
    if (!mesh) return;
    const bool built = BuildBlendMesh(*mesh);
    EXPECT_TRUE(built);
    if (!built) return;

    ASkinnedMeshComponent3D component;
    EXPECT_FALSE(component.BlendTo(0u, 0.5f, true));
    component.SetMeshAsset(mesh);
    EXPECT_TRUE(component.PlayByName(FStringView("Source"), true));
    component.OnUpdate(0.25f);
    const f32 source_time = component.Player().Time();
    EXPECT_FALSE(component.BlendToByName(FStringView("Missing"), 0.5f, true));
    EXPECT_FALSE(component.BlendToByName(FStringView("Target"), std::numeric_limits<f32>::infinity(), true));
    EXPECT_NEAR(component.Player().Time(), source_time, 0.0f);

    EXPECT_TRUE(component.BlendToByName(FStringView("Target"), 0.5f, false));
    component.OnUpdate(0.25f);
    EXPECT_NEAR(component.Player().Time(), 0.25f, 1.0e-6f);
    const FMat4 blended_pose = ReadRootPalette(component.Player());
    EXPECT_FALSE(component.BlendToByName(FStringView("Source"), 0.25f, true));
    EXPECT_NEAR(component.Player().Time(), 0.25f, 0.0f);
    ExpectMatrixNear(ReadRootPalette(component.Player()), blended_pose, 0.0f);
    EXPECT_FALSE(component.BlendTo(7u, 0.5f, true));
    EXPECT_NEAR(component.Player().Time(), 0.25f, 0.0f);
}
