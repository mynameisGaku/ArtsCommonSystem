// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "asset/AssetRegistry.h"
#include "asset/AudioAsset.h"
#include "foundation/Move.h"
#include "gameframework/AudioDirector.h"
#include "gameframework/SpatialAudio.h"
#include "memory/SharedPtr.h"
#include "platform/FileSystem.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

using namespace acs;
using namespace acs::game;

namespace {

/** Director から受けた voice parameter 更新を記録する test backend。 */
class CRecordingAudioBackend final : public IAudioBackend {
public:
    TResult<void> Init(u32 max_voices = 64u) noexcept override
    {
        (void)max_voices;
        m_Initialized = true;
        return Ok();
    }

    void Shutdown() noexcept override
    {
        m_Initialized = false;
        m_Active = false;
    }

    bool IsInitialized() const noexcept override { return m_Initialized; }

    FAudioVoiceHandle PlayOneShot(const FAudioClipDesc& clip,
                                  f32 volume,
                                  f32 pitch) noexcept override
    {
        (void)clip;
        if (!m_Initialized) return kInvalidAudioVoice;
        if (m_FailNextPlay) {
            m_FailNextPlay = false;
            return kInvalidAudioVoice;
        }
        ++m_PlayCallCount;
        m_LastPlayVolume = volume;
        m_LastPlayPitch = pitch;
        m_AllPlayVolumesFinite = m_AllPlayVolumesFinite && std::isfinite(volume);
        m_AllPlayPitchesFinite = m_AllPlayPitchesFinite && std::isfinite(pitch);
        m_Active = true;
        m_ActiveVoice = FAudioVoiceHandle::FromPackedValue(
            m_RepeatVoiceHandle ? 1u : m_NextVoiceValue++);
        return m_ActiveVoice;
    }

    FAudioVoiceHandle PlayLooped(const FAudioClipDesc& clip,
                                 f32 volume,
                                 f32 pitch) noexcept override
    {
        return PlayOneShot(clip, volume, pitch);
    }

    void StopVoice(FAudioVoiceHandle voice) noexcept override
    {
        if (m_StopRecordCount < kStopRecordCapacity) {
            m_StoppedVoices[m_StopRecordCount] = voice;
            ++m_StopRecordCount;
        }
        ++m_StopVoiceCount;
        if (m_Active && voice == m_ActiveVoice) m_Active = false;
    }

    void SetVoiceVolume(FAudioVoiceHandle voice, f32 volume) noexcept override
    {
        (void)voice;
        ++m_VoiceVolumeCallCount;
        m_LastVoiceVolume = volume;
        m_AllVoiceVolumesFinite = m_AllVoiceVolumesFinite && std::isfinite(volume);
    }

    void StopAllVoices() noexcept override { m_Active = false; }

    u32 ActiveVoiceCount() const noexcept override { return m_Active ? 1u : 0u; }

    void Tick(f32 dt) noexcept override
    {
        ++m_TickCallCount;
        m_LastTickDelta = dt;
        m_AllTickDeltasFinite = m_AllTickDeltasFinite && std::isfinite(dt);
    }

    void SetVoiceParameters(FAudioVoiceHandle voice,
                            f32 volume,
                            f32 pan,
                            f32 pitch) noexcept override
    {
        ++m_ParameterCallCount;
        if (!m_Initialized || !m_Active || !(voice == m_ActiveVoice)) return;
        ++m_AppliedParameterCount;
        m_LastVolume = volume;
        m_LastPan = pan;
        m_LastPitch = pitch;
    }

    void ResetParameterRecords() noexcept
    {
        m_ParameterCallCount = 0u;
        m_AppliedParameterCount = 0u;
        m_LastVolume = 0.0f;
        m_LastPan = 0.0f;
        m_LastPitch = 0.0f;
    }

    /** 再生要求ごとに同じ voice handle を返すかを設定する。 */
    void SetRepeatVoiceHandle(bool repeat) noexcept { m_RepeatVoiceHandle = repeat; }

    /** 次の再生要求だけを失敗させる。 */
    void SetFailNextPlay() noexcept
    {
        m_FailNextPlay = true;
    }

    /** 指定した voice handle が停止された回数を返す。 */
    u32 StopVoiceCount(FAudioVoiceHandle voice) const noexcept
    {
        u32 count = 0u;
        for (u32 index = 0u; index < m_StopRecordCount; ++index) {
            if (m_StoppedVoices[index] == voice) ++count;
        }
        return count;
    }

    u32 ParameterCallCount() const noexcept { return m_ParameterCallCount; }
    u32 AppliedParameterCount() const noexcept { return m_AppliedParameterCount; }
    u32 StopVoiceCount() const noexcept { return m_StopVoiceCount; }
    u32 PlayCallCount() const noexcept { return m_PlayCallCount; }
    u32 VoiceVolumeCallCount() const noexcept { return m_VoiceVolumeCallCount; }
    u32 TickCallCount() const noexcept { return m_TickCallCount; }
    f32 LastPlayVolume() const noexcept { return m_LastPlayVolume; }
    f32 LastPlayPitch() const noexcept { return m_LastPlayPitch; }
    f32 LastVoiceVolume() const noexcept { return m_LastVoiceVolume; }
    f32 LastTickDelta() const noexcept { return m_LastTickDelta; }
    bool AllPlayVolumesFinite() const noexcept { return m_AllPlayVolumesFinite; }
    bool AllPlayPitchesFinite() const noexcept { return m_AllPlayPitchesFinite; }
    bool AllVoiceVolumesFinite() const noexcept { return m_AllVoiceVolumesFinite; }
    bool AllTickDeltasFinite() const noexcept { return m_AllTickDeltasFinite; }
    f32 LastVolume() const noexcept { return m_LastVolume; }
    f32 LastPan() const noexcept { return m_LastPan; }
    f32 LastPitch() const noexcept { return m_LastPitch; }

private:
    /** テスト中に保持する停止要求の最大件数。 */
    static constexpr u32 kStopRecordCapacity = 8u;

    FAudioVoiceHandle m_ActiveVoice = kInvalidAudioVoice;
    FAudioVoiceHandle m_StoppedVoices[kStopRecordCapacity]{};
    u32 m_NextVoiceValue = 1u;
    u32 m_StopRecordCount = 0u;
    u32 m_ParameterCallCount = 0u;
    u32 m_AppliedParameterCount = 0u;
    u32 m_StopVoiceCount = 0u;
    u32 m_PlayCallCount = 0u;
    u32 m_VoiceVolumeCallCount = 0u;
    u32 m_TickCallCount = 0u;
    f32 m_LastPlayVolume = 0.0f;
    f32 m_LastPlayPitch = 0.0f;
    f32 m_LastVoiceVolume = 0.0f;
    f32 m_LastTickDelta = 0.0f;
    f32 m_LastVolume = 0.0f;
    f32 m_LastPan = 0.0f;
    f32 m_LastPitch = 0.0f;
    bool m_Initialized = false;
    bool m_Active = false;
    bool m_FailNextPlay = false;
    bool m_RepeatVoiceHandle = false;
    bool m_AllPlayVolumesFinite = true;
    bool m_AllPlayPitchesFinite = true;
    bool m_AllVoiceVolumesFinite = true;
    bool m_AllTickDeltasFinite = true;
};

/** 名前解決経路を実ファイル I/O まで通し、最小 PCM asset を返す検証用 loader。 */
class CTestAudioAssetLoader final : public IAssetLoader {
public:
    /** 生成する asset 型を返す。 */
    AssetType TypeId() const noexcept override { return AAudioAsset::StaticType(); }

    /** 検証用拡張子を返す。 */
    const char* Extension() const noexcept override { return "audiotest"; }

