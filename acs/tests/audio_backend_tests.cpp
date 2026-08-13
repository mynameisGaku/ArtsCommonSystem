// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "foundation/Limits.h"
#include "gameframework/audio_backend/IAudioBackend.h"
#include "gameframework/audio_backend/XAudio2Backend.h"

using namespace acs;
using namespace acs::game;

namespace {

/** SetVoiceParameters を上書きせず既定の音量 fallback を記録する backend。 */
class FDefaultVoiceParametersBackend final : public IAudioBackend {
public:
    /** 初期化済みとして成功する。 */
    TResult<void> Init(u32) noexcept override
    {
        return Ok();
    }

    /** 保持資源がないため何もしない。 */
    void Shutdown() noexcept override
    {
    }

    /** 本テストでは常に利用可能とする。 */
    bool IsInitialized() const noexcept override
    {
        return true;
    }

    /** 再生を行わず無効ハンドルを返す。 */
    FAudioVoiceHandle PlayOneShot(const FAudioClipDesc&, f32, f32) noexcept override
    {
        return kInvalidAudioVoice;
    }

    /** 再生を行わず無効ハンドルを返す。 */
    FAudioVoiceHandle PlayLooped(const FAudioClipDesc&, f32, f32) noexcept override
    {
        return kInvalidAudioVoice;
    }

    /** 保持 voice がないため何もしない。 */
    void StopVoice(FAudioVoiceHandle) noexcept override
    {
    }

    /** 既定 SetVoiceParameters から渡された voice と音量を記録する。 */
    void SetVoiceVolume(FAudioVoiceHandle Voice, f32 Volume) noexcept override
    {
        LastVoice = Voice;
        LastVolume = Volume;
        ++SetVolumeCallCount;
    }

    /** 保持 voice がないため何もしない。 */
    void StopAllVoices() noexcept override
    {
    }

    /** 保持 voice がないため 0 を返す。 */
    u32 ActiveVoiceCount() const noexcept override
    {
        return 0u;
    }

    /** 進める内部状態がないため何もしない。 */
    void Tick(f32) noexcept override
    {
    }

    /** 最後に音量更新された voice。 */
    FAudioVoiceHandle LastVoice{};

    /** 最後に渡された正規化済み音量。 */
    f32 LastVolume = -1.0f;

    /** SetVoiceVolume が呼ばれた回数。 */
    u32 SetVolumeCallCount = 0u;
};

} // namespace

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

ACS_TEST(AudioBackend, VoiceParametersDefaultUsesFiniteClampedVolumeFallback)
{
    FDefaultVoiceParametersBackend Backend;
    const FAudioVoiceHandle Voice = FAudioVoiceHandle::FromPackedValue(17u);

    Backend.SetVoiceParameters(Voice, 0.25f, -1.0f, 4.0f);
    EXPECT_EQ(Backend.LastVoice, Voice);
    EXPECT_EQ(Backend.LastVolume, 0.25f);
    EXPECT_EQ(Backend.SetVolumeCallCount, 1u);

    Backend.SetVoiceParameters(Voice, 2.0f, 4.0f, -8.0f);
    EXPECT_EQ(Backend.LastVolume, 1.0f);
    EXPECT_EQ(Backend.SetVolumeCallCount, 2u);

    Backend.SetVoiceParameters(Voice, -1.0f, 0.0f, 1.0f);
    EXPECT_EQ(Backend.LastVolume, 0.0f);
    EXPECT_EQ(Backend.SetVolumeCallCount, 3u);

    const f32 PositiveInfinity = TNumLimits<f32>::Infinity();
    volatile f32 RuntimeInfinity = PositiveInfinity;
    const f32 NotANumber = RuntimeInfinity - RuntimeInfinity;
    Backend.SetVoiceParameters(Voice, PositiveInfinity, NotANumber, PositiveInfinity);
    EXPECT_EQ(Backend.LastVolume, 0.0f);
    EXPECT_EQ(Backend.SetVolumeCallCount, 4u);

    Backend.SetVoiceParameters(Voice, NotANumber, 0.0f, 1.0f);
    EXPECT_EQ(Backend.LastVolume, 0.0f);
    EXPECT_EQ(Backend.SetVolumeCallCount, 5u);
}
