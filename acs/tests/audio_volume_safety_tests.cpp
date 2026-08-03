// SPDX-License-Identifier: Apache-2.0
#include "audio/AudioEngine.h"
#include "foundation/Limits.h"
#include "foundation/Move.h"

#include <type_traits>

using namespace acs;

namespace {

/** 公開音量操作の正規化とbackend失敗時の非変更契約を一度検証する。 */
i32 RunAudioVolumeSafetyTest() noexcept
{
    static_assert(std::is_same_v<FAudioEngine, CAudioEngine>);
#if defined(_WIN64)
    static_assert(sizeof(CAudioEngine) == 24u && alignof(CAudioEngine) == 8u);
#endif

    /** テストに使う正の無限大。 */
    const f32 PositiveInfinity = TNumLimits<f32>::Infinity();

    /** 実行時にNaNを作るための無限大。 */
    volatile f32 RuntimeInfinity = PositiveInfinity;

    /** 数ではない音量入力。 */
    const f32 NotANumber = RuntimeInfinity - RuntimeInfinity;

    if (CAudioEngine::NormalizeVolumeForTesting(-1.0f) != 0.0f || CAudioEngine::NormalizeVolumeForTesting(0.25f) != 0.25f || CAudioEngine::NormalizeVolumeForTesting(2.0f) != 1.0f || CAudioEngine::NormalizeVolumeForTesting(PositiveInfinity) != 0.0f || CAudioEngine::NormalizeVolumeForTesting(-PositiveInfinity) != 0.0f || CAudioEngine::NormalizeVolumeForTesting(NotANumber) != 0.0f)
    {
        return 1;
    }

    /** 最小構成の16bit PCM音声データ。 */
    TArray<byte> Samples;
    if (!Samples.TrySetNum(4u))
    {
        return 2;
    }
    Samples[0] = 0u;
    Samples[1] = 0u;
    Samples[2] = 0u;
    Samples[3] = 0u;

    /** 公開Play経路へ渡す短い音声アセット。 */
    AAudioAsset Asset(48000u, 1u, ESampleFormat::PCM_S16, 2u, Move(Samples));

    /** 実音声機器を使わず公開操作を実行する音声エンジン。 */
    CAudioEngine Engine;
    if (Engine.InitializeVolumeTestState().IsErr())
    {
        return 3;
    }

    /** NaNを0へ正規化して開始した再生。 */
    const FSoundHandle Handle = Engine.Play(Asset, NotANumber, false);
    if (!Handle.IsValid() || Engine.ActiveCount() != 1u || Engine.AllocatedVoiceCountForTesting() != 1u || Engine.VolumeForTesting(Handle) != 0.0f || Engine.LastVolumeAttemptForTesting() != 0.0f || Engine.ResidentBufferBytesForTesting() == 0u)
    {
        return 4;
    }

    Engine.SetVolume(Handle, 2.0f);
    if (Engine.VolumeForTesting(Handle) != 1.0f || Engine.LastVolumeAttemptForTesting() != 1.0f)
    {
        return 5;
    }

    Engine.SetVolume(Handle, 0.25f);
    Engine.ConfigureVolumeFailuresForTesting(false, true, false);
    Engine.SetVolume(Handle, 0.75f);
    if (Engine.VolumeForTesting(Handle) != 0.25f || Engine.LastVolumeAttemptForTesting() != 0.75f || Engine.VolumeFailureWarningCountForTesting() != 1u)
    {
        return 6;
    }

    Engine.ConfigureVolumeFailuresForTesting(false, false, false);
    Engine.SetMasterVolume(0.5f);
    Engine.ConfigureVolumeFailuresForTesting(false, false, true);
    Engine.SetMasterVolume(-PositiveInfinity);
    if (Engine.MasterVolumeForTesting() != 0.5f || Engine.LastVolumeAttemptForTesting() != 0.0f || Engine.VolumeFailureWarningCountForTesting() != 2u)
    {
        return 7;
    }

    Engine.ConfigureVolumeFailuresForTesting(false, false, false);
    Engine.SetMasterVolume(NotANumber);
    if (Engine.MasterVolumeForTesting() != 0.0f || Engine.LastVolumeAttemptForTesting() != 0.0f)
    {
        return 8;
    }

    Engine.Stop(Handle);
    if (Engine.ActiveCount() != 0u || Engine.AllocatedVoiceCountForTesting() != 0u || Engine.ResidentBufferBytesForTesting() != 0u)
    {
        return 9;
    }

    Engine.ConfigureVolumeFailuresForTesting(true, false, false);

    /** backend音量設定を拒否させた再生結果。 */
    const FSoundHandle FailedHandle = Engine.Play(Asset, PositiveInfinity, false);
    if (FailedHandle != kInvalidSound || Engine.LastVolumeAttemptForTesting() != 0.0f || Engine.ActiveCount() != 0u || Engine.AllocatedVoiceCountForTesting() != 0u || Engine.ResidentBufferBytesForTesting() != 0u)
    {
        return 10;
    }

    Engine.ConfigureVolumeFailuresForTesting(false, false, false);

    /** 失敗後に同じ発音枠を再利用できることを確認する再生。 */
    const FSoundHandle RecoveryHandle = Engine.Play(Asset, -1.0f, false);
    if (!RecoveryHandle.IsValid() || Engine.AllocatedVoiceCountForTesting() != 1u || Engine.VolumeForTesting(RecoveryHandle) != 0.0f)
    {
        return 11;
    }
    Engine.StopAll();
    if (Engine.ActiveCount() != 0u || Engine.AllocatedVoiceCountForTesting() != 0u || Engine.ResidentBufferBytesForTesting() != 0u)
    {
        return 12;
    }

    Engine.Shutdown();
    if (Engine.HasLifecycleStateForTesting())
    {
        return 13;
    }
    return 0;
}

} // namespace

/** 音量安全契約の検証結果を終了コードで返す。 */
int main()
{
    return RunAudioVolumeSafetyTest();
}