    /** 入力内容に依存しない無音 1 sample の audio asset を返す。 */
    TResult<TSharedPtr<AAsset>> LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept override
    {
        (void)id;
        (void)bytes;
        TArray<byte> samples;
        if (!samples.TrySetNum(sizeof(i16))) {
            return ACS_ERR(Memory, 2800u, "CTestAudioAssetLoader: allocation failed");
        }
        samples[0] = 0u;
        samples[1] = 0u;
        TSharedPtr<AAudioAsset> audio = MakeShared<AAudioAsset>(
            48000u, 1u, ESampleFormat::PCM_S16, 1u, Move(samples));
        if (!audio.Get()) {
            return ACS_ERR(Memory, 2801u, "CTestAudioAssetLoader: allocation failed");
        }
        TSharedPtr<AAsset> asset(Move(audio));
        return TResult<TSharedPtr<AAsset>>(OkInit, Move(asset));
    }
};

/** workspace 基準で source contract の検証対象を読み込む。 */
std::string ReadAudioWorkspaceSource(const char* relative_path)
{
    const std::filesystem::path test_file{__FILE__};
    const std::filesystem::path source_path =
        test_file.parent_path().parent_path() / std::filesystem::path{relative_path};
    std::ifstream stream(source_path, std::ios::binary);
    if (!stream) {
        stream.open(std::filesystem::path{"acs"} / std::filesystem::path{relative_path}, std::ios::binary);
    }
    return std::string{
        std::istreambuf_iterator<char>{stream},
        std::istreambuf_iterator<char>{}};
}

/** fake backend が受理する最小 mono clip を返す。 */
FAudioClipDesc MakeClip() noexcept
{
    static const i16 kSilentSample = 0;
    FAudioClipDesc clip;
    clip.pcm_data = &kSilentSample;
    clip.pcm_size = sizeof(kSilentSample);
    clip.sample_rate = 48000u;
    clip.channel_count = 1u;
    clip.format = EAudioFormat::Pcm16;
    return clip;
}

} // namespace

ACS_TEST(AudioDirectorSfxVoice, ResolvesNameAndReturnsBackendVoiceWithoutDirectorStateMutation)
{
    constexpr const wchar_t* kAudioPath = L"acs_audio_director_voice.audiotest";
    constexpr const char* kAudioPathUtf8 = "acs_audio_director_voice.audiotest";
    const byte file_payload[]{0u};
    (void)CFileSystem::Delete(kAudioPath);
    EXPECT_TRUE(CFileSystem::WriteAllBytes(kAudioPath, file_payload, sizeof(file_payload)).IsOk());

    CTestAudioAssetLoader loader;
    CAssetRegistry registry;
    EXPECT_TRUE(registry.TryRegisterLoader(&loader).IsOk());
    CRecordingAudioBackend backend;
    EXPECT_TRUE(backend.Init(4u).IsOk());
    CAudioDirector director;
    director.SetBackend(&backend);
    director.SetAssetRegistry(&registry);
    director.SetMasterVolume(0.5f);
    director.SetSfxVolume(0.5f);

    unsigned char state_before[sizeof(CAudioDirector)]{};
    std::memcpy(state_before, &director, sizeof(director));
    const FAudioVoiceHandle voice = director.PlaySfxVoice(kAudioPathUtf8, 2.0f, 1.5f);
    EXPECT_TRUE(voice.IsValid());
    EXPECT_EQ(backend.PlayCallCount(), 1u);
    EXPECT_NEAR(backend.LastPlayVolume(), 0.5f, 1e-4f);
    EXPECT_NEAR(backend.LastPlayPitch(), 1.5f, 1e-4f);
    EXPECT_EQ(std::memcmp(state_before, &director, sizeof(director)), 0);

    std::memcpy(state_before, &director, sizeof(director));
    EXPECT_FALSE(director.PlaySfxVoice("acs_audio_director_missing.audiotest").IsValid());
    EXPECT_EQ(std::memcmp(state_before, &director, sizeof(director)), 0);

    backend.SetFailNextPlay();
    std::memcpy(state_before, &director, sizeof(director));
    EXPECT_FALSE(director.PlaySfxVoice(kAudioPathUtf8).IsValid());
    EXPECT_EQ(std::memcmp(state_before, &director, sizeof(director)), 0);

    registry.Shutdown();
    EXPECT_TRUE(CFileSystem::Delete(kAudioPath).IsOk());
}

ACS_TEST(AudioDirectorSfxVoice, RejectsInvalidOrUnresolvedRequestsWithoutStateOnlyEntry)
{
    CRecordingAudioBackend backend;
    EXPECT_TRUE(backend.Init(4u).IsOk());
    CAssetRegistry registry;
    CAudioDirector director;
    director.SetBackend(&backend);
    director.SetAssetRegistry(&registry);

    unsigned char state_before[sizeof(CAudioDirector)]{};
    std::memcpy(state_before, &director, sizeof(director));
    EXPECT_FALSE(director.PlaySfxVoice(nullptr).IsValid());
    EXPECT_FALSE(director.PlaySfxVoice("").IsValid());
    EXPECT_FALSE(director.PlaySfxVoice("missing.audiotest").IsValid());
    EXPECT_FALSE(director.PlaySfxVoice("missing.audiotest", 0.0f).IsValid());
    EXPECT_FALSE(director.PlaySfxVoice(
        "missing.audiotest", std::numeric_limits<f32>::quiet_NaN()).IsValid());
    EXPECT_EQ(backend.PlayCallCount(), 0u);
    EXPECT_EQ(std::memcmp(state_before, &director, sizeof(director)), 0);

    CAudioDirector missing_registry;
    missing_registry.SetBackend(&backend);
    EXPECT_FALSE(missing_registry.PlaySfxVoice("missing.audiotest").IsValid());

    CAudioDirector missing_backend;
    missing_backend.SetAssetRegistry(&registry);
    EXPECT_FALSE(missing_backend.PlaySfxVoice("missing.audiotest").IsValid());
}

ACS_TEST(AudioDirectorSfx, NonFiniteVolumeIsNoOpBeforeBackendOrRingMutation)
{
    const f32 positive_infinity = std::numeric_limits<f32>::infinity();
    const f32 invalid_volumes[]{std::numeric_limits<f32>::quiet_NaN(), positive_infinity, -positive_infinity};

    CRecordingAudioBackend backend;
    EXPECT_TRUE(backend.Init(4u).IsOk());
    CAudioDirector director;
    director.SetBackend(&backend);

    for (const f32 volume : invalid_volumes) {
        unsigned char state_before[sizeof(CAudioDirector)]{};
        std::memcpy(state_before, &director, sizeof(director));
        const u32 play_calls_before = backend.PlayCallCount();

        director.PlaySfx("invalid-volume", volume);
        EXPECT_EQ(backend.PlayCallCount(), play_calls_before);
        EXPECT_EQ(std::memcmp(state_before, &director, sizeof(director)), 0);

        const FAudioVoiceHandle voice = director.PlaySfxClip(MakeClip(), volume, 1.0f);
        EXPECT_FALSE(voice.IsValid());
        EXPECT_EQ(backend.PlayCallCount(), play_calls_before);
    }
}

