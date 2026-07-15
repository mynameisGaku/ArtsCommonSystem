// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/audio_backend/IAudioBackend.h"
#include "gameframework/audio_backend/XAudio2Backend.h"

using namespace acs;
using namespace acs::game;

ACS_TEST(AudioBackend, VoiceHandleRetains32BitAbi)
{
    EXPECT_EQ(sizeof(AudioVoiceHandle), sizeof(u32));
    EXPECT_EQ(alignof(AudioVoiceHandle), alignof(u32));

    const AudioVoiceHandle compatible{0x00123456u, 0xABu};
    EXPECT_EQ(compatible.Index(), 0x00123456u);
    EXPECT_EQ(compatible.Generation(), static_cast<u8>(0xABu));
    EXPECT_EQ(compatible.PackedValue(), 0xAB123456u);
}

ACS_TEST(AudioBackend, VoiceHandleAcceptsFullWidthOpaqueTickets)
{
    const AudioVoiceHandle first = AudioVoiceHandle::FromPackedValue(1u);
    const AudioVoiceHandle after_old_generation_wrap = AudioVoiceHandle::FromPackedValue(256u);
    const AudioVoiceHandle largest = AudioVoiceHandle::FromPackedValue(static_cast<u32>(~u32(0)));

    EXPECT_TRUE(first.IsValid());
    EXPECT_TRUE(after_old_generation_wrap.IsValid());
    EXPECT_TRUE(largest.IsValid());
    EXPECT_FALSE(first == after_old_generation_wrap);
    EXPECT_EQ(largest.PackedValue(), static_cast<u32>(~u32(0)));
    EXPECT_FALSE(AudioVoiceHandle::FromPackedValue(0u).IsValid());
}

ACS_TEST(AudioBackend, XAudio2RejectsUnboundedVoicePoolsBeforeOsInitialization)
{
    FXAudio2Backend Backend;

    EXPECT_TRUE(Backend.Init(0u).IsErr());
    EXPECT_TRUE(Backend.Init(kXAudio2BackendMaximumVoiceCount + 1u).IsErr());
    EXPECT_FALSE(Backend.IsInitialized());
    EXPECT_EQ(Backend.ActiveVoiceCount(), 0u);
}
