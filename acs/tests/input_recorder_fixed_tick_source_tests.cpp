// SPDX-License-Identifier: Apache-2.0
#include "gameframework/InputRecorderFixedTickSource.h"
#include "gameframework/InputStateSnapshot.h"
#include "gameframework/Lockstep.h"
#include "test/Expect.h"
#include "test/Test.h"

using namespace acs;
using namespace acs::game;

namespace {

/** test用raw codeを二つのACSキーへ変換する。 */
bool DecodeTestKey(u8 key_code, EKey& output) noexcept
{
    if (key_code == 1u)
        output = EKey::Space;
    else if (key_code == 2u)
        output = EKey::Enter;
    else
        return false;
    return true;
}

/** tick 0でSpaceと左mouseを押し、tick 2で両方を離す疎な録画を作る。 */
void PopulateSparseRecording(FInputRecorder& recorder) noexcept
{
    recorder.StartRecording(60u);
    FInputSample pressed{};
    pressed.tick = 0u;
    pressed.key_codes_changed[0] = 1u;
    pressed.key_states[0] = 1u;
    pressed.mouse_button_states = 1u;
    recorder.Capture(pressed);

    FInputSample released{};
    released.tick = 2u;
    released.key_codes_changed[0] = 1u;
    released.key_states[0] = 0u;
    released.mouse_button_states = 0u;
    recorder.Capture(released);
    recorder.StopRecording();
}

} // namespace

ACS_TEST(InputRecorderFixedTickSource, SparseSamplesReconstructLevelsEdgesAndRollback)
{
    FInputRecorder recorder;
    PopulateSparseRecording(recorder);
    const u32 recorder_tick = recorder.CurrentTick();
    FInputRecorderFixedTickSource source(recorder, DecodeTestKey);
    EXPECT_EQ(source.TickRateHz(), 60u);

    FInputStateSnapshot tick0;
    EXPECT_TRUE(source.TryCaptureFixedTickInput(0u, tick0));
    EXPECT_TRUE(tick0.IsKeyDown(EKey::Space));
    EXPECT_TRUE(tick0.IsKeyPressed(EKey::Space));
    EXPECT_FALSE(tick0.IsKeyReleased(EKey::Space));
    EXPECT_TRUE(tick0.IsMouseButtonDown(EMouseButton::Left));
    EXPECT_TRUE(tick0.IsMouseButtonPressed(EMouseButton::Left));

    FInputStateSnapshot tick1;
    EXPECT_TRUE(source.TryCaptureFixedTickInput(1u, tick1));
    EXPECT_TRUE(tick1.IsKeyDown(EKey::Space));
    EXPECT_FALSE(tick1.IsKeyPressed(EKey::Space));
    EXPECT_FALSE(tick1.IsKeyReleased(EKey::Space));
    EXPECT_TRUE(tick1.IsMouseButtonDown(EMouseButton::Left));
    EXPECT_FALSE(tick1.IsMouseButtonPressed(EMouseButton::Left));

    FInputStateSnapshot tick2;
    EXPECT_TRUE(source.TryCaptureFixedTickInput(2u, tick2));
    EXPECT_FALSE(tick2.IsKeyDown(EKey::Space));
    EXPECT_TRUE(tick2.IsKeyReleased(EKey::Space));
    EXPECT_FALSE(tick2.IsMouseButtonDown(EMouseButton::Left));
    EXPECT_TRUE(tick2.IsMouseButtonReleased(EMouseButton::Left));

    FInputStateSnapshot replayed_tick0;
    EXPECT_TRUE(source.TryCaptureFixedTickInput(0u, replayed_tick0));
    EXPECT_TRUE(replayed_tick0.IsKeyDown(EKey::Space));
    EXPECT_TRUE(replayed_tick0.IsKeyPressed(EKey::Space));
    EXPECT_EQ(recorder.CurrentTick(), recorder_tick);
    EXPECT_EQ(static_cast<u32>(recorder.CurrentMode()), static_cast<u32>(ERecorderMode::Idle));
}

