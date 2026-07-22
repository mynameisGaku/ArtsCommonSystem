// SPDX-License-Identifier: Apache-2.0
// GameFramework の XAudio2 backend 実装。
#include "gameframework/audio_backend/XAudio2Backend.h"

#include "container/Array.h"
#include "foundation/Assert.h"
#include "foundation/Log.h"
#include "memory/Memory.h"
#include "threading/Atomic.h"
#include "threading/Mutex.h"
#include "threading/ScopedLock.h"
#if defined(ACS_XAUDIO2_BACKEND_TEST_HOOKS)
#include "threading/Thread.h"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <combaseapi.h>
#include <new>
#include <xaudio2.h>

namespace acs::game {

namespace {

/** 音量を XAudio2 backend の公開範囲へ収める。 */
f32 ClampVolume(f32 Value) noexcept
{
    if (Value < 0.0f)
    {
        return 0.0f;
    }
    if (Value > 1.0f)
    {
        return 1.0f;
    }
    return Value;
}

/** 再生ピッチを実用範囲へ収める。 */
f32 ClampPitch(f32 Value) noexcept
{
    constexpr f32 kMinimumPitch = 0.25f;
    constexpr f32 kMaximumPitch = 4.0f;
    if (Value < kMinimumPitch)
    {
        return kMinimumPitch;
    }
    if (Value > kMaximumPitch)
    {
        return kMaximumPitch;
    }
    return Value;
}

/** 1 個の発音 slot。 */
struct FVoiceSlot {
    /** XAudio2 source voice。空き slot では nullptr。 */
    IXAudio2SourceVoice* Voice = nullptr;

    /** XAudio2 が再生中に参照し続ける PCM コピー。 */
    TArray<byte> Buffer;

    /** この発音へ割り当てたプロセス一意のハンドル。 */
    FAudioVoiceHandle Handle = {};

    /** 常駐 buffer 予算へ計上した確保容量。 */
    u64 ReservedBufferBytes = 0u;

    /** 使用中か。 */
    bool bActive = false;

    /** ループ再生か。 */
    bool bLooped = false;
};

/** Shutdown 要求数を関数終了まで保持し、新しい共有操作を閉じる。 */
class FScopedBackendShutdownRequest final {
public:
    explicit FScopedBackendShutdownRequest(TAtomic<u32>& Requests) noexcept
        : m_Requests(Requests)
    {
        m_Requests.FetchAdd(1u);
    }

    ~FScopedBackendShutdownRequest() noexcept
    {
        m_Requests.FetchSub(1u);
    }

