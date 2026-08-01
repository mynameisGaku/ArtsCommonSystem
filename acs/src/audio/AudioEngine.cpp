// SPDX-License-Identifier: Apache-2.0
// XAudio2 ベースの音声エンジン実装
#include "audio/AudioEngine.h"
#include "foundation/Assert.h"
#include "foundation/Platform.h"
#include "foundation/Log.h"
#include "threading/ScopedLock.h"
#if defined(ACS_AUDIO_TEST_HOOKS)
#include "threading/Atomic.h"
#include "threading/Thread.h"
#endif

#include <cmath>
#include <combaseapi.h>
#include <new>
#include <xaudio2.h>

namespace acs {

namespace {

/** 同時に確保できる発音スロット (ソースボイス) の上限。 */
constexpr u32 kMaxVoices = 64;

/**
 * 音量を音声APIへ渡せる安全な範囲へ収める。
 * @param Volume 利用者から受け取った音量。
 * @return 非有限値は0、有限値は0から1へ収めた値。
 */
f32 NormalizeAudioVolume(f32 Volume) noexcept
{
    if (!std::isfinite(Volume) || Volume <= 0.0f)
    {
        return 0.0f;
    }
    if (Volume >= 1.0f)
    {
        return 1.0f;
    }
    return Volume;
}

/**
 * 1 個の発音スロット (XAudio2 ソースボイス + 再生用データ)。
 */
struct FVoiceSlot {
    /** XAudio2 ソースボイス (未使用時は nullptr)。 */
    IXAudio2SourceVoice* Voice = nullptr;

    /** 再生中に参照され続けるサンプルデータのコピー。 */
    TArray<byte> BufferCopy;

    /** スロットの世代 (解放のたびに加算し、古いハンドルを無効化)。 */
    u32 Generation = 0;

    /** 常駐 PCM 予算へ計上した確保容量。 */
    u64 ReservedBufferBytes = 0u;

    /** このスロットが現在使用中か。 */
    bool bInUse = false;

#if defined(ACS_AUDIO_TEST_HOOKS)
    /** テストbackendが保持する個別音量。 */
    f32 TestVolume = 1.0f;

    /** テストbackend上で発音ボイスを確保しているか。 */
    bool bTestVoiceCreated = false;
#endif
};

u64 DestroySlot(FVoiceSlot& Slot) noexcept;

/** Shutdown 要求数を関数終了まで保持し、新しい共有操作の開始を閉じる。 */
class FScopedAudioShutdownRequest final {
public:
    explicit FScopedAudioShutdownRequest(TAtomic<u32>& Requests) noexcept
        : m_Requests(Requests)
    {
        m_Requests.FetchAdd(1u);
    }

    ~FScopedAudioShutdownRequest() noexcept
    {
        m_Requests.FetchSub(1u);
    }

    FScopedAudioShutdownRequest(const FScopedAudioShutdownRequest&) = delete;
    FScopedAudioShutdownRequest& operator=(const FScopedAudioShutdownRequest&) = delete;

private:
    TAtomic<u32>& m_Requests;
};

#if defined(ACS_AUDIO_TEST_HOOKS)
TAtomic<TAtomic<u32>*> g_AudioLifecycleTestEntered{nullptr};
TAtomic<TAtomic<u32>*> g_AudioLifecycleTestRelease{nullptr};

void WaitForAudioLifecycleTestGate() noexcept
{
    TAtomic<u32>* Entered = g_AudioLifecycleTestEntered.Load(EMemoryOrder::Acquire);
    TAtomic<u32>* Release = g_AudioLifecycleTestRelease.Load(EMemoryOrder::Acquire);
    if (Entered == nullptr || Release == nullptr)
    {
        return;
    }

    Entered->Store(1u, EMemoryOrder::Release);
    while (Release->Load(EMemoryOrder::Acquire) == 0u)
    {
        Yield();
    }
}
#endif

} // namespace

/**
 * CAudioEngine の pimpl 実装 (XAudio2 ハンドルとスロット表を保持)。
 */
struct CAudioEngine::FImpl {
    /** XAudio2 エンジン本体。 */
    IXAudio2* XAudio2 = nullptr;

    /** 最終出力のマスタリングボイス。 */
    IXAudio2MasteringVoice* MasteringVoice = nullptr;