ACS_TEST(AudioDirectorSfx, NormalizesPitchAndKeepsEffectiveVolumeFinite)
{
    struct FPitchCase {
        f32 input;
        f32 expected;
    };

    const f32 positive_infinity = std::numeric_limits<f32>::infinity();
    const FPitchCase cases[]{
        {std::numeric_limits<f32>::quiet_NaN(), 1.0f},
        {positive_infinity, 1.0f},
        {-positive_infinity, 1.0f},
        {-1.0f, 0.25f},
        {8.0f, 4.0f},
        {1.5f, 1.5f},
    };

    CRecordingAudioBackend backend;
    EXPECT_TRUE(backend.Init(8u).IsOk());
    CAudioDirector director;
    director.SetBackend(&backend);

    for (const FPitchCase& value : cases) {
        const u32 play_calls_before = backend.PlayCallCount();
        const FAudioVoiceHandle voice = director.PlaySfxClip(MakeClip(), 2.0f, value.input);
        EXPECT_TRUE(voice.IsValid());
        EXPECT_EQ(backend.PlayCallCount(), play_calls_before + 1u);
        EXPECT_TRUE(std::isfinite(backend.LastPlayVolume()));
        EXPECT_NEAR(backend.LastPlayVolume(), 1.0f, 1e-4f);
        EXPECT_NEAR(backend.LastPlayPitch(), value.expected, 1e-4f);
        EXPECT_TRUE(backend.AllPlayVolumesFinite());
        EXPECT_TRUE(backend.AllPlayPitchesFinite());
    }
}

ACS_TEST(HrtfRendererStub, NormalizesNonFiniteListenerSourceAndSamples)
{
    const f32 not_a_number = std::numeric_limits<f32>::quiet_NaN();
    const f32 positive_infinity = std::numeric_limits<f32>::infinity();
    const f32 negative_infinity = -positive_infinity;

    CHrtfRendererStub renderer;
    EXPECT_TRUE(renderer.Init().IsOk());
    FAudioListener listener;
    listener.position = {not_a_number, 2.0f, negative_infinity};
    listener.forward = {positive_infinity, 0.0f, 1.0f};
    listener.up = {0.0f, 1.0f, not_a_number};
    renderer.SetListener(listener);

    FAudioSource3D source;
    source.position = {positive_infinity, 3.0f, not_a_number};
    source.volume = 0.75f;
    source.max_distance = 10.0f;
    source.active = true;
    const f32 input[]{not_a_number, positive_infinity, negative_infinity, 0.5f};
    f32 output[8]{};
    renderer.ProcessSource(source, input, output, 4u);

    for (const f32 sample : output) EXPECT_TRUE(std::isfinite(sample));
    for (u32 index = 0u; index < 6u; ++index) EXPECT_NEAR(output[index], 0.0f, 1e-4f);
    EXPECT_TRUE(output[6] > 0.0f);
    EXPECT_TRUE(output[7] > 0.0f);
    renderer.Shutdown();
}

ACS_TEST(HrtfRendererStub, NormalizesVolumeAndDefendsFinalOutput)
{
    const f32 positive_infinity = std::numeric_limits<f32>::infinity();
    const f32 invalid_volumes[]{std::numeric_limits<f32>::quiet_NaN(), positive_infinity, -positive_infinity};
    const f32 input[]{1.0f, std::numeric_limits<f32>::max()};

    CHrtfRendererStub renderer;
    EXPECT_TRUE(renderer.Init().IsOk());
    FAudioSource3D source;
    source.position = {0.0f, 0.0f, 1.0f};
    source.max_distance = 10.0f;

    for (const f32 volume : invalid_volumes) {
        source.volume = volume;
        f32 output[]{1.0f, 1.0f, 1.0f, 1.0f};
        renderer.ProcessSource(source, input, output, 2u);
        for (const f32 sample : output) {
            EXPECT_TRUE(std::isfinite(sample));
            EXPECT_NEAR(sample, 0.0f, 1e-4f);
        }
    }

    source.volume = 2.0f;
    f32 clamped_output[4]{};
    renderer.ProcessSource(source, input, clamped_output, 2u);
    for (const f32 sample : clamped_output) EXPECT_TRUE(std::isfinite(sample));
    EXPECT_TRUE(clamped_output[0] > 0.0f && clamped_output[0] <= 1.0f);
    EXPECT_TRUE(clamped_output[1] > 0.0f && clamped_output[1] <= 1.0f);
    renderer.Shutdown();
}

ACS_TEST(HrtfRendererStub, PreservesInvalidBufferAndInactiveContracts)
{
    CHrtfRendererStub renderer;
    FAudioSource3D source;
    const f32 input[]{1.0f, -1.0f};
    f32 output[]{7.0f, 7.0f, 7.0f, 7.0f};

    renderer.ProcessSource(source, nullptr, output, 2u);
    for (const f32 sample : output) EXPECT_NEAR(sample, 7.0f, 1e-4f);
    renderer.ProcessSource(source, input, output, 0u);
    for (const f32 sample : output) EXPECT_NEAR(sample, 7.0f, 1e-4f);
    renderer.ProcessSource(source, input, nullptr, 2u);

    source.active = false;
    renderer.ProcessSource(source, input, output, 2u);
    for (const f32 sample : output) EXPECT_NEAR(sample, 0.0f, 1e-4f);
}

ACS_TEST(SpatialAudioSourceId, ExhaustionIsPermanentAndNeverReusesStaleIds)
{
    CSpatialAudio spatial;
    const u32 maximum_id = std::numeric_limits<u32>::max();
    spatial.SetNextSourceIdForTesting(maximum_id);

    const u32 last_id = spatial.RegisterSource({0.0f, 0.0f, 0.0f}, 10.0f, EAttenuationCurve::Linear);
    EXPECT_EQ(last_id, maximum_id);
    EXPECT_EQ(spatial.SourceCount(), 1u);
    EXPECT_NEAR(spatial.ComputeAttenuatedVolume(last_id), 1.0f, 1e-4f);

    const u32 exhausted_id = spatial.RegisterSource({1.0f, 0.0f, 0.0f}, 10.0f, EAttenuationCurve::Linear);
    EXPECT_EQ(exhausted_id, 0u);
    EXPECT_EQ(spatial.SourceCount(), 1u);
    EXPECT_EQ(spatial.RegisterSource({2.0f, 0.0f, 0.0f}, 10.0f, EAttenuationCurve::Linear), 0u);
    EXPECT_EQ(spatial.SourceCount(), 1u);

    spatial.Clear();
    EXPECT_EQ(spatial.SourceCount(), 0u);
    EXPECT_NEAR(spatial.ComputeAttenuatedVolume(last_id), 0.0f, 1e-4f);
    EXPECT_EQ(spatial.RegisterSource({3.0f, 0.0f, 0.0f}, 10.0f, EAttenuationCurve::Linear), 0u);
    EXPECT_EQ(spatial.SourceCount(), 0u);
}

ACS_TEST(AudioDirectorBgm, ImmediateStopStopsCurrentAndFadingVoicesOnce)
{
    CRecordingAudioBackend backend;
    EXPECT_TRUE(backend.Init(4u).IsOk());
    CAudioDirector director;
    director.SetBackend(&backend);

    const FAudioVoiceHandle current_voice = director.PlayBgmClip(MakeClip(), 0.0f, true);
    const FAudioVoiceHandle fading_voice = director.PlayBgmClip(MakeClip(), 1.0f, true);
    EXPECT_TRUE(current_voice.IsValid());
    EXPECT_TRUE(fading_voice.IsValid());
    EXPECT_FALSE(current_voice == fading_voice);

    director.StopBgm(0.0f);
    EXPECT_EQ(backend.StopVoiceCount(), 2u);
    EXPECT_EQ(backend.StopVoiceCount(current_voice), 1u);
    EXPECT_EQ(backend.StopVoiceCount(fading_voice), 1u);
    EXPECT_NEAR(director.EffectiveBgmVolume(), 0.0f, 1e-4f);

    director.StopBgm(0.0f);
    director.Tick(1.0f);
    EXPECT_EQ(backend.StopVoiceCount(), 2u);
}

