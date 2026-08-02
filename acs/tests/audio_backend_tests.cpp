// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/audio_backend/IAudioBackend.h"
#include "gameframework/audio_backend/XAudio2Backend.h"

using namespace acs;
using namespace acs::game;

ACS_TEST(AudioBackend, VoiceHandleRetains32BitAbi)
{
    EXPECT_EQ(sizeof(FAudioVoiceHandle), sizeof(u32));
    EXPECT_EQ(alignof(FAudioVoiceHandle), alignof(u32));

    const FAudioVoiceHandle compatible{0x00123456u, 0xABu};
    EXPECT_EQ(compatible.Index(), 0x00123456u);
    EXPECT_EQ(compatible.Generation(), static_cast<u8>(0xABu));
    EXPECT_EQ(compatible.PackedValue(), 0xAB123456u);
}

ACS_TEST(AudioBackend, VoiceHandleAcceptsFullWidthOpaqueTickets)
{
    const FAudioVoiceHandle first = FAudioVoiceHandle::FromPackedValue(1u);
    const FAudioVoiceHandle after_old_generation_wrap = FAudioVoiceHandle::FromPackedValue(256u);
    const FAudioVoiceHandle largest = FAudioVoiceHandle::FromPackedValue(static_cast<u32>(~u32(0)));

    EXPECT_TRUE(first.IsValid());
    EXPECT_TRUE(after_old_generation_wrap.IsValid());
    EXPECT_TRUE(largest.IsValid());
    EXPECT_FALSE(first == after_old_generation_wrap);
    EXPECT_EQ(largest.PackedValue(), static_cast<u32>(~u32(0)));
    EXPECT_FALSE(FAudioVoiceHandle::FromPackedValue(0u).IsValid());
}

ACS_TEST(AudioBackend, XAudio2RejectsUnboundedVoicePoolsBeforeOsInitialization)
{
    CXAudio2Backend Backend;

    EXPECT_TRUE(Backend.Init(0u).IsErr());
    EXPECT_TRUE(Backend.Init(kXAudio2BackendMaximumVoiceCount + 1u).IsErr());
    EXPECT_FALSE(Backend.IsInitialized());
    EXPECT_EQ(Backend.ActiveVoiceCount(), 0u);
}
