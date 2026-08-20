// SPDX-License-Identifier: Apache-2.0
#include "gameframework/InputRecorderFixedTickSource.h"
#include "gameframework/InputStateSnapshot.h"
#include "gameframework/Lockstep.h"
#include "test/Expect.h"
#include "test/Test.h"

using namespace acs;
using namespace acs::game;

namespace {

/** 検証用raw code 7だけをSpaceへ変換する。 */
bool DecodeRecordedKey(u8 key_code, EKey& output) noexcept
{
    if (key_code != 7u) return false;
    output = EKey::Space;
    return true;
}

/** 指定tickのSpace変化とマウス保持状態を表すsampleを作る。 */
FInputSample MakeSample(u32 tick, bool space_down, bool mouse_down) noexcept
{
    FInputSample sample{};
    sample.tick = tick;
    sample.key_codes_changed[0] = 7u;
    sample.key_states[0] = space_down ? 1u : 0u;
    sample.mouse_button_states = mouse_down ? 1u : 0u;
    return sample;
}

} // namespace

ACS_TEST(InputRecorderFixedTickSource, SequentialAndRollbackTicksReproduceEdges)
{
    CInputRecorder recorder;
    recorder.StartRecording(60u);
    recorder.Capture(MakeSample(0u, true, true));
    recorder.Capture(MakeSample(2u, false, false));

    CInputRecorderFixedTickSource source(recorder, &DecodeRecordedKey);
    FInputStateSnapshot tick_zero;
    FInputStateSnapshot tick_one;
    FInputStateSnapshot tick_two;
    EXPECT_TRUE(source.TryCaptureFixedTickInput(0u, tick_zero));
    EXPECT_TRUE(source.TryCaptureFixedTickInput(1u, tick_one));
    EXPECT_TRUE(source.TryCaptureFixedTickInput(2u, tick_two));

    EXPECT_TRUE(tick_zero.IsKeyDown(EKey::Space));
    EXPECT_TRUE(tick_zero.IsKeyPressed(EKey::Space));
    EXPECT_TRUE(tick_zero.IsMouseButtonPressed(EMouseButton::Left));
    EXPECT_TRUE(tick_one.IsKeyDown(EKey::Space));
    EXPECT_FALSE(tick_one.IsKeyPressed(EKey::Space));
    EXPECT_FALSE(tick_one.IsKeyReleased(EKey::Space));
    EXPECT_FALSE(tick_two.IsKeyDown(EKey::Space));
    EXPECT_TRUE(tick_two.IsKeyReleased(EKey::Space));
    EXPECT_TRUE(tick_two.IsMouseButtonReleased(EMouseButton::Left));

    FInputStateSnapshot replayed_tick_zero;
    EXPECT_TRUE(source.TryCaptureFixedTickInput(0u, replayed_tick_zero));
    EXPECT_TRUE(replayed_tick_zero.IsKeyDown(EKey::Space));
    EXPECT_TRUE(replayed_tick_zero.IsKeyPressed(EKey::Space));
}

ACS_TEST(InputRecorderFixedTickSource, MalformedRecordingPreservesOutput)
{
    CInputRecorder recorder;
    recorder.StartRecording(60u);
    FInputSample malformed{};
    malformed.tick = 0u;
    malformed.key_codes_changed[0] = 9u;
    malformed.key_states[0] = 1u;
    recorder.Capture(malformed);

    CInputRecorderFixedTickSource source(recorder, &DecodeRecordedKey);
    FInputStateSnapshot output;
    EXPECT_TRUE(output.TrySetKeyState(EKey::Enter, true, true, false));
    EXPECT_FALSE(source.TryCaptureFixedTickInput(0u, output));
    EXPECT_TRUE(output.IsKeyDown(EKey::Enter));
    EXPECT_TRUE(output.IsKeyPressed(EKey::Enter));
}

ACS_TEST(InputRecorderFixedTickSource, PersistenceAdaptersRejectAliasesWithoutFriendAccess)
{
    CInputRecorder recorder;
    CInputRecorder recorder_staging;
    EXPECT_FALSE(recorder.PersistenceAccess().PrepareLoadStaging(recorder));
    EXPECT_TRUE(recorder.PersistenceAccess().PrepareLoadStaging(recorder_staging));
    EXPECT_FALSE(recorder.PersistenceAccess().CommitLoadedState(recorder));

    CLockstep lockstep;
    CLockstep lockstep_staging;
    EXPECT_FALSE(lockstep.PersistenceAccess().PrepareLoadStaging(lockstep));
    EXPECT_TRUE(lockstep.PersistenceAccess().PrepareLoadStaging(lockstep_staging));
    EXPECT_FALSE(lockstep.PersistenceAccess().CommitLoadedState(lockstep));
}