    FScopedBackendShutdownRequest(const FScopedBackendShutdownRequest&) = delete;
    FScopedBackendShutdownRequest& operator=(const FScopedBackendShutdownRequest&) = delete;

private:
    TAtomic<u32>& m_Requests;
};

/** 発音ハンドルへ割り当てるプロセス通算チケット。 */
TAtomic<u64> g_NextAudioVoiceTicket{1u};

FAudioVoiceHandle AcquireAudioVoiceHandle() noexcept
{
    constexpr u64 kLargestTicket = static_cast<u64>(~u32(0));
    u64 Current = g_NextAudioVoiceTicket.Load(EMemoryOrder::Acquire);
    for (;;)
    {
        if (Current > kLargestTicket)
        {
            return kInvalidAudioVoice;
        }
        if (g_NextAudioVoiceTicket.CompareExchange(Current, Current + 1u))
        {
            return FAudioVoiceHandle::FromPackedValue(static_cast<u32>(Current));
        }
    }
}

#if defined(ACS_XAUDIO2_BACKEND_TEST_HOOKS)
TAtomic<TAtomic<u32>*> g_BackendLifecycleTestEntered{nullptr};
TAtomic<TAtomic<u32>*> g_BackendLifecycleTestRelease{nullptr};

void WaitForBackendLifecycleTestGate() noexcept
{
    TAtomic<u32>* Entered = g_BackendLifecycleTestEntered.Load(EMemoryOrder::Acquire);
    TAtomic<u32>* Release = g_BackendLifecycleTestRelease.Load(EMemoryOrder::Acquire);
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

/** XAudio2 と COM MTA cookie と voice pool を保持する pimpl。 */
struct FXAudio2Backend::FImpl {
    /** XAudio2 エンジン。 */
    IXAudio2* XAudio2 = nullptr;

    /** 最終出力の mastering voice。 */
    IXAudio2MasteringVoice* MasteringVoice = nullptr;

    /** MTA 利用参照を任意スレッドから解除するための cookie。 */
    CO_MTA_USAGE_COOKIE MtaUsageCookie = nullptr;

    /** CoIncrementMTAUsage が成功したか。 */
    bool bMtaUsageAcquired = false;

#if defined(ACS_XAUDIO2_BACKEND_TEST_HOOKS)
    /** テスト用の疑似 MTA 参照で、実 COM API を呼ばない。 */
    bool bTestMtaUsage = false;
#endif

    /** Init が最後まで成功したか。 */
    bool bInitialized = false;

    /** voice pool と統計を保護する mutex。 */
    FMutex StateMutex;

    /** 固定容量の発音 slot。 */
    TArray<FVoiceSlot> Slots;

    /** slot 数。 */
    u32 MaxVoices = 0u;

    /** 使用中 slot 数。 */
    u32 ActiveVoiceCount = 0u;

    /** 全 slot の buffer 確保容量合計。 */
    u64 ResidentBufferBytes = 0u;
};

namespace {

void DestroySlot(FXAudio2Backend::FImpl& Implementation, FVoiceSlot& Slot) noexcept;

/** mutex 取得済みの pimpl から全 voice を解放する。 */
void StopAllVoicesLocked(FXAudio2Backend::FImpl& Implementation) noexcept
{
    for (u32 Index = 0u; Index < Implementation.MaxVoices; ++Index)
    {
        FVoiceSlot& Slot = Implementation.Slots[Index];
        if (Slot.bActive)
        {
            DestroySlot(Implementation, Slot);
        }
    }
    Implementation.ActiveVoiceCount = 0u;
}

/** clip から検証済み WAVEFORMATEX を作る。 */
bool FillWaveFormat(const FAudioClipDesc& Clip, WAVEFORMATEX& WaveFormat) noexcept
{
    if (Clip.channel_count == 0u || Clip.channel_count > XAUDIO2_MAX_AUDIO_CHANNELS ||
        Clip.sample_rate < XAUDIO2_MIN_SAMPLE_RATE || Clip.sample_rate > XAUDIO2_MAX_SAMPLE_RATE)
    {
        return false;
    }

    WaveFormat = WAVEFORMATEX{};
    WaveFormat.nChannels = static_cast<WORD>(Clip.channel_count);
    WaveFormat.nSamplesPerSec = Clip.sample_rate;
    switch (Clip.format)
    {
    case EAudioFormat::Pcm16:
        WaveFormat.wFormatTag = WAVE_FORMAT_PCM;
        WaveFormat.wBitsPerSample = 16u;
        break;
    case EAudioFormat::Pcm32Float:
        WaveFormat.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        WaveFormat.wBitsPerSample = 32u;
        break;
    case EAudioFormat::Wav:
        return false;
    default:
        return false;
    }

    const u32 BlockAlign =
        (static_cast<u32>(WaveFormat.nChannels) * WaveFormat.wBitsPerSample) / 8u;
    if (BlockAlign == 0u || BlockAlign > 0xFFFFu)
    {
        return false;
    }

    WaveFormat.nBlockAlign = static_cast<WORD>(BlockAlign);
    WaveFormat.nAvgBytesPerSec = WaveFormat.nSamplesPerSec * BlockAlign;
    WaveFormat.cbSize = 0u;
    return true;
}

/** voice と保持中の PCM コピーを完全に破棄する。 */
void DestroySlot(FXAudio2Backend::FImpl& Implementation, FVoiceSlot& Slot) noexcept
{
    if (Slot.Voice != nullptr)
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
    ACS_ASSERT(Implementation.ResidentBufferBytes >= Slot.ReservedBufferBytes);
    if (Implementation.ResidentBufferBytes >= Slot.ReservedBufferBytes)
    {
        Implementation.ResidentBufferBytes -= Slot.ReservedBufferBytes;
    }
    else
    {
        Implementation.ResidentBufferBytes = 0u;
    }
    Slot.Buffer.ReleaseStorage();
    Slot.Handle = kInvalidAudioVoice;
    Slot.ReservedBufferBytes = 0u;
    Slot.bActive = false;
    Slot.bLooped = false;
}

/** mutex 取得済みの pool から指定ハンドルを探す。 */
FVoiceSlot* FindVoiceSlot(FXAudio2Backend::FImpl& Implementation,
                          FAudioVoiceHandle Voice) noexcept
{
    for (u32 Index = 0u; Index < Implementation.MaxVoices; ++Index)
    {
        FVoiceSlot& Slot = Implementation.Slots[Index];
        if (Slot.bActive && Slot.Handle == Voice)
        {
            return &Slot;
        }
    }
    return nullptr;
}

/** mutex 取得済みの pimpl で一発またはループ再生を開始する。 */
FAudioVoiceHandle PlayInternal(FXAudio2Backend::FImpl& Implementation,
                              const FAudioClipDesc& Clip,
                              f32 Volume,
                              f32 Pitch,
                              bool bLoop) noexcept
{
    if (!Implementation.bInitialized || Implementation.XAudio2 == nullptr ||
        Clip.pcm_data == nullptr || Clip.pcm_size == 0u ||
        Clip.pcm_size > static_cast<u64>(~u32(0)))
    {
        return kInvalidAudioVoice;
    }

    WAVEFORMATEX WaveFormat{};
    if (!FillWaveFormat(Clip, WaveFormat) ||
        (Clip.pcm_size % static_cast<u64>(WaveFormat.nBlockAlign)) != 0u)
    {
        ACS_LOG_WARN("FXAudio2Backend::Play: invalid clip (channels=%u, rate=%u, format=%u, bytes=%llu)",
                     Clip.channel_count,
                     Clip.sample_rate,
                     static_cast<u32>(Clip.format),
                     static_cast<unsigned long long>(Clip.pcm_size));
        return kInvalidAudioVoice;
    }

    u32 SlotIndex = Implementation.MaxVoices;
    for (u32 Index = 0u; Index < Implementation.MaxVoices; ++Index)
    {
        if (!Implementation.Slots[Index].bActive)
        {
            SlotIndex = Index;
            break;
        }
    }
    if (SlotIndex == Implementation.MaxVoices)
    {
        ACS_LOG_WARN("FXAudio2Backend::Play: voice pool full (max_voices=%u)",
                     Implementation.MaxVoices);
        return kInvalidAudioVoice;
    }

    const FAudioVoiceHandle Handle = AcquireAudioVoiceHandle();
    if (!Handle.IsValid())
    {
        ACS_LOG_ERROR("FXAudio2Backend::Play: 32-bit voice handle space exhausted");
        return kInvalidAudioVoice;
    }

    FVoiceSlot& Slot = Implementation.Slots[SlotIndex];
    IXAudio2SourceVoice* SourceVoice = nullptr;
    HRESULT Result = Implementation.XAudio2->CreateSourceVoice(&SourceVoice, &WaveFormat);
    if (FAILED(Result) || SourceVoice == nullptr)
    {
        if (SourceVoice != nullptr)
        {
            SourceVoice->DestroyVoice();
        }
        ACS_LOG_WARN("FXAudio2Backend::Play: CreateSourceVoice failed hr=0x%08x",
                     static_cast<u32>(Result));
        return kInvalidAudioVoice;
    }

    if (Implementation.ResidentBufferBytes > kXAudio2BackendResidentBufferBudgetBytes ||
        Clip.pcm_size >
            kXAudio2BackendResidentBufferBudgetBytes - Implementation.ResidentBufferBytes)
    {
        SourceVoice->DestroyVoice();
        ACS_LOG_WARN("FXAudio2Backend::Play: resident PCM budget exceeded "
                     "(requested=%llu, resident=%llu, budget=%llu)",
                     static_cast<unsigned long long>(Clip.pcm_size),
                     static_cast<unsigned long long>(Implementation.ResidentBufferBytes),
                     static_cast<unsigned long long>(kXAudio2BackendResidentBufferBudgetBytes));
        return kInvalidAudioVoice;
    }

    const usize ByteCount = static_cast<usize>(Clip.pcm_size);
    if (!Slot.Buffer.TryResize(ByteCount))
    {
        SourceVoice->DestroyVoice();
        Slot.Buffer.ReleaseStorage();
        ACS_LOG_WARN("FXAudio2Backend::Play: PCM copy allocation failed (bytes=%llu)",
                     static_cast<unsigned long long>(Clip.pcm_size));
        return kInvalidAudioVoice;
    }
    const u64 ReservedBufferBytes = static_cast<u64>(Slot.Buffer.Capacity());
    if (ReservedBufferBytes >
        kXAudio2BackendResidentBufferBudgetBytes - Implementation.ResidentBufferBytes)
    {
        SourceVoice->DestroyVoice();
        Slot.Buffer.ReleaseStorage();
        ACS_LOG_WARN("FXAudio2Backend::Play: allocated PCM capacity exceeds resident budget");
        return kInvalidAudioVoice;
    }
    MemCopy(Slot.Buffer.Data(), Clip.pcm_data, ByteCount);

    XAUDIO2_BUFFER Buffer{};
    Buffer.AudioBytes = static_cast<UINT32>(ByteCount);
    Buffer.pAudioData = Slot.Buffer.Data();
    Buffer.Flags = XAUDIO2_END_OF_STREAM;
    Buffer.LoopCount = bLoop ? XAUDIO2_LOOP_INFINITE : 0u;

    Result = SourceVoice->SubmitSourceBuffer(&Buffer);
    if (FAILED(Result))
    {
        SourceVoice->DestroyVoice();
        Slot.Buffer.ReleaseStorage();
        ACS_LOG_WARN("FXAudio2Backend::Play: SubmitSourceBuffer failed hr=0x%08x",
                     static_cast<u32>(Result));
        return kInvalidAudioVoice;
    }

    SourceVoice->SetVolume(ClampVolume(Volume));
    SourceVoice->SetFrequencyRatio(ClampPitch(Pitch));
    Result = SourceVoice->Start(0);
    if (FAILED(Result))
    {
        SourceVoice->DestroyVoice();
        Slot.Buffer.ReleaseStorage();
        ACS_LOG_WARN("FXAudio2Backend::Play: Start failed hr=0x%08x",
                     static_cast<u32>(Result));
        return kInvalidAudioVoice;
    }

    Slot.Voice = SourceVoice;
    Slot.Handle = Handle;
    Slot.ReservedBufferBytes = ReservedBufferBytes;
    Slot.bActive = true;
    Slot.bLooped = bLoop;
    Implementation.ResidentBufferBytes += ReservedBufferBytes;
    ++Implementation.ActiveVoiceCount;
    return Handle;
}

} // namespace

FXAudio2Backend::FXAudio2Backend() noexcept = default;

FXAudio2Backend::~FXAudio2Backend() noexcept
{
    Shutdown();
}

bool FXAudio2Backend::IsShutdownRequested() const noexcept
{
    return m_ShutdownRequests.Load(EMemoryOrder::Acquire) != 0u;
}

TResult<void> FXAudio2Backend::Init(u32 MaxVoices) noexcept
{
    if (IsShutdownRequested())
    {
        return ACS_ERR(Generic, kSubAudioNotInitialized,
                       "FXAudio2Backend::Init: shutdown is in progress");
    }
    if (MaxVoices == 0u || MaxVoices > kXAudio2BackendMaximumVoiceCount)
    {
        return ACS_ERR(Generic, kSubAudioInvalidArgs,
                       "FXAudio2Backend::Init: MaxVoices exceeds the supported range");
    }

    FScopedExclusiveLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested())
    {
        return ACS_ERR(Generic, kSubAudioNotInitialized,
                       "FXAudio2Backend::Init: shutdown is in progress");
    }
    if (m_Impl != nullptr)
    {
        return ACS_ERR(Generic, kSubAudioAlreadyInitialized,
                       "FXAudio2Backend::Init: already initialized");
    }

    m_Impl = new (std::nothrow) FImpl();
    if (m_Impl == nullptr)
    {
        return ACS_ERR(Memory, kSubAudioOutOfMemory,
                       "FXAudio2Backend::Init: state allocation failed");
    }

    HRESULT Result = ::CoIncrementMTAUsage(&m_Impl->MtaUsageCookie);
    if (FAILED(Result))
    {
        ShutdownUnlocked();
        return ACS_ERR_OS(Generic, kSubAudioComInitFailed,
                          "FXAudio2Backend::Init: CoIncrementMTAUsage failed",
                          static_cast<u32>(Result));
    }
    m_Impl->bMtaUsageAcquired = true;

    Result = ::XAudio2Create(&m_Impl->XAudio2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(Result))
    {
        ShutdownUnlocked();
        return ACS_ERR_OS(Generic, kSubAudioCreateFailed,
                          "FXAudio2Backend::Init: XAudio2Create failed",
                          static_cast<u32>(Result));
    }

    Result = m_Impl->XAudio2->CreateMasteringVoice(&m_Impl->MasteringVoice);
    if (FAILED(Result))
    {
        ShutdownUnlocked();
        return ACS_ERR_OS(Generic, kSubAudioMasterVoiceFailed,
                          "FXAudio2Backend::Init: CreateMasteringVoice failed",
                          static_cast<u32>(Result));
    }

    if (!m_Impl->Slots.TryResize(MaxVoices))
    {
        ShutdownUnlocked();
        return ACS_ERR(Memory, kSubAudioOutOfMemory,
                       "FXAudio2Backend::Init: voice pool allocation failed");
    }
    m_Impl->MaxVoices = MaxVoices;
    m_Impl->bInitialized = true;
    ACS_LOG_INFO("FXAudio2Backend: initialized (max_voices=%u)", MaxVoices);
    return Ok();
}

void FXAudio2Backend::Shutdown() noexcept
{
    FScopedBackendShutdownRequest ShutdownRequest(m_ShutdownRequests);
    FScopedExclusiveLock LifecycleLock(m_LifecycleLock);
    if (m_Impl == nullptr)
    {
        return;
    }
    ShutdownUnlocked();
}

void FXAudio2Backend::ShutdownUnlocked() noexcept
{
    FImpl* const Implementation = m_Impl;
    {
        FScopedLock StateLock(Implementation->StateMutex);
        StopAllVoicesLocked(*Implementation);
        Implementation->Slots.ReleaseStorage();
        Implementation->MaxVoices = 0u;
        Implementation->ResidentBufferBytes = 0u;

        if (Implementation->MasteringVoice != nullptr)
        {
            Implementation->MasteringVoice->DestroyVoice();
            Implementation->MasteringVoice = nullptr;
        }
        if (Implementation->XAudio2 != nullptr)
        {
            Implementation->XAudio2->Release();
            Implementation->XAudio2 = nullptr;
        }
        if (Implementation->bMtaUsageAcquired)
        {
            HRESULT Result = S_OK;
#if defined(ACS_XAUDIO2_BACKEND_TEST_HOOKS)
            if (!Implementation->bTestMtaUsage)
            {
                Result = ::CoDecrementMTAUsage(Implementation->MtaUsageCookie);
            }
#else
            Result = ::CoDecrementMTAUsage(Implementation->MtaUsageCookie);
#endif
            if (FAILED(Result))
            {
                ACS_LOG_ERROR("FXAudio2Backend::Shutdown: CoDecrementMTAUsage failed (hr=0x%08lx)",
                              static_cast<unsigned long>(Result));
                ACS_ASSERT(false && "FXAudio2Backend MTA usage release failed");
            }
            Implementation->MtaUsageCookie = nullptr;
            Implementation->bMtaUsageAcquired = false;
        }
        Implementation->bInitialized = false;
    }

    delete Implementation;
    m_Impl = nullptr;
}

bool FXAudio2Backend::IsInitialized() const noexcept
{
    if (IsShutdownRequested())
    {
        return false;
    }
    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested())
    {
        return false;
    }
    FImpl* const Implementation = m_Impl;
    if (Implementation == nullptr)
    {
        return false;
    }
    FScopedLock StateLock(Implementation->StateMutex);
    return Implementation->bInitialized;
}

FAudioVoiceHandle FXAudio2Backend::PlayOneShot(const FAudioClipDesc& Clip,
                                              f32 Volume,
                                              f32 Pitch) noexcept
{
    if (IsShutdownRequested())
    {
        return kInvalidAudioVoice;
    }
    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested())
    {
        return kInvalidAudioVoice;
    }
    FImpl* const Implementation = m_Impl;
    if (Implementation == nullptr)
    {
        return kInvalidAudioVoice;
    }
    FScopedLock StateLock(Implementation->StateMutex);
    return PlayInternal(*Implementation, Clip, Volume, Pitch, false);
}