    /** MTA の利用参照を任意スレッドから解除するための COM cookie。 */
    CO_MTA_USAGE_COOKIE MtaUsageCookie = nullptr;

    /** CoIncrementMTAUsage が成功し、解除すべき参照を保持しているか。 */
    bool bMtaUsageAcquired = false;

#if defined(ACS_AUDIO_TEST_HOOKS)
    /** テスト用の疑似 MTA 参照で、実 CoDecrementMTAUsage を呼ばない。 */
    bool bTestMtaUsage = false;

    /** 実音声機器の代わりに決定的なテストbackendを使うか。 */
    bool bVolumeTestBackend = false;

    /** 再生開始時の個別音量設定を拒否するか。 */
    bool bFailPlayVolume = false;

    /** 再生中の個別音量設定を拒否するか。 */
    bool bFailSetVolume = false;

    /** 全体音量設定を拒否するか。 */
    bool bFailMasterVolume = false;

    /** テストbackendが保持する全体音量。 */
    f32 TestMasterVolume = 1.0f;

    /** テストbackendが最後に受け取った正規化済み音量。 */
    f32 LastVolumeAttempt = 1.0f;

    /** void音量操作が失敗して警告した回数。 */
    u32 VolumeFailureWarningCount = 0u;
#endif

    /** スロット表とアクティブ数を保護する mutex。 */
    FMutex StateMutex;

    /** 発音スロット表 (最大 kMaxVoices 個)。 */
    FVoiceSlot Slots[kMaxVoices] {};

    /** 使用中スロット数。 */
    u32 ActiveVoiceCount = 0;