ACS_TEST(AudioDirectorBgm, ImmediateStopAcceptsNullBackendAndInvalidVoices)
{
    CAudioDirector state_only_director;
    state_only_director.PlayBgm("state-only", 0.0f, true);
    state_only_director.StopBgm(0.0f);
    EXPECT_TRUE(state_only_director.CurrentBgmName() == nullptr);
    EXPECT_NEAR(state_only_director.EffectiveBgmVolume(), 0.0f, 1e-4f);

    CRecordingAudioBackend backend;
    EXPECT_TRUE(backend.Init(4u).IsOk());
    CAudioDirector invalid_voice_director;
    invalid_voice_director.SetBackend(&backend);
    invalid_voice_director.PlayBgm("state-only", 0.0f, true);
    invalid_voice_director.StopBgm(0.0f);
    EXPECT_EQ(backend.StopVoiceCount(), 0u);
}

ACS_TEST(AudioDirectorBgm, ImmediateStopDeduplicatesRepeatedVoiceHandle)
{
    CRecordingAudioBackend backend;
    EXPECT_TRUE(backend.Init(4u).IsOk());
    backend.SetRepeatVoiceHandle(true);
    CAudioDirector director;
    director.SetBackend(&backend);

    const FAudioVoiceHandle current_voice = director.PlayBgmClip(MakeClip(), 0.0f, true);
    const FAudioVoiceHandle fading_voice = director.PlayBgmClip(MakeClip(), 1.0f, true);
    EXPECT_TRUE(current_voice.IsValid());
    EXPECT_TRUE(current_voice == fading_voice);

    director.StopBgm(0.0f);
    EXPECT_EQ(backend.StopVoiceCount(), 1u);
    EXPECT_EQ(backend.StopVoiceCount(current_voice), 1u);

    director.StopBgm(0.0f);
    director.Tick(1.0f);
    EXPECT_EQ(backend.StopVoiceCount(), 1u);
}

ACS_TEST(AudioDirectorBgm, NonFiniteStopStopsDistinctAndDuplicateVoicesImmediately)
{
    const f32 positive_infinity = std::numeric_limits<f32>::infinity();
    const f32 nonfinite_fades[]{std::numeric_limits<f32>::quiet_NaN(), positive_infinity, -positive_infinity};
    const bool repeat_handles[]{false, true};

    for (const f32 fade : nonfinite_fades) {
        for (const bool repeat_handle : repeat_handles) {
            CRecordingAudioBackend backend;
            EXPECT_TRUE(backend.Init(4u).IsOk());
            backend.SetRepeatVoiceHandle(repeat_handle);
            CAudioDirector director;
            director.SetBackend(&backend);

            const FAudioVoiceHandle current_voice = director.PlayBgmClip(MakeClip(), 0.0f, true);
            const FAudioVoiceHandle fading_voice = director.PlayBgmClip(MakeClip(), 1.0f, true);
            director.StopBgm(fade);

            const u32 expected_stop_count = repeat_handle ? 1u : 2u;
            EXPECT_EQ(backend.StopVoiceCount(), expected_stop_count);
            EXPECT_EQ(backend.StopVoiceCount(current_voice), 1u);
            EXPECT_EQ(backend.StopVoiceCount(fading_voice), 1u);
            EXPECT_NEAR(director.EffectiveBgmVolume(), 0.0f, 1e-4f);

            director.StopBgm(fade);
            director.Tick(1.0f);
            EXPECT_EQ(backend.StopVoiceCount(), expected_stop_count);
        }
    }
}

ACS_TEST(AudioDirectorBgm, NonFinitePlayFadeBecomesStableImmediateGain)
{
    const f32 positive_infinity = std::numeric_limits<f32>::infinity();
    const f32 nonfinite_fades[]{std::numeric_limits<f32>::quiet_NaN(), positive_infinity, -positive_infinity};

    for (const f32 fade : nonfinite_fades) {
        CRecordingAudioBackend backend;
        EXPECT_TRUE(backend.Init(4u).IsOk());
        CAudioDirector director;
        director.SetBackend(&backend);

        const FAudioVoiceHandle voice = director.PlayBgmClip(MakeClip(), fade, true);
        EXPECT_TRUE(voice.IsValid());
        EXPECT_TRUE(std::isfinite(director.EffectiveBgmVolume()));
        EXPECT_NEAR(director.EffectiveBgmVolume(), 1.0f, 1e-4f);
        director.Tick(1.0f);
        EXPECT_TRUE(std::isfinite(backend.LastVoiceVolume()));
        EXPECT_NEAR(backend.LastVoiceVolume(), 1.0f, 1e-4f);

        CAudioDirector state_only_director;
        state_only_director.PlayBgm("state-only", fade, true);
        EXPECT_NEAR(state_only_director.EffectiveBgmVolume(), 1.0f, 1e-4f);
    }
}

ACS_TEST(AudioDirectorBgm, SubnormalFadeUsesImmediatePlayAndStopPaths)
{
    const f32 subnormal_fade = std::numeric_limits<f32>::denorm_min();

    CAudioDirector state_only_director;
    state_only_director.PlayBgm("first", 0.0f, true);
    state_only_director.PlayBgm("second", subnormal_fade, true);
    state_only_director.Tick(0.0f);
    EXPECT_TRUE(std::isfinite(state_only_director.EffectiveBgmVolume()));
    EXPECT_NEAR(state_only_director.EffectiveBgmVolume(), 1.0f, 1e-4f);
    EXPECT_TRUE(state_only_director.CurrentBgmName() != nullptr);

    CRecordingAudioBackend backend;
    EXPECT_TRUE(backend.Init(4u).IsOk());
    CAudioDirector director;
    director.SetBackend(&backend);
    const FAudioVoiceHandle current_voice = director.PlayBgmClip(MakeClip(), 0.0f, true);
    const FAudioVoiceHandle replacement_voice = director.PlayBgmClip(MakeClip(), subnormal_fade, true);
    EXPECT_TRUE(current_voice.IsValid());
    EXPECT_TRUE(replacement_voice.IsValid());
    EXPECT_EQ(backend.StopVoiceCount(current_voice), 1u);

    director.Tick(0.0f);
    EXPECT_TRUE(std::isfinite(director.EffectiveBgmVolume()));
    EXPECT_TRUE(backend.AllVoiceVolumesFinite());
    director.StopBgm(subnormal_fade);
    EXPECT_EQ(backend.StopVoiceCount(replacement_voice), 1u);
    EXPECT_NEAR(director.EffectiveBgmVolume(), 0.0f, 1e-4f);
    director.Tick(1.0f);
    EXPECT_EQ(backend.StopVoiceCount(current_voice), 1u);
    EXPECT_EQ(backend.StopVoiceCount(replacement_voice), 1u);
}