FAudioVoiceHandle FXAudio2Backend::PlayLooped(const FAudioClipDesc& Clip,
                                             f32 Volume,
                                             f32 Pitch) noexcept
{
    if (IsShutdownRequested())
    {
        return kInvalidAudioVoice;
    }
    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested())
    {
        return kInvalidAudioVoice;
    }
    FImpl* const Implementation = m_Impl;
    if (Implementation == nullptr)
    {
        return kInvalidAudioVoice;
    }
    FScopedLock StateLock(Implementation->StateMutex);
    return PlayInternal(*Implementation, Clip, Volume, Pitch, true);
}

void FXAudio2Backend::StopVoice(FAudioVoiceHandle Voice) noexcept
{
    if (IsShutdownRequested() || !Voice.IsValid())
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
    if (!Implementation->bInitialized)
    {
        return;
    }
    FVoiceSlot* const Slot = FindVoiceSlot(*Implementation, Voice);
    if (Slot == nullptr)
    {
        return;
    }
    DestroySlot(*Implementation, *Slot);
    if (Implementation->ActiveVoiceCount > 0u)
    {
        --Implementation->ActiveVoiceCount;
    }
}

void FXAudio2Backend::SetVoiceVolume(FAudioVoiceHandle Voice, f32 Volume) noexcept
{
    if (IsShutdownRequested() || !Voice.IsValid())
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
    if (!Implementation->bInitialized)
    {
        return;
    }
    FVoiceSlot* const Slot = FindVoiceSlot(*Implementation, Voice);
    if (Slot == nullptr || Slot->Voice == nullptr)
    {
        return;
    }
    Slot->Voice->SetVolume(ClampVolume(Volume));
}