    /** 全スロットの PCM buffer 確保容量合計。 */
    u64 ResidentBufferBytes = 0u;
};

namespace {

/**
 * 再生用backendが利用可能かを返す。
 * @param Implementation 確認する音声エンジンの内部状態。
 * @return 実backendまたはテストbackendを使える場合はtrue。
 */
bool HasPlaybackBackend(const CAudioEngine::FImpl& Implementation) noexcept
{
#if defined(ACS_AUDIO_TEST_HOOKS)
    if (Implementation.bVolumeTestBackend)
    {
        return true;
    }
#endif
    return Implementation.XAudio2 != nullptr;
}

/**
 * 音声アセット用の発音ボイスを作る。
 * @param Implementation 操作する音声エンジンの内部状態。
 * @param Slot 作成したボイスを保持する発音枠。
 * @param WaveFormat 音声データの形式。
 * @return 音声APIの結果。
 */
HRESULT CreateSourceVoice(CAudioEngine::FImpl& Implementation, FVoiceSlot& Slot, const WAVEFORMATEX& WaveFormat) noexcept
{
#if defined(ACS_AUDIO_TEST_HOOKS)
    if (Implementation.bVolumeTestBackend)
    {
        Slot.bTestVoiceCreated = true;
        return S_OK;
    }
#endif
    if (Implementation.XAudio2 == nullptr)
    {
        return E_POINTER;
    }
    return Implementation.XAudio2->CreateSourceVoice(&Slot.Voice, &WaveFormat);
}

/**
 * 発音ボイスが作成済みかを返す。
 * @param Implementation 操作する音声エンジンの内部状態。
 * @param Slot 確認する発音枠。
 * @return 実ボイスまたはテスト用ボイスが存在する場合はtrue。
 */
bool HasSourceVoice(const CAudioEngine::FImpl& Implementation, const FVoiceSlot& Slot) noexcept
{
#if defined(ACS_AUDIO_TEST_HOOKS)
    if (Implementation.bVolumeTestBackend)
    {
        return Slot.bTestVoiceCreated;
    }
#else
    (void)Implementation;
#endif
    return Slot.Voice != nullptr;
}

/**
 * 再生用データを発音ボイスへ渡す。
 * @param Implementation 操作する音声エンジンの内部状態。
 * @param Slot 再生に使う発音枠。
 * @param Buffer 再生用データの参照情報。
 * @return 音声APIの結果。
 */
HRESULT SubmitSourceBuffer(CAudioEngine::FImpl& Implementation, FVoiceSlot& Slot, const XAUDIO2_BUFFER& Buffer) noexcept
{
#if defined(ACS_AUDIO_TEST_HOOKS)
    if (Implementation.bVolumeTestBackend)
    {
        return S_OK;
    }
#else
    (void)Implementation;
#endif
    if (Slot.Voice == nullptr)
    {
        return E_POINTER;
    }
    return Slot.Voice->SubmitSourceBuffer(&Buffer);
}

/**
 * 個別音量を発音ボイスへ設定する。
 * @param Implementation 操作する音声エンジンの内部状態。
 * @param Slot 音量を変える発音枠。
 * @param Volume 正規化済み音量。
 * @param bPlaySetup 再生開始中の設定ならtrue。
 * @return 音声APIの結果。
 */
HRESULT SetSourceVolume(CAudioEngine::FImpl& Implementation, FVoiceSlot& Slot, f32 Volume, bool bPlaySetup) noexcept
{
#if defined(ACS_AUDIO_TEST_HOOKS)
    if (Implementation.bVolumeTestBackend)
    {
        Implementation.LastVolumeAttempt = Volume;
        if ((bPlaySetup && Implementation.bFailPlayVolume) || (!bPlaySetup && Implementation.bFailSetVolume))
        {
            return E_FAIL;
        }
        Slot.TestVolume = Volume;
        return S_OK;
    }
#else
    (void)Implementation;
    (void)bPlaySetup;
#endif
    if (Slot.Voice == nullptr)
    {
        return E_POINTER;
    }
    return Slot.Voice->SetVolume(Volume);
}

/**
 * 発音ボイスの再生を開始する。
 * @param Implementation 操作する音声エンジンの内部状態。
 * @param Slot 再生を開始する発音枠。
 * @return 音声APIの結果。
 */
HRESULT StartSourceVoice(CAudioEngine::FImpl& Implementation, FVoiceSlot& Slot) noexcept
{
#if defined(ACS_AUDIO_TEST_HOOKS)
    if (Implementation.bVolumeTestBackend)
    {
        return S_OK;
    }
#else
    (void)Implementation;
#endif
    if (Slot.Voice == nullptr)
    {
        return E_POINTER;
    }
    return Slot.Voice->Start(0);
}

/**
 * 全体音量を最終出力へ設定する。
 * @param Implementation 操作する音声エンジンの内部状態。
 * @param Volume 正規化済み音量。
 * @return 音声APIの結果。
 */
HRESULT ApplyMasterVolume(CAudioEngine::FImpl& Implementation, f32 Volume) noexcept
{
#if defined(ACS_AUDIO_TEST_HOOKS)
    if (Implementation.bVolumeTestBackend)
    {
        Implementation.LastVolumeAttempt = Volume;
        if (Implementation.bFailMasterVolume)
        {
            return E_FAIL;
        }
        Implementation.TestMasterVolume = Volume;
        return S_OK;
    }
#endif
    if (Implementation.MasteringVoice == nullptr)
    {
        return E_POINTER;
    }
    return Implementation.MasteringVoice->SetVolume(Volume);
}

/**
 * void音量操作の失敗警告をテスト診断へ記録する。
 * @param Implementation 操作する音声エンジンの内部状態。
 */
void RecordVolumeFailureWarning(CAudioEngine::FImpl& Implementation) noexcept
{
#if defined(ACS_AUDIO_TEST_HOOKS)
    if (Implementation.bVolumeTestBackend)
    {
        ++Implementation.VolumeFailureWarningCount;
    }
#else
    (void)Implementation;
#endif
}

/** 全スロットを停止して active count を 0 に戻す。 */
void StopAllSlots(CAudioEngine::FImpl& Implementation) noexcept
{
    FScopedLock Lock(Implementation.StateMutex);
    for (u32 Index = 0; Index < kMaxVoices; ++Index)
    {
        if (Implementation.Slots[Index].bInUse)
        {
            (void)DestroySlot(Implementation.Slots[Index]);
        }
    }
    Implementation.ActiveVoiceCount = 0;
    Implementation.ResidentBufferBytes = 0u;
}

} // namespace

CAudioEngine::~CAudioEngine() noexcept
{
    Shutdown();
}

bool CAudioEngine::IsShutdownRequested() const noexcept
{
    return m_ShutdownRequests.Load(EMemoryOrder::Acquire) != 0u;
}

TResult<void> CAudioEngine::Init() noexcept
{
    if (IsShutdownRequested())
    {
        return ACS_ERR(Generic, 5, "CAudioEngine shutdown is in progress");
    }

    FScopedExclusiveLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested())
    {
        return ACS_ERR(Generic, 5, "CAudioEngine shutdown is in progress");
    }
    if (m_Impl)
    {
        return ACS_ERR(Generic, 1, "CAudioEngine already initialized");
    }
    m_Impl = new (std::nothrow) FImpl();
    if (m_Impl == nullptr)
    {
        return ACS_ERR(Memory, 6, "CAudioEngine state allocation failed");
    }