ACS_TEST(AudioDirectorBgm, MinimumNormalFadeKeepsFiniteRatesAndClosesVoicesOnce)
{
    const f32 minimum_normal_fade = std::numeric_limits<f32>::min();
    CRecordingAudioBackend backend;
    EXPECT_TRUE(backend.Init(4u).IsOk());
    CAudioDirector director;
    director.SetBackend(&backend);

    const FAudioVoiceHandle current_voice = director.PlayBgmClip(MakeClip(), 0.0f, true);
    const FAudioVoiceHandle fading_voice = director.PlayBgmClip(MakeClip(), minimum_normal_fade, true);
    EXPECT_TRUE(current_voice.IsValid());
    EXPECT_TRUE(fading_voice.IsValid());
    EXPECT_EQ(backend.StopVoiceCount(), 0u);

    director.Tick(0.0f);
    EXPECT_EQ(backend.StopVoiceCount(), 0u);
    EXPECT_TRUE(std::isfinite(director.EffectiveBgmVolume()));
    EXPECT_TRUE(backend.AllVoiceVolumesFinite());

    director.Tick(1.0f);
    EXPECT_EQ(backend.StopVoiceCount(current_voice), 1u);
    EXPECT_EQ(backend.StopVoiceCount(fading_voice), 0u);
    EXPECT_TRUE(std::isfinite(director.EffectiveBgmVolume()));
    EXPECT_TRUE(backend.AllVoiceVolumesFinite());

    director.StopBgm(minimum_normal_fade);
    director.Tick(0.0f);
    EXPECT_EQ(backend.StopVoiceCount(fading_voice), 0u);
    EXPECT_TRUE(backend.AllVoiceVolumesFinite());
    director.Tick(1.0f);
    EXPECT_EQ(backend.StopVoiceCount(current_voice), 1u);
    EXPECT_EQ(backend.StopVoiceCount(fading_voice), 1u);
    EXPECT_NEAR(director.EffectiveBgmVolume(), 0.0f, 1e-4f);
    director.Tick(1.0f);
    EXPECT_EQ(backend.StopVoiceCount(), 2u);
}

ACS_TEST(AudioDirectorBgm, NonFiniteReplacementAndFailedPlayLeaveNoVoiceUnstopped)
{
    CRecordingAudioBackend backend;
    EXPECT_TRUE(backend.Init(4u).IsOk());
    CAudioDirector director;
    director.SetBackend(&backend);

    const FAudioVoiceHandle first_voice = director.PlayBgmClip(MakeClip(), 0.0f, true);
    const FAudioVoiceHandle fading_voice = director.PlayBgmClip(MakeClip(), 1.0f, true);
    const FAudioVoiceHandle replacement_voice = director.PlayBgmClip(MakeClip(), std::numeric_limits<f32>::quiet_NaN(),
                                                                     true);
    EXPECT_TRUE(replacement_voice.IsValid());
    EXPECT_EQ(backend.StopVoiceCount(first_voice), 1u);
    EXPECT_EQ(backend.StopVoiceCount(fading_voice), 1u);

    const FAudioVoiceHandle next_fading_voice = director.PlayBgmClip(MakeClip(), 1.0f, true);
    backend.SetFailNextPlay();
    const FAudioVoiceHandle failed_voice = director.PlayBgmClip(MakeClip(), std::numeric_limits<f32>::infinity(), true);
    EXPECT_FALSE(failed_voice.IsValid());
    EXPECT_EQ(backend.StopVoiceCount(replacement_voice), 1u);

    director.StopBgm(std::numeric_limits<f32>::infinity());
    EXPECT_EQ(backend.StopVoiceCount(next_fading_voice), 1u);
    EXPECT_NEAR(director.EffectiveBgmVolume(), 0.0f, 1e-4f);
}

ACS_TEST(AudioDirectorVolume, NonFiniteSettersKeepEffectiveAndBackendVolumesFinite)
{
    const f32 positive_infinity = std::numeric_limits<f32>::infinity();
    const f32 nonfinite_values[]{std::numeric_limits<f32>::quiet_NaN(), positive_infinity, -positive_infinity};

    for (const f32 value : nonfinite_values) {
        CRecordingAudioBackend backend;
        EXPECT_TRUE(backend.Init(4u).IsOk());
        CAudioDirector director;
        director.SetBackend(&backend);
        const FAudioVoiceHandle bgm_voice = director.PlayBgmClip(MakeClip(), 0.0f, true);
        EXPECT_TRUE(bgm_voice.IsValid());

        director.SetMasterVolume(value);
        EXPECT_NEAR(director.GetMasterVolume(), 0.0f, 1e-4f);
        EXPECT_TRUE(std::isfinite(director.EffectiveBgmVolume()));
        EXPECT_TRUE(std::isfinite(director.EffectiveSfxVolume()));
        EXPECT_TRUE(director.EffectiveBgmVolume() >= 0.0f && director.EffectiveBgmVolume() <= 1.0f);
        EXPECT_TRUE(director.EffectiveSfxVolume() >= 0.0f && director.EffectiveSfxVolume() <= 1.0f);
        director.SetMasterVolume(1.0f);

        director.SetBgmVolume(value);
        EXPECT_NEAR(director.GetBgmVolume(), 0.0f, 1e-4f);
        EXPECT_TRUE(std::isfinite(director.EffectiveBgmVolume()));
        EXPECT_TRUE(director.EffectiveBgmVolume() >= 0.0f && director.EffectiveBgmVolume() <= 1.0f);
        director.SetBgmVolume(1.0f);

        director.SetSfxVolume(value);
        EXPECT_NEAR(director.GetSfxVolume(), 0.0f, 1e-4f);
        EXPECT_TRUE(std::isfinite(director.EffectiveSfxVolume()));
        EXPECT_TRUE(director.EffectiveSfxVolume() >= 0.0f && director.EffectiveSfxVolume() <= 1.0f);

        director.SetMasterVolume(value);
        director.SetBgmVolume(value);
        const u32 volume_calls_before_tick = backend.VoiceVolumeCallCount();
        director.Tick(1.0f);
        EXPECT_EQ(backend.VoiceVolumeCallCount(), volume_calls_before_tick + 1u);
        EXPECT_TRUE(std::isfinite(backend.LastVoiceVolume()));
        EXPECT_TRUE(backend.LastVoiceVolume() >= 0.0f && backend.LastVoiceVolume() <= 1.0f);

        const u32 play_calls_before_sfx = backend.PlayCallCount();
        const FAudioVoiceHandle sfx_voice = director.PlaySfxClip(MakeClip(), 1.0f, 1.0f);
        EXPECT_TRUE(sfx_voice.IsValid());
        EXPECT_EQ(backend.PlayCallCount(), play_calls_before_sfx + 1u);
        EXPECT_TRUE(std::isfinite(backend.LastPlayVolume()));
        EXPECT_TRUE(backend.LastPlayVolume() >= 0.0f && backend.LastPlayVolume() <= 1.0f);
    }
}

ACS_TEST(AudioDirectorTime, InvalidTickDoesNotCorruptCrossfadeAndFiniteTicksCloseVoices)
{
    const f32 positive_infinity = std::numeric_limits<f32>::infinity();
    const f32 invalid_deltas[]{std::numeric_limits<f32>::quiet_NaN(), positive_infinity, -positive_infinity, -1.0f};

    CRecordingAudioBackend backend;
    EXPECT_TRUE(backend.Init(4u).IsOk());
    CAudioDirector director;
    director.SetBackend(&backend);
    const FAudioVoiceHandle current_voice = director.PlayBgmClip(MakeClip(), 0.0f, true);
    const FAudioVoiceHandle fading_voice = director.PlayBgmClip(MakeClip(), 1.0f, true);

    for (const f32 dt : invalid_deltas) {
        const u32 tick_calls_before = backend.TickCallCount();
        director.Tick(dt);
        EXPECT_EQ(backend.TickCallCount(), tick_calls_before + 1u);
        EXPECT_NEAR(backend.LastTickDelta(), 0.0f, 1e-4f);
        EXPECT_TRUE(backend.AllTickDeltasFinite());
        EXPECT_TRUE(backend.AllVoiceVolumesFinite());
        EXPECT_TRUE(std::isfinite(director.EffectiveBgmVolume()));
        EXPECT_EQ(backend.StopVoiceCount(), 0u);
    }

    const FAudioVoiceHandle sfx_voice = director.PlaySfxClip(MakeClip(), 1.0f, 1.0f);
    EXPECT_TRUE(sfx_voice.IsValid());
    EXPECT_TRUE(backend.AllPlayVolumesFinite());

    director.Tick(1.0f);
    EXPECT_EQ(backend.StopVoiceCount(current_voice), 1u);
    EXPECT_EQ(backend.StopVoiceCount(fading_voice), 0u);
    EXPECT_TRUE(backend.AllVoiceVolumesFinite());

    director.StopBgm(1.0f);
    director.Tick(1.0f);
    EXPECT_EQ(backend.StopVoiceCount(fading_voice), 1u);
    EXPECT_NEAR(director.EffectiveBgmVolume(), 0.0f, 1e-4f);
    director.Tick(1.0f);
    EXPECT_EQ(backend.StopVoiceCount(current_voice), 1u);
    EXPECT_EQ(backend.StopVoiceCount(fading_voice), 1u);
}