void FXAudio2Backend::StopAllVoices() noexcept
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
    StopAllVoicesLocked(*Implementation);
}

u32 FXAudio2Backend::ActiveVoiceCount() const noexcept
{
    if (IsShutdownRequested())
    {
        return 0u;
    }
    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested())
    {
        return 0u;
    }
#if defined(ACS_XAUDIO2_BACKEND_TEST_HOOKS)
    WaitForBackendLifecycleTestGate();
#endif
    FImpl* const Implementation = m_Impl;
    if (Implementation == nullptr)
    {
        return 0u;
    }
    FScopedLock StateLock(Implementation->StateMutex);
    return Implementation->ActiveVoiceCount;
}

void FXAudio2Backend::Tick(f32 DeltaSeconds) noexcept
{
    (void)DeltaSeconds;
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
    if (!Implementation->bInitialized)
    {
        return;
    }

    for (u32 Index = 0u; Index < Implementation->MaxVoices; ++Index)
    {
        FVoiceSlot& Slot = Implementation->Slots[Index];
        if (!Slot.bActive || Slot.bLooped || Slot.Voice == nullptr)
        {
            continue;
        }

        XAUDIO2_VOICE_STATE State{};
        Slot.Voice->GetState(&State);
        if (State.BuffersQueued == 0u)
        {
            DestroySlot(*Implementation, Slot);
            if (Implementation->ActiveVoiceCount > 0u)
            {
                --Implementation->ActiveVoiceCount;
            }
        }
    }
}