    // MTA 利用参照は cookie で保持し、Shutdown を呼ぶスレッドに依存させない。
    HRESULT Result = ::CoIncrementMTAUsage(&m_Impl->MtaUsageCookie);
    if (FAILED(Result))
    {
        ShutdownUnlocked();
        return ACS_ERR_OS(Generic, 4, "CoIncrementMTAUsage failed", static_cast<u32>(Result));
    }
    m_Impl->bMtaUsageAcquired = true;

    // XAudio2 エンジン作成
    Result = ::XAudio2Create(&m_Impl->XAudio2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(Result))
    {
        ShutdownUnlocked();
        return ACS_ERR_OS(Generic, 2, "XAudio2Create failed", static_cast<u32>(Result));
    }

    // マスタリングボイス（最終出力）
    Result = m_Impl->XAudio2->CreateMasteringVoice(&m_Impl->MasteringVoice);
    if (FAILED(Result))
    {
        ShutdownUnlocked();
        return ACS_ERR_OS(Generic, 3, "CreateMasteringVoice failed", static_cast<u32>(Result));
    }

    ACS_LOG_INFO("CAudioEngine initialized (XAudio2)");
    return Ok();
}

void CAudioEngine::Shutdown() noexcept
{
    FScopedAudioShutdownRequest ShutdownRequest(m_ShutdownRequests);
    FScopedExclusiveLock LifecycleLock(m_LifecycleLock);
    if (!m_Impl)
    {
        return;
    }

    ShutdownUnlocked();
}

void CAudioEngine::ShutdownUnlocked() noexcept
{
    StopAllSlots(*m_Impl);
    if (m_Impl->MasteringVoice)
    {
        m_Impl->MasteringVoice->DestroyVoice();
        m_Impl->MasteringVoice = nullptr;
    }
    if (m_Impl->XAudio2)
    {
        m_Impl->XAudio2->Release();
        m_Impl->XAudio2 = nullptr;
    }
    if (m_Impl->bMtaUsageAcquired)
    {
        HRESULT Result = S_OK;
#if defined(ACS_AUDIO_TEST_HOOKS)
        if (!m_Impl->bTestMtaUsage)
        {
            Result = ::CoDecrementMTAUsage(m_Impl->MtaUsageCookie);
        }
#else
        Result = ::CoDecrementMTAUsage(m_Impl->MtaUsageCookie);
#endif
        if (FAILED(Result))
        {
            ACS_LOG_ERROR("CAudioEngine::Shutdown: CoDecrementMTAUsage failed (hr=0x%08lx)", static_cast<unsigned long>(Result));
            ACS_ASSERT(false && "CAudioEngine MTA usage release failed");
        }
        m_Impl->MtaUsageCookie = nullptr;
        m_Impl->bMtaUsageAcquired = false;
    }
    delete m_Impl;
    m_Impl = nullptr;
}

namespace {
/**
 * 再生終了した一発再生スロットを回収しつつ、空きスロット番号を探す。
 *
 * @details
 * まず BuffersQueued==0 になった一発再生ボイスを破棄して空きに戻す
 * (これを怠ると kMaxVoices 回再生した時点でスロットが尽きる)。ループ再生は
 * BuffersQueued が 0 にならないため回収対象外。
 * @param Implementation 対象エンジンの pimpl。
 * @return 使える空きスロット番号 (満杯なら 0xFFFFFFFF)。
 */
u32 FindFreeSlot(CAudioEngine::FImpl& Implementation) noexcept
{
    // 自然に再生が終わった一発再生ボイスを回収する。これをしないと、
    // 一発再生を kMaxVoices 回呼んだ時点でスロットが尽き、以降の Play が
    // 無音になる。ループ再生は BuffersQueued が 0 にならず回収されない。
    for (u32 Index = 0; Index < kMaxVoices; ++Index)
    {
        FVoiceSlot& Slot = Implementation.Slots[Index];
        if (Slot.bInUse && Slot.Voice)
        {
            XAUDIO2_VOICE_STATE State{};
            Slot.Voice->GetState(&State);
            if (State.BuffersQueued == 0)
            {
                const u64 ReleasedBufferBytes = DestroySlot(Slot);
                ACS_ASSERT(Implementation.ResidentBufferBytes >= ReleasedBufferBytes);
                if (Implementation.ResidentBufferBytes >= ReleasedBufferBytes)
                {
                    Implementation.ResidentBufferBytes -= ReleasedBufferBytes;
                }
                else
                {
                    Implementation.ResidentBufferBytes = 0u;
                }
                if (Implementation.ActiveVoiceCount > 0)
                {
                    --Implementation.ActiveVoiceCount;
                }
            }
        }
    }
    for (u32 Index = 0; Index < kMaxVoices; ++Index)
    {
        if (!Implementation.Slots[Index].bInUse)
        {
            return Index;
        }
    }
    return 0xFFFFFFFFu;
}
} // namespace