ACS_TEST(AudioDirectorTime, NonFiniteDuckDurationNeverCreatesPermanentDuck)
{
    const f32 positive_infinity = std::numeric_limits<f32>::infinity();
    const f32 invalid_durations[]{std::numeric_limits<f32>::quiet_NaN(), positive_infinity, -positive_infinity};

    for (const f32 duration : invalid_durations) {
        CRecordingAudioBackend backend;
        EXPECT_TRUE(backend.Init(4u).IsOk());
        CAudioDirector director;
        director.SetBackend(&backend);
        const FAudioVoiceHandle voice = director.PlayBgmClip(MakeClip(), 0.0f, true);

        director.Duck(0.25f, 0.25f);
        director.Duck(duration, 0.0f);
        EXPECT_TRUE(std::isfinite(director.EffectiveBgmVolume()));
        director.Tick(10.0f);
        EXPECT_TRUE(backend.AllTickDeltasFinite());
        EXPECT_TRUE(backend.AllVoiceVolumesFinite());
        EXPECT_TRUE(std::isfinite(director.EffectiveBgmVolume()));
        EXPECT_NEAR(director.EffectiveBgmVolume(), 1.0f, 1e-4f);

        director.StopBgm(0.0f);
        EXPECT_EQ(backend.StopVoiceCount(voice), 1u);
    }
}

ACS_TEST(SpatialAudioVoice, UpdatesCenterRightAndSilentOutOfRangeOnce)
{
    CRecordingAudioBackend backend;
    EXPECT_TRUE(backend.Init(4u).IsOk());
    CAudioDirector director;
    director.SetBackend(&backend);
    const FAudioVoiceHandle voice = director.PlaySfxClip(MakeClip());
    EXPECT_TRUE(voice.IsValid());

    CSpatialAudio spatial;
    spatial.SetListener(FAudioListener{});
    const u32 source_id = spatial.RegisterSource(
        {0.0f, 0.0f, 5.0f}, 10.0f, EAttenuationCurve::Linear);

    backend.ResetParameterRecords();
    director.UpdateSpatialSfxVoice(voice, spatial, source_id);
    EXPECT_EQ(backend.ParameterCallCount(), 1u);
    EXPECT_EQ(backend.AppliedParameterCount(), 1u);
    EXPECT_NEAR(backend.LastVolume(), 0.5f, 1e-4f);
    EXPECT_NEAR(backend.LastPan(), 0.0f, 1e-4f);

    spatial.UpdateSource(source_id, {5.0f, 0.0f, 0.0f});
    backend.ResetParameterRecords();
    director.UpdateSpatialSfxVoice(voice, spatial, source_id);
    EXPECT_EQ(backend.ParameterCallCount(), 1u);
    EXPECT_EQ(backend.AppliedParameterCount(), 1u);
    EXPECT_NEAR(backend.LastVolume(), 0.5f, 1e-4f);
    EXPECT_NEAR(backend.LastPan(), 1.0f, 1e-4f);

    spatial.UpdateSource(source_id, {20.0f, 0.0f, 0.0f});
    backend.ResetParameterRecords();
    director.UpdateSpatialSfxVoice(voice, spatial, source_id);
    EXPECT_EQ(backend.ParameterCallCount(), 1u);
    EXPECT_EQ(backend.AppliedParameterCount(), 1u);
    EXPECT_NEAR(backend.LastVolume(), 0.0f, 1e-4f);
    EXPECT_NEAR(backend.LastPan(), 1.0f, 1e-4f);
}

ACS_TEST(SpatialAudioVoice, ComposesDirectorSourceAndDistanceVolume)
{
    CRecordingAudioBackend backend;
    EXPECT_TRUE(backend.Init(4u).IsOk());
    CAudioDirector director;
    director.SetBackend(&backend);
    director.SetMasterVolume(0.5f);
    director.SetSfxVolume(0.4f);
    const FAudioVoiceHandle voice = director.PlaySfxClip(MakeClip());
    EXPECT_TRUE(voice.IsValid());

    CSpatialAudio spatial;
    const u32 source_id = spatial.RegisterSource(
        {0.0f, 0.0f, 5.0f}, 10.0f, EAttenuationCurve::Linear);
    spatial.SetSourceVolume(source_id, 0.25f);

    backend.ResetParameterRecords();
    director.UpdateSpatialSfxVoice(voice, spatial, source_id);
    EXPECT_EQ(backend.ParameterCallCount(), 1u);
    EXPECT_NEAR(backend.LastVolume(), 0.025f, 1e-4f);
}

ACS_TEST(SpatialAudioVoice, ComposesRequestVolumeAndPreservesFourArgumentPitchContract)
{
    CRecordingAudioBackend backend;
    EXPECT_TRUE(backend.Init(4u).IsOk());
    CAudioDirector director;
    director.SetBackend(&backend);
    director.SetMasterVolume(0.5f);
    director.SetSfxVolume(0.4f);
    const FAudioVoiceHandle voice = director.PlaySfxClip(MakeClip());
    EXPECT_TRUE(voice.IsValid());

    CSpatialAudio spatial;
    const u32 source_id = spatial.RegisterSource(
        {0.0f, 0.0f, 5.0f}, 10.0f, EAttenuationCurve::Linear);

    backend.ResetParameterRecords();
    director.UpdateSpatialSfxVoice(voice, spatial, source_id, 0.25f, 1.75f);
    EXPECT_EQ(backend.ParameterCallCount(), 1u);
    EXPECT_NEAR(backend.LastVolume(), 0.025f, 1e-4f);
    EXPECT_NEAR(backend.LastPitch(), 1.75f, 1e-4f);

    // 既存 4 引数版の第 4 引数は volume_scale ではなく pitch のまま維持する。
    backend.ResetParameterRecords();
    director.UpdateSpatialSfxVoice(voice, spatial, source_id, 0.25f);
    EXPECT_EQ(backend.ParameterCallCount(), 1u);
    EXPECT_NEAR(backend.LastVolume(), 0.1f, 1e-4f);
    EXPECT_NEAR(backend.LastPitch(), 0.25f, 1e-4f);

    backend.ResetParameterRecords();
    director.UpdateSpatialSfxVoice(
        voice, spatial, source_id, std::numeric_limits<f32>::quiet_NaN(), 1.0f);
    EXPECT_EQ(backend.ParameterCallCount(), 1u);
    EXPECT_NEAR(backend.LastVolume(), 0.0f, 1e-4f);

    backend.ResetParameterRecords();
    director.UpdateSpatialSfxVoice(voice, spatial, source_id, 2.0f, 1.0f);
    EXPECT_EQ(backend.ParameterCallCount(), 1u);
    EXPECT_NEAR(backend.LastVolume(), 0.2f, 1e-4f);

    spatial.RemoveSource(source_id);
    EXPECT_FALSE(spatial.HasSource(source_id));
    backend.ResetParameterRecords();
    director.UpdateSpatialSfxVoice(voice, spatial, source_id, 1.0f, 1.0f);
    EXPECT_EQ(backend.ParameterCallCount(), 0u);
}