ACS_TEST(InputRecorderFixedTickSource, InvalidRawStatePreservesOutputAndCachedState)
{
    FInputRecorder recorder;
    recorder.StartRecording(60u);
    FInputSample valid{};
    valid.tick = 0u;
    valid.key_codes_changed[0] = 1u;
    valid.key_states[0] = 1u;
    recorder.Capture(valid);
    FInputSample invalid{};
    invalid.tick = 1u;
    invalid.key_codes_changed[0] = 1u;
    invalid.key_states[0] = 2u;
    recorder.Capture(invalid);
    recorder.StopRecording();

    FInputRecorderFixedTickSource source(recorder, DecodeTestKey);
    FInputStateSnapshot tick0;
    EXPECT_TRUE(source.TryCaptureFixedTickInput(0u, tick0));

    FInputStateSnapshot unchanged;
    EXPECT_TRUE(unchanged.TrySetKeyState(EKey::Enter, true, true, false));
    EXPECT_FALSE(source.TryCaptureFixedTickInput(1u, unchanged));
    EXPECT_TRUE(unchanged.IsKeyDown(EKey::Enter));
    EXPECT_TRUE(unchanged.IsKeyPressed(EKey::Enter));
    EXPECT_FALSE(unchanged.IsKeyDown(EKey::Space));

    FInputStateSnapshot replayed_tick0;
    EXPECT_TRUE(source.TryCaptureFixedTickInput(0u, replayed_tick0));
    EXPECT_TRUE(replayed_tick0.IsKeyDown(EKey::Space));
    EXPECT_TRUE(replayed_tick0.IsKeyPressed(EKey::Space));
}

ACS_TEST(InputRecorderFixedTickSource, DecoderMouseMaskAndTickBoundsAreValidated)
{
    FInputRecorder recorder;
    recorder.StartRecording(60u);
    FInputSample sample{};
    sample.tick = 0u;
    sample.key_codes_changed[0] = 99u;
    sample.key_states[0] = 1u;
    recorder.Capture(sample);
    recorder.StopRecording();

    FInputRecorderFixedTickSource source(recorder, DecodeTestKey);
    FInputStateSnapshot unchanged;
    EXPECT_TRUE(unchanged.TrySetKeyState(EKey::Enter, true, false, false));
    EXPECT_FALSE(source.TryCaptureFixedTickInput(0u, unchanged));
    EXPECT_TRUE(unchanged.IsKeyDown(EKey::Enter));
    EXPECT_FALSE(source.TryCaptureFixedTickInput(static_cast<u64>(~u32{0}) + 1u, unchanged));

    FInputRecorder invalid_mouse;
    invalid_mouse.StartRecording(60u);
    FInputSample mouse_sample{};
    mouse_sample.mouse_button_states = 0x20u;
    invalid_mouse.Capture(mouse_sample);
    invalid_mouse.StopRecording();
    FInputRecorderFixedTickSource mouse_source(invalid_mouse, DecodeTestKey);
    EXPECT_FALSE(mouse_source.TryCaptureFixedTickInput(0u, unchanged));

    FInputRecorderFixedTickSource missing_decoder(invalid_mouse, nullptr);
    EXPECT_FALSE(missing_decoder.TryCaptureFixedTickInput(0u, unchanged));
}

ACS_TEST(InputRecorderPersistenceAdapter, SelfStagingIsRejectedWithoutMutation)
{
    FInputRecorder recorder;
    PopulateSparseRecording(recorder);
    EXPECT_FALSE(recorder.PersistenceAccess().PrepareLoadStaging(recorder));
    EXPECT_FALSE(recorder.PersistenceAccess().CommitLoadedState(recorder));
    EXPECT_EQ(recorder.SampleCount(), 2u);

    FLockstep lockstep;
    lockstep.Init(ENetMode::Local, 60u);
    FInputFrame frame{};
    lockstep.RecordInput(frame);
    EXPECT_FALSE(lockstep.PersistenceAccess().PrepareLoadStaging(lockstep));
    EXPECT_FALSE(lockstep.PersistenceAccess().CommitLoadedState(lockstep));
    EXPECT_EQ(lockstep.InputCount(), 1u);
}