FSoundHandle CAudioEngine::Play(const AAudioAsset& Asset, f32 Volume, bool bLoop) noexcept
{
    if (IsShutdownRequested())
    {
        return kInvalidSound;
    }

    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested())
    {
        return kInvalidSound;
    }
    FImpl* const Implementation = m_Impl;
    if (Implementation == nullptr || !HasPlaybackBackend(*Implementation) || Asset.SampleByteCount() == 0 || Asset.SampleByteCount() > 0xFFFFFFFFu)
    {
        return kInvalidSound;
    }

    FScopedLock StateLock(Implementation->StateMutex);

    const u32 Index = FindFreeSlot(*Implementation);
    if (Index == 0xFFFFFFFFu)
    {
        return kInvalidSound;
    }
    FVoiceSlot& Slot = Implementation->Slots[Index];

    if (Implementation->ResidentBufferBytes > kAudioEngineResidentBufferBudgetBytes ||
        static_cast<u64>(Asset.SampleByteCount()) >
            kAudioEngineResidentBufferBudgetBytes - Implementation->ResidentBufferBytes)
    {
        return kInvalidSound;
    }

    // ソースボイスのフォーマット設定
    WAVEFORMATEX WaveFormat{};
    WaveFormat.wFormatTag =
        (Asset.Format() == ESampleFormat::PCM_F32) ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    WaveFormat.nChannels = Asset.Channels();
    WaveFormat.nSamplesPerSec = Asset.SampleRate();
    WaveFormat.wBitsPerSample = (Asset.Format() == ESampleFormat::PCM_F32) ? 32 : 16;
    WaveFormat.nBlockAlign = (WaveFormat.nChannels * WaveFormat.wBitsPerSample) / 8;
    WaveFormat.nAvgBytesPerSec = WaveFormat.nSamplesPerSec * WaveFormat.nBlockAlign;
    WaveFormat.cbSize = 0;

    HRESULT Result = CreateSourceVoice(*Implementation, Slot, WaveFormat);
    if (FAILED(Result) || !HasSourceVoice(*Implementation, Slot))
    {
        if (Slot.Voice != nullptr)
        {
            DestroySlot(Slot);
        }
        return kInvalidSound;
    }

    // サンプルデータを再生中保持するためコピー
    if (!Slot.BufferCopy.TryResize(Asset.SampleByteCount()))
    {
        DestroySlot(Slot);
        return kInvalidSound;
    }
    const u64 ReservedBufferBytes = static_cast<u64>(Slot.BufferCopy.Capacity());
    if (ReservedBufferBytes >
        kAudioEngineResidentBufferBudgetBytes - Implementation->ResidentBufferBytes)
    {
        (void)DestroySlot(Slot);
        return kInvalidSound;
    }
    for (usize SampleIndex = 0; SampleIndex < Asset.SampleByteCount(); ++SampleIndex)
    {
        Slot.BufferCopy[SampleIndex] = Asset.Samples()[SampleIndex];
    }

    XAUDIO2_BUFFER Buffer{};
    Buffer.AudioBytes = static_cast<UINT32>(Slot.BufferCopy.Size());
    Buffer.pAudioData = Slot.BufferCopy.Data();
    Buffer.Flags = XAUDIO2_END_OF_STREAM;
    Buffer.LoopCount = bLoop ? XAUDIO2_LOOP_INFINITE : 0;

    Volume = NormalizeAudioVolume(Volume);

    Result = SubmitSourceBuffer(*Implementation, Slot, Buffer);
    if (FAILED(Result))
    {
        DestroySlot(Slot);
        return kInvalidSound;
    }
    Result = SetSourceVolume(*Implementation, Slot, Volume, true);
    if (FAILED(Result))
    {
        DestroySlot(Slot);
        return kInvalidSound;
    }
    Result = StartSourceVoice(*Implementation, Slot);
    if (FAILED(Result))
    {
        DestroySlot(Slot);
        return kInvalidSound;
    }
    Slot.bInUse = true;
    Slot.ReservedBufferBytes = ReservedBufferBytes;
    ++Implementation->ActiveVoiceCount;
    Implementation->ResidentBufferBytes += ReservedBufferBytes;

    return FSoundHandle{Index, Slot.Generation};
}