ACS_TEST(SpatialAudioVoice, UpdatesPitchAndPauseResumeVolume)
{
    CRecordingAudioBackend backend;
    EXPECT_TRUE(backend.Init(4u).IsOk());
    CAudioDirector director;
    director.SetBackend(&backend);
    const FAudioVoiceHandle voice = director.PlaySfxClip(MakeClip());
    EXPECT_TRUE(voice.IsValid());

    CSpatialAudio spatial;
    const u32 source_id = spatial.RegisterSource(
        {0.0f, 0.0f, 5.0f}, 10.0f, EAttenuationCurve::Linear);

    backend.ResetParameterRecords();
    director.UpdateSpatialSfxVoice(voice, spatial, source_id, 1.75f);
    EXPECT_EQ(backend.ParameterCallCount(), 1u);
    EXPECT_NEAR(backend.LastVolume(), 0.5f, 1e-4f);
    EXPECT_NEAR(backend.LastPitch(), 1.75f, 1e-4f);

    director.Pause();
    backend.ResetParameterRecords();
    director.UpdateSpatialSfxVoice(voice, spatial, source_id, 2.0f);
    EXPECT_EQ(backend.ParameterCallCount(), 1u);
    EXPECT_NEAR(backend.LastVolume(), 0.0f, 1e-4f);
    EXPECT_NEAR(backend.LastPitch(), 2.0f, 1e-4f);

    director.Resume();
    backend.ResetParameterRecords();
    director.UpdateSpatialSfxVoice(voice, spatial, source_id, 1.25f);
    EXPECT_EQ(backend.ParameterCallCount(), 1u);
    EXPECT_NEAR(backend.LastVolume(), 0.5f, 1e-4f);
    EXPECT_NEAR(backend.LastPitch(), 1.25f, 1e-4f);
}

ACS_TEST(SpatialAudioVoice, RejectsMissingBackendInvalidVoiceAndRemovedSource)
{
    CAudioDirector director;
    CSpatialAudio spatial;
    const u32 source_id = spatial.RegisterSource(
        {0.0f, 0.0f, 5.0f}, 10.0f, EAttenuationCurve::Linear);

    director.UpdateSpatialSfxVoice(
        FAudioVoiceHandle::FromPackedValue(9u), spatial, source_id);

    CRecordingAudioBackend backend;
    EXPECT_TRUE(backend.Init(4u).IsOk());
    director.SetBackend(&backend);
    const FAudioVoiceHandle voice = director.PlaySfxClip(MakeClip());
    EXPECT_TRUE(voice.IsValid());

    backend.ResetParameterRecords();
    director.UpdateSpatialSfxVoice(kInvalidAudioVoice, spatial, source_id);
    EXPECT_EQ(backend.ParameterCallCount(), 0u);

    spatial.RemoveSource(source_id);
    backend.ResetParameterRecords();
    director.UpdateSpatialSfxVoice(voice, spatial, source_id);
    EXPECT_EQ(backend.ParameterCallCount(), 0u);
}

ACS_TEST(SpatialAudioVoice, StopsVoiceBeforeRemovingSourceExactlyOnce)
{
    CRecordingAudioBackend backend;
    EXPECT_TRUE(backend.Init(4u).IsOk());
    CAudioDirector director;
    director.SetBackend(&backend);
    const FAudioVoiceHandle voice = director.PlaySfxClip(MakeClip());
    EXPECT_TRUE(voice.IsValid());

    CSpatialAudio spatial;
    const u32 source_id = spatial.RegisterSource(
        {0.0f, 0.0f, 5.0f}, 10.0f, EAttenuationCurve::Linear);

    backend.StopVoice(voice);
    EXPECT_EQ(backend.StopVoiceCount(), 1u);
    EXPECT_EQ(backend.ActiveVoiceCount(), 0u);
    spatial.RemoveSource(source_id);
    backend.ResetParameterRecords();
    director.UpdateSpatialSfxVoice(voice, spatial, source_id);
    EXPECT_EQ(backend.ParameterCallCount(), 0u);
    EXPECT_EQ(backend.StopVoiceCount(), 1u);
}

ACS_TEST(SpatialAudioVoice, StopsVoiceBeforeClearingSourcesExactlyOnce)
{
    CRecordingAudioBackend backend;
    EXPECT_TRUE(backend.Init(4u).IsOk());
    CAudioDirector director;
    director.SetBackend(&backend);
    const FAudioVoiceHandle voice = director.PlaySfxClip(MakeClip());
    EXPECT_TRUE(voice.IsValid());

    CSpatialAudio spatial;
    const u32 source_id = spatial.RegisterSource(
        {0.0f, 0.0f, 5.0f}, 10.0f, EAttenuationCurve::Linear);

    backend.ResetParameterRecords();
    const u32 parameter_calls_before = backend.ParameterCallCount();
    backend.StopVoice(voice);
    EXPECT_EQ(backend.StopVoiceCount(), 1u);
    EXPECT_EQ(backend.ActiveVoiceCount(), 0u);
    spatial.Clear();
    EXPECT_EQ(spatial.SourceCount(), 0u);

    director.UpdateSpatialSfxVoice(voice, spatial, source_id);
    EXPECT_EQ(backend.ParameterCallCount(), parameter_calls_before);
    EXPECT_EQ(backend.StopVoiceCount(), 1u);
}

ACS_TEST(SpatialAudioVoice, RemovedSourceDoesNotStopActiveVoice)
{
    CRecordingAudioBackend backend;
    EXPECT_TRUE(backend.Init(4u).IsOk());
    CAudioDirector director;
    director.SetBackend(&backend);
    const FAudioVoiceHandle voice = director.PlaySfxClip(MakeClip());
    EXPECT_TRUE(voice.IsValid());

    CSpatialAudio spatial;
    const u32 source_id = spatial.RegisterSource(
        {0.0f, 0.0f, 5.0f}, 10.0f, EAttenuationCurve::Linear);

    spatial.RemoveSource(source_id);
    backend.ResetParameterRecords();
    director.UpdateSpatialSfxVoice(voice, spatial, source_id);
    EXPECT_EQ(backend.ParameterCallCount(), 0u);
    EXPECT_EQ(backend.StopVoiceCount(), 0u);
    EXPECT_EQ(backend.ActiveVoiceCount(), 1u);
}

ACS_TEST(SpatialAudioVoice, LeavesValidLookingStaleVoiceUntouched)
{
    CRecordingAudioBackend backend;
    EXPECT_TRUE(backend.Init(4u).IsOk());
    CAudioDirector director;
    director.SetBackend(&backend);
    const FAudioVoiceHandle voice = director.PlaySfxClip(MakeClip());
    EXPECT_TRUE(voice.IsValid());

    CSpatialAudio spatial;
    const u32 source_id = spatial.RegisterSource(
        {0.0f, 0.0f, 5.0f}, 10.0f, EAttenuationCurve::Linear);
    backend.StopVoice(voice);

    backend.ResetParameterRecords();
    director.UpdateSpatialSfxVoice(voice, spatial, source_id);
    EXPECT_EQ(backend.ParameterCallCount(), 1u);
    EXPECT_EQ(backend.AppliedParameterCount(), 0u);
}

