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

#include <combaseapi.h>
#include <new>
#include <xaudio2.h>

namespace acs {

namespace {

/** 同時に確保できる発音スロット (ソースボイス) の上限。 */
constexpr u32 kMaxVoices = 64;

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
 * FAudioEngine の pimpl 実装 (XAudio2 ハンドルとスロット表を保持)。
 */
struct FAudioEngine::FImpl {
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

/** 全スロットを停止して active count を 0 に戻す。 */
void StopAllSlots(FAudioEngine::FImpl& Implementation) noexcept
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

FAudioEngine::~FAudioEngine() noexcept
{
    Shutdown();
}

bool FAudioEngine::IsShutdownRequested() const noexcept
{
    return m_ShutdownRequests.Load(EMemoryOrder::Acquire) != 0u;
}

TResult<void> FAudioEngine::Init() noexcept
{
    if (IsShutdownRequested())
    {
        return ACS_ERR(Generic, 5, "FAudioEngine shutdown is in progress");
    }

    FScopedExclusiveLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested())
    {
        return ACS_ERR(Generic, 5, "FAudioEngine shutdown is in progress");
    }
    if (m_Impl)
    {
        return ACS_ERR(Generic, 1, "FAudioEngine already initialized");
    }
    m_Impl = new (std::nothrow) FImpl();
    if (m_Impl == nullptr)
    {
        return ACS_ERR(Memory, 6, "FAudioEngine state allocation failed");
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

    ACS_LOG_INFO("FAudioEngine initialized (XAudio2)");
    return Ok();
}

void FAudioEngine::Shutdown() noexcept
{
    FScopedAudioShutdownRequest ShutdownRequest(m_ShutdownRequests);
    FScopedExclusiveLock LifecycleLock(m_LifecycleLock);
    if (!m_Impl)
    {
        return;
    }

    ShutdownUnlocked();
}

void FAudioEngine::ShutdownUnlocked() noexcept
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
            ACS_LOG_ERROR("FAudioEngine::Shutdown: CoDecrementMTAUsage failed (hr=0x%08lx)",
                          static_cast<unsigned long>(Result));
            ACS_ASSERT(false && "FAudioEngine MTA usage release failed");
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
u32 FindFreeSlot(FAudioEngine::FImpl& Implementation) noexcept
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

FSoundHandle FAudioEngine::Play(const FAudioAsset& Asset, f32 Volume, bool bLoop) noexcept
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
    if (Implementation == nullptr || Implementation->XAudio2 == nullptr ||
        Asset.SampleByteCount() == 0 || Asset.SampleByteCount() > 0xFFFFFFFFu)
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

    HRESULT Result = Implementation->XAudio2->CreateSourceVoice(&Slot.Voice, &WaveFormat);
    if (FAILED(Result) || Slot.Voice == nullptr)
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

    if (Volume < 0)
    {
        Volume = 0;
    }
    if (Volume > 1)
    {
        Volume = 1;
    }

    Result = Slot.Voice->SubmitSourceBuffer(&Buffer);
    if (FAILED(Result))
    {
        DestroySlot(Slot);
        return kInvalidSound;
    }
    Slot.Voice->SetVolume(Volume);
    Result = Slot.Voice->Start(0);
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
    return ReleasedBufferBytes;
}
} // namespace

void FAudioEngine::Stop(FSoundHandle Handle) noexcept
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

void FAudioEngine::SetVolume(FSoundHandle Handle, f32 Volume) noexcept
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
    if (Volume < 0)
    {
        Volume = 0;
    }
    if (Volume > 1)
    {
        Volume = 1;
    }
    if (Slot.Voice)
    {
        Slot.Voice->SetVolume(Volume);
    }
}

void FAudioEngine::StopAll() noexcept
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

void FAudioEngine::SetMasterVolume(f32 Volume) noexcept
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
    if (Implementation == nullptr || Implementation->MasteringVoice == nullptr)
    {
        return;
    }
    FScopedLock StateLock(Implementation->StateMutex);
    if (Volume < 0)
    {
        Volume = 0;
    }
    if (Volume > 1)
    {
        Volume = 1;
    }
    Implementation->MasteringVoice->SetVolume(Volume);
}

void FAudioEngine::PauseAll() noexcept
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

void FAudioEngine::ResumeAll() noexcept
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

u32 FAudioEngine::ActiveCount() const noexcept
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
TResult<void> FAudioEngine::InitializeLifecycleTestState() noexcept
{
    if (IsShutdownRequested())
    {
        return ACS_ERR(Generic, 5, "FAudioEngine shutdown is in progress");
    }

    FScopedExclusiveLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested())
    {
        return ACS_ERR(Generic, 5, "FAudioEngine shutdown is in progress");
    }
    if (m_Impl != nullptr)
    {
        return ACS_ERR(Generic, 1, "FAudioEngine already initialized");
    }

    m_Impl = new (std::nothrow) FImpl();
    if (m_Impl == nullptr)
    {
        return ACS_ERR(Memory, 6, "FAudioEngine test state allocation failed");
    }
    m_Impl->bMtaUsageAcquired = true;
    m_Impl->bTestMtaUsage = true;
    return Ok();
}

void FAudioEngine::ConfigureLifecycleOperationTestGate(TAtomic<u32>* Entered,
                                                       TAtomic<u32>* Release) noexcept
{
    g_AudioLifecycleTestRelease.Store(Release, EMemoryOrder::Release);
    g_AudioLifecycleTestEntered.Store(Entered, EMemoryOrder::Release);
}

bool FAudioEngine::IsShutdownRequestedForTesting() const noexcept
{
    return IsShutdownRequested();
}

bool FAudioEngine::HasLifecycleStateForTesting() const noexcept
{
    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    return m_Impl != nullptr;
}
#endif

} // namespace acs