namespace {
/** スロットのボイスを停止・破棄し、世代を進めて空きに戻す。 */
u64 DestroySlot(FVoiceSlot& Slot) noexcept
{
    const u64 ReleasedBufferBytes = Slot.ReservedBufferBytes;
    if (Slot.Voice)
    {
        Slot.Voice->Stop(0);
        Slot.Voice->FlushSourceBuffers();
        // SAFETY: DestroyVoice は mix スレッドが voice を処理し終えるまで同期的にブロックする。
        // ここは StateMutex 保持中だが、source voice は callback 無し (CreateSourceVoice に
        // IXAudio2VoiceCallback を渡さない) で作るため mix スレッドが StateMutex へ再入せず、
        // ブロックしても deadlock しない。将来 voice callback を登録するなら、この破棄は
        // StateMutex の外へ出すこと。
        Slot.Voice->DestroyVoice();
        Slot.Voice = nullptr;
    }
    Slot.BufferCopy.ReleaseStorage();
    ++Slot.Generation;
    Slot.ReservedBufferBytes = 0u;
    Slot.bInUse = false;
#if defined(ACS_AUDIO_TEST_HOOKS)
    Slot.TestVolume = 1.0f;
    Slot.bTestVoiceCreated = false;
#endif
    return ReleasedBufferBytes;
}
} // namespace

void CAudioEngine::Stop(FSoundHandle Handle) noexcept
{
    if (IsShutdownRequested())
    {
        return;
    }

    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested())
    {
        return;
    }
    FImpl* const Implementation = m_Impl;
    if (Implementation == nullptr || !Handle.IsValid() || Handle.index >= kMaxVoices)
    {
        return;
    }
    FScopedLock StateLock(Implementation->StateMutex);
    FVoiceSlot& Slot = Implementation->Slots[Handle.index];
    if (!Slot.bInUse || Slot.Generation != Handle.generation)
    {
        return;
    }
    const u64 ReleasedBufferBytes = DestroySlot(Slot);
    ACS_ASSERT(Implementation->ResidentBufferBytes >= ReleasedBufferBytes);
    if (Implementation->ResidentBufferBytes >= ReleasedBufferBytes)
    {
        Implementation->ResidentBufferBytes -= ReleasedBufferBytes;
    }
    else
    {
        Implementation->ResidentBufferBytes = 0u;
    }
    if (Implementation->ActiveVoiceCount > 0)
    {
        --Implementation->ActiveVoiceCount;
    }
}

void CAudioEngine::SetVolume(FSoundHandle Handle, f32 Volume) noexcept
{
    if (IsShutdownRequested())
    {
        return;
    }

    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested())
    {
        return;
    }
    FImpl* const Implementation = m_Impl;
    if (Implementation == nullptr || !Handle.IsValid() || Handle.index >= kMaxVoices)
    {
        return;
    }
    FScopedLock StateLock(Implementation->StateMutex);
    FVoiceSlot& Slot = Implementation->Slots[Handle.index];
    if (!Slot.bInUse || Slot.Generation != Handle.generation)
    {
        return;
    }
    Volume = NormalizeAudioVolume(Volume);
    const HRESULT Result = SetSourceVolume(*Implementation, Slot, Volume, false);
    if (FAILED(Result))
    {
        RecordVolumeFailureWarning(*Implementation);
        ACS_LOG_WARN("CAudioEngine::SetVolume failed (hr=0x%08lx)", static_cast<unsigned long>(Result));
    }
}