ACS_TEST(SpatialAudioVoice, NormalizesNonFiniteVolumePanAndPitch)
{
    CRecordingAudioBackend backend;
    EXPECT_TRUE(backend.Init(4u).IsOk());
    CAudioDirector director;
    director.SetBackend(&backend);
    const FAudioVoiceHandle voice = director.PlaySfxClip(MakeClip());
    EXPECT_TRUE(voice.IsValid());

    CSpatialAudio spatial;
    const u32 source_id = spatial.RegisterSource(
        {0.0f, 0.0f, 5.0f}, 10.0f, EAttenuationCurve::Linear);

    const f32 not_a_number = std::numeric_limits<f32>::quiet_NaN();
    spatial.SetSourceVolume(source_id, not_a_number);
    backend.ResetParameterRecords();
    director.UpdateSpatialSfxVoice(voice, spatial, source_id, 1.0f);
    EXPECT_EQ(backend.ParameterCallCount(), 1u);
    EXPECT_EQ(backend.AppliedParameterCount(), 1u);
    EXPECT_NEAR(backend.LastVolume(), 0.0f, 1e-4f);
    EXPECT_NEAR(backend.LastPan(), 0.0f, 1e-4f);

    spatial.SetSourceVolume(source_id, 1.0f);
    spatial.UpdateSource(source_id, {not_a_number, 0.0f, 0.0f});
    backend.ResetParameterRecords();
    director.UpdateSpatialSfxVoice(voice, spatial, source_id, 1.0f);
    EXPECT_EQ(backend.ParameterCallCount(), 1u);
    EXPECT_NEAR(backend.LastPan(), 0.0f, 1e-4f);

    spatial.UpdateSource(source_id, {5.0f, 0.0f, 0.0f});
    backend.ResetParameterRecords();
    director.UpdateSpatialSfxVoice(voice, spatial, source_id, not_a_number);
    EXPECT_EQ(backend.ParameterCallCount(), 1u);
    EXPECT_NEAR(backend.LastPitch(), 1.0f, 1e-4f);

    backend.ResetParameterRecords();
    director.UpdateSpatialSfxVoice(voice, spatial, source_id, 0.0f);
    EXPECT_NEAR(backend.LastPitch(), 0.25f, 1e-4f);

    backend.ResetParameterRecords();
    director.UpdateSpatialSfxVoice(voice, spatial, source_id, 8.0f);
    EXPECT_NEAR(backend.LastPitch(), 4.0f, 1e-4f);
}

ACS_TEST(SpatialAudio, NormalizesNonFinitePublicInputsAndReturnsFiniteRanges)
{
    const f32 not_a_number = std::numeric_limits<f32>::quiet_NaN();
    const f32 positive_infinity = std::numeric_limits<f32>::infinity();
    const f32 negative_infinity = -positive_infinity;
    const FVec3 invalid_position{not_a_number, 2.0f, negative_infinity};
    const FVec3 invalid_velocity{positive_infinity, not_a_number, negative_infinity};

    CSpatialAudio spatial;
    FAudioListener listener;
    listener.position = invalid_position;
    listener.forward = {not_a_number, 0.0f, positive_infinity};
    listener.up = {0.0f, negative_infinity, not_a_number};
    spatial.SetListener(listener);

    const FAudioListener& finite_listener = spatial.GetListener();
    EXPECT_NEAR(finite_listener.position.x, 0.0f, 1e-4f);
    EXPECT_NEAR(finite_listener.position.y, 2.0f, 1e-4f);
    EXPECT_NEAR(finite_listener.position.z, 0.0f, 1e-4f);
    EXPECT_NEAR(finite_listener.forward.x, 0.0f, 1e-4f);
    EXPECT_NEAR(finite_listener.forward.z, 0.0f, 1e-4f);
    EXPECT_NEAR(finite_listener.up.y, 0.0f, 1e-4f);
    EXPECT_NEAR(finite_listener.up.z, 0.0f, 1e-4f);

    const u32 source_id = spatial.RegisterSource(invalid_position, positive_infinity, EAttenuationCurve::Linear);
    EXPECT_NEAR(spatial.ComputeAttenuatedVolume(source_id), 1.0f, 1e-4f);
    EXPECT_NEAR(spatial.ComputePan(source_id), 0.0f, 1e-4f);

    spatial.SetSourceVolume(source_id, not_a_number);
    EXPECT_NEAR(spatial.ComputeAttenuatedVolume(source_id), 0.0f, 1e-4f);
    spatial.SetSourceVolume(source_id, positive_infinity);
    EXPECT_NEAR(spatial.ComputeAttenuatedVolume(source_id), 0.0f, 1e-4f);
    spatial.SetSourceVolume(source_id, negative_infinity);
    EXPECT_NEAR(spatial.ComputeAttenuatedVolume(source_id), 0.0f, 1e-4f);

    spatial.SetSourceVolume(source_id, 1.0f);
    const FVec3 updated_position{not_a_number, 3.0f, positive_infinity};
    spatial.UpdateSource(source_id, updated_position, invalid_velocity);
    const f32 volume = spatial.ComputeAttenuatedVolume(source_id);
    const f32 pan = spatial.ComputePan(source_id);
    EXPECT_TRUE(std::isfinite(volume));
    EXPECT_TRUE(volume >= 0.0f && volume <= 1.0f);
    EXPECT_NEAR(volume, 0.95f, 1e-4f);
    EXPECT_TRUE(std::isfinite(pan));
    EXPECT_TRUE(pan >= -1.0f && pan <= 1.0f);
}

ACS_TEST(SpatialAudio, HasSourceIsPublicAndRejectsStaleIds)
{
    CSpatialAudio spatial;
    EXPECT_FALSE(spatial.HasSource(0u));
    EXPECT_FALSE(spatial.HasSource(999u));

    const u32 first_source = spatial.RegisterSource(
        {0.0f, 0.0f, 1.0f}, 10.0f, EAttenuationCurve::Linear);
    EXPECT_TRUE(spatial.HasSource(first_source));
    spatial.RemoveSource(first_source);
    EXPECT_FALSE(spatial.HasSource(first_source));

    const u32 second_source = spatial.RegisterSource(
        {0.0f, 0.0f, 2.0f}, 10.0f, EAttenuationCurve::Linear);
    EXPECT_TRUE(spatial.HasSource(second_source));
    spatial.Clear();
    EXPECT_FALSE(spatial.HasSource(second_source));
}

ACS_TEST(SpatialAudioContract, PublicSourceQueryDoesNotUseDirectorFriend)
{
    const std::string header = ReadAudioWorkspaceSource("src/gameframework/SpatialAudio.h");
    EXPECT_FALSE(header.empty());
    EXPECT_EQ(header.find("friend class CAudioDirector"), std::string::npos);

    const std::size_t class_begin = header.find("class CSpatialAudio");
    const std::size_t public_begin = header.find("public:", class_begin);
    const std::size_t query = header.find("bool HasSource(u32 id) const noexcept;", public_begin);
    const std::size_t private_begin = header.find("private:", public_begin);
    EXPECT_NE(class_begin, std::string::npos);
    EXPECT_NE(public_begin, std::string::npos);
    EXPECT_NE(query, std::string::npos);
    EXPECT_TRUE(query < private_begin);
}

ACS_TEST(SpatialAudio, KeepsFiniteReturnRangesWhenVectorMathOverflows)
{
    CSpatialAudio spatial;
    const f32 largest = std::numeric_limits<f32>::max();
    const u32 source_id = spatial.RegisterSource({largest, largest, largest}, 20.0f, EAttenuationCurve::Linear);

    const f32 volume = spatial.ComputeAttenuatedVolume(source_id);
    const f32 pan = spatial.ComputePan(source_id);
    EXPECT_TRUE(std::isfinite(volume));
    EXPECT_TRUE(volume >= 0.0f && volume <= 1.0f);
    EXPECT_TRUE(std::isfinite(pan));
    EXPECT_TRUE(pan >= -1.0f && pan <= 1.0f);
}