void FXAudio2Backend::SetMasterVolume(f32 Volume) noexcept
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
    if (Implementation->MasteringVoice != nullptr)
    {
        Implementation->MasteringVoice->SetVolume(ClampVolume(Volume));
    }
}

#if defined(ACS_XAUDIO2_BACKEND_TEST_HOOKS)
TResult<void> FXAudio2Backend::InitializeLifecycleTestState() noexcept
{
    if (IsShutdownRequested())
    {
        return ACS_ERR(Generic, kSubAudioNotInitialized,
                       "FXAudio2Backend test shutdown is in progress");
    }
    FScopedExclusiveLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested())
    {
        return ACS_ERR(Generic, kSubAudioNotInitialized,
                       "FXAudio2Backend test shutdown is in progress");
    }
    if (m_Impl != nullptr)
    {
        return ACS_ERR(Generic, kSubAudioAlreadyInitialized,
                       "FXAudio2Backend test state already initialized");
    }

    m_Impl = new (std::nothrow) FImpl();
    if (m_Impl == nullptr)
    {
        return ACS_ERR(Memory, kSubAudioOutOfMemory,
                       "FXAudio2Backend test state allocation failed");
    }
    m_Impl->bMtaUsageAcquired = true;
    m_Impl->bTestMtaUsage = true;
    m_Impl->bInitialized = true;
    return Ok();
}

void FXAudio2Backend::ConfigureLifecycleOperationTestGate(TAtomic<u32>* Entered,
                                                          TAtomic<u32>* Release) noexcept
{
    g_BackendLifecycleTestRelease.Store(Release, EMemoryOrder::Release);
    g_BackendLifecycleTestEntered.Store(Entered, EMemoryOrder::Release);
}

bool FXAudio2Backend::IsShutdownRequestedForTesting() const noexcept
{
    return IsShutdownRequested();
}

bool FXAudio2Backend::HasLifecycleStateForTesting() const noexcept
{
    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    return m_Impl != nullptr;
}
#endif

} // namespace acs::game