void CAudioEngine::StopAll() noexcept
{
    if (IsShutdownRequested())
    {
        return;
    }

    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested())
    {
        return;
    }
    FImpl* const Implementation = m_Impl;
    if (Implementation == nullptr)
    {
        return;
    }
    StopAllSlots(*Implementation);
}

void CAudioEngine::SetMasterVolume(f32 Volume) noexcept
{
    if (IsShutdownRequested())
    {
        return;
    }

    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested())
    {
        return;
    }
    FImpl* const Implementation = m_Impl;
    if (Implementation == nullptr)
    {
        return;
    }
    FScopedLock StateLock(Implementation->StateMutex);
    Volume = NormalizeAudioVolume(Volume);
    const HRESULT Result = ApplyMasterVolume(*Implementation, Volume);
    if (FAILED(Result))
    {
        RecordVolumeFailureWarning(*Implementation);
        ACS_LOG_WARN("CAudioEngine::SetMasterVolume failed (hr=0x%08lx)", static_cast<unsigned long>(Result));
    }
}

void CAudioEngine::PauseAll() noexcept
{
    if (IsShutdownRequested())
    {
        return;
    }

    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested())
    {
        return;
    }
    FImpl* const Implementation = m_Impl;
    if (Implementation == nullptr)
    {
        return;
    }
    FScopedLock StateLock(Implementation->StateMutex);
    for (u32 Index = 0; Index < kMaxVoices; ++Index)
    {
        FVoiceSlot& Slot = Implementation->Slots[Index];
        if (Slot.bInUse && Slot.Voice)
        {
            Slot.Voice->Stop(0);
        }
    }
}

void CAudioEngine::ResumeAll() noexcept
{
    if (IsShutdownRequested())
    {
        return;
    }

    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested())
    {
        return;
    }
    FImpl* const Implementation = m_Impl;
    if (Implementation == nullptr)
    {
        return;
    }
    FScopedLock StateLock(Implementation->StateMutex);
    for (u32 Index = 0; Index < kMaxVoices; ++Index)
    {
        FVoiceSlot& Slot = Implementation->Slots[Index];
        if (Slot.bInUse && Slot.Voice)
        {
            Slot.Voice->Start(0);
        }
    }
}

u32 CAudioEngine::ActiveCount() const noexcept
{
    if (IsShutdownRequested())
    {
        return 0;
    }

    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested())
    {
        return 0;
    }
#if defined(ACS_AUDIO_TEST_HOOKS)
    WaitForAudioLifecycleTestGate();
#endif
    FImpl* const Implementation = m_Impl;
    if (Implementation == nullptr)
    {
        return 0;
    }
    FScopedLock StateLock(Implementation->StateMutex);
    return Implementation->ActiveVoiceCount;
}

#if defined(ACS_AUDIO_TEST_HOOKS)
TResult<void> CAudioEngine::InitializeLifecycleTestState() noexcept
{
    if (IsShutdownRequested())
    {
        return ACS_ERR(Generic, 5, "CAudioEngine shutdown is in progress");
    }

    FScopedExclusiveLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested())
    {
        return ACS_ERR(Generic, 5, "CAudioEngine shutdown is in progress");
    }
    if (m_Impl != nullptr)
    {
        return ACS_ERR(Generic, 1, "CAudioEngine already initialized");
    }

    m_Impl = new (std::nothrow) FImpl();
    if (m_Impl == nullptr)
    {
        return ACS_ERR(Memory, 6, "CAudioEngine test state allocation failed");
    }
    m_Impl->bMtaUsageAcquired = true;
    m_Impl->bTestMtaUsage = true;
    return Ok();
}

/** 公開音量操作を実音声機器なしで検証できる内部状態を作る。 */
TResult<void> CAudioEngine::InitializeVolumeTestState() noexcept
{
    TResult<void> Result = InitializeLifecycleTestState();
    if (Result.IsErr())
    {
        return Result;
    }

    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (m_Impl == nullptr)
    {
        return ACS_ERR(Generic, 5, "CAudioEngine volume test state is unavailable");
    }
    FScopedLock StateLock(m_Impl->StateMutex);
    m_Impl->bVolumeTestBackend = true;
    return Ok();
}

/** 状態読み取り処理を停止するテスト用の合図を設定する。 */
void CAudioEngine::ConfigureLifecycleOperationTestGate(TAtomic<u32>* Entered, TAtomic<u32>* Release) noexcept
{
    g_AudioLifecycleTestRelease.Store(Release, EMemoryOrder::Release);
    g_AudioLifecycleTestEntered.Store(Entered, EMemoryOrder::Release);
}

/** テストbackendで失敗させる音量操作を設定する。 */
void CAudioEngine::ConfigureVolumeFailuresForTesting(bool bPlayVolume, bool bSetVolume, bool bMasterVolume) noexcept
{
    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (m_Impl == nullptr)
    {
        return;
    }
    FScopedLock StateLock(m_Impl->StateMutex);
    if (!m_Impl->bVolumeTestBackend)
    {
        return;
    }
    m_Impl->bFailPlayVolume = bPlayVolume;
    m_Impl->bFailSetVolume = bSetVolume;
    m_Impl->bFailMasterVolume = bMasterVolume;
}

/** 公開音量操作と同じ規則で入力を正規化する。 */
f32 CAudioEngine::NormalizeVolumeForTesting(f32 Volume) noexcept
{
    return NormalizeAudioVolume(Volume);
}

/** 指定した再生についてテストbackendが保持する個別音量を返す。 */
f32 CAudioEngine::VolumeForTesting(FSoundHandle Handle) const noexcept
{
    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (m_Impl == nullptr || !Handle.IsValid() || Handle.index >= kMaxVoices)
    {
        return -1.0f;
    }
    FScopedLock StateLock(m_Impl->StateMutex);
    const FVoiceSlot& Slot = m_Impl->Slots[Handle.index];
    if (!m_Impl->bVolumeTestBackend || !Slot.bInUse || Slot.Generation != Handle.generation)
    {
        return -1.0f;
    }
    return Slot.TestVolume;
}

/** テストbackendが保持する全体音量を返す。 */
f32 CAudioEngine::MasterVolumeForTesting() const noexcept
{
    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (m_Impl == nullptr)
    {
        return -1.0f;
    }
    FScopedLock StateLock(m_Impl->StateMutex);
    return m_Impl->bVolumeTestBackend ? m_Impl->TestMasterVolume : -1.0f;
}

/** テストbackendが最後に受け取った正規化済み音量を返す。 */
f32 CAudioEngine::LastVolumeAttemptForTesting() const noexcept
{
    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (m_Impl == nullptr)
    {
        return -1.0f;
    }
    FScopedLock StateLock(m_Impl->StateMutex);
    return m_Impl->bVolumeTestBackend ? m_Impl->LastVolumeAttempt : -1.0f;
}

/** テストbackendで再生中に保持している音声データ容量を返す。 */
u64 CAudioEngine::ResidentBufferBytesForTesting() const noexcept
{
    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (m_Impl == nullptr)
    {
        return 0u;
    }
    FScopedLock StateLock(m_Impl->StateMutex);
    return m_Impl->ResidentBufferBytes;
}

/** テストbackendが現在確保している発音ボイス数を返す。 */
u32 CAudioEngine::AllocatedVoiceCountForTesting() const noexcept
{
    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (m_Impl == nullptr)
    {
        return 0u;
    }
    FScopedLock StateLock(m_Impl->StateMutex);
    if (!m_Impl->bVolumeTestBackend)
    {
        return 0u;
    }

    /** 確保中のテスト用発音ボイス数。 */
    u32 VoiceCount = 0u;

    /** 確保状態を確認する発音枠番号。 */
    for (u32 Index = 0u; Index < kMaxVoices; ++Index)
    {
        if (m_Impl->Slots[Index].bTestVoiceCreated)
        {
            ++VoiceCount;
        }
    }
    return VoiceCount;
}

/** 個別音量または全体音量の失敗を警告した回数を返す。 */
u32 CAudioEngine::VolumeFailureWarningCountForTesting() const noexcept
{
    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (m_Impl == nullptr)
    {
        return 0u;
    }
    FScopedLock StateLock(m_Impl->StateMutex);
    return m_Impl->VolumeFailureWarningCount;
}

bool CAudioEngine::IsShutdownRequestedForTesting() const noexcept
{
    return IsShutdownRequested();
}

bool CAudioEngine::HasLifecycleStateForTesting() const noexcept
{
    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    return m_Impl != nullptr;
}
#endif

} // namespace acs
