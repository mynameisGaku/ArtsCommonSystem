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
#    include "threading/Thread.h"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
#include <combaseapi.h>
#include <cmath>
#include <new>
#include <xaudio2.h>

namespace acs::game {

namespace {

/** 再生ピッチの有限な下限。 */
constexpr f32 kMinimumPitch = 0.25f;

/** 再生ピッチと CreateSourceVoice の上限。 */
constexpr f32 kMaximumPitch = 4.0f;

/** 音量を XAudio2 backend の公開範囲へ収める。 */
f32 ClampVolume(f32 Value) noexcept
{
    if (!std::isfinite(Value)) {
        return 0.0f;
    }
    if (Value < 0.0f) {
        return 0.0f;
    }
    if (Value > 1.0f) {
        return 1.0f;
    }
    return Value;
}

/** 左右パンを有限な [-1, 1] へ収める。 */
f32 ClampPan(f32 Value) noexcept
{
    if (!std::isfinite(Value)) {
        return 0.0f;
    }
    if (Value < -1.0f) {
        return -1.0f;
    }
    if (Value > 1.0f) {
        return 1.0f;
    }
    return Value;
}

/** 再生ピッチを実用範囲へ収める。 */
f32 ClampPitch(f32 Value) noexcept
{
    if (!std::isfinite(Value)) {
        return 1.0f;
    }
    if (Value < kMinimumPitch) {
        return kMinimumPitch;
    }
    if (Value > kMaximumPitch) {
        return kMaximumPitch;
    }
    return Value;
}

/** 32bit speaker mask に立っている bit 数を返す。 */
u32 CountSetBits(u32 Mask) noexcept
{
    u32 Count = 0u;
    while (Mask != 0u) {
        Count += Mask & 1u;
        Mask >>= 1u;
    }
    return Count;
}

/**
 * mastering voice の speaker mask から mono voice 用の左右パン matrix を作る。
 *
 * @param SourceChannels source voice の入力 channel 数。1 以外は未対応。
 * @param DestinationChannels mastering voice の入力 channel 数。
 * @param DestinationChannelMask speaker の配置 mask。
 * @param Pan 左右パン。有限でない値は中央にする。
 * @param OutMatrix DestinationChannels 要素以上を持つ書き込み先。
 * @param MatrixCapacity OutMatrix の要素数。
 * @return mask の set-bit 順で前方左右 speaker を特定できた場合は true。
 */
bool BuildMonoPanMatrix(u32 SourceChannels, u32 DestinationChannels, u32 DestinationChannelMask, f32 Pan,
                        f32* OutMatrix, u32 MatrixCapacity) noexcept
{
    constexpr u32 kFrontLeftMask = static_cast<u32>(SPEAKER_FRONT_LEFT);
    constexpr u32 kFrontRightMask = static_cast<u32>(SPEAKER_FRONT_RIGHT);
    constexpr u32 kUnsupportedSpeakerMask = static_cast<u32>(SPEAKER_RESERVED) | static_cast<u32>(SPEAKER_ALL);
    if (SourceChannels != 1u || OutMatrix == nullptr || DestinationChannels < 2u ||
        DestinationChannels > XAUDIO2_MAX_AUDIO_CHANNELS || MatrixCapacity < DestinationChannels ||
        DestinationChannelMask == 0u || (DestinationChannelMask & kUnsupportedSpeakerMask) != 0u ||
        (DestinationChannelMask & kFrontLeftMask) == 0u || (DestinationChannelMask & kFrontRightMask) == 0u ||
        CountSetBits(DestinationChannelMask) != DestinationChannels) {
        return false;
    }

    for (u32 DestinationIndex = 0u; DestinationIndex < DestinationChannels; ++DestinationIndex) {
        OutMatrix[DestinationIndex] = 0.0f;
    }

    u32 FrontLeftIndex = DestinationChannels;
    u32 FrontRightIndex = DestinationChannels;
    u32 DestinationIndex = 0u;
    for (u32 SpeakerBitIndex = 0u; SpeakerBitIndex < 32u; ++SpeakerBitIndex) {
        const u32 SpeakerBit = 1u << SpeakerBitIndex;
        if ((DestinationChannelMask & SpeakerBit) == 0u) {
            continue;
        }
        if (SpeakerBit == kFrontLeftMask) {
            FrontLeftIndex = DestinationIndex;
        } else if (SpeakerBit == kFrontRightMask) {
            FrontRightIndex = DestinationIndex;
        }
        ++DestinationIndex;
    }
    if (FrontLeftIndex >= DestinationChannels || FrontRightIndex >= DestinationChannels) {
        return false;
    }

    const f32 NormalizedPan = ClampPan(Pan);
    const f32 LeftPower = (1.0f - NormalizedPan) * 0.5f;
    const f32 RightPower = (1.0f + NormalizedPan) * 0.5f;
    OutMatrix[FrontLeftIndex] = std::sqrt(LeftPower);
    OutMatrix[FrontRightIndex] = std::sqrt(RightPower);
    return true;
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

    /** source voice の入力 channel 数。空き slot では 0。 */
    u32 SourceChannels = 0u;

    /** 使用中か。 */
    bool bActive = false;

    /** ループ再生か。 */
    bool bLooped = false;
};

/** Shutdown 要求数を関数終了まで保持し、新しい共有操作を閉じる。 */
class FScopedBackendShutdownRequest final {
public:
    explicit FScopedBackendShutdownRequest(TAtomic<u32>& Requests) noexcept : m_Requests(Requests)
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
    for (;;) {
        if (Current > kLargestTicket) {
            return kInvalidAudioVoice;
        }
        if (g_NextAudioVoiceTicket.CompareExchange(Current, Current + 1u)) {
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
    if (Entered == nullptr || Release == nullptr) {
        return;
    }

    Entered->Store(1u, EMemoryOrder::Release);
    while (Release->Load(EMemoryOrder::Acquire) == 0u) {
        Yield();
    }
}
#endif

} // namespace

/** XAudio2 と COM MTA cookie と voice pool を保持する pimpl。 */
struct CXAudio2Backend::FImpl {
    /** XAudio2 エンジン。 */
    IXAudio2* XAudio2 = nullptr;

    /** 最終出力の mastering voice。 */
    IXAudio2MasteringVoice* MasteringVoice = nullptr;

    /** mastering voice が受け取る destination channel 数。 */
    u32 MasteringInputChannels = 0u;

    /** mastering voice の speaker 配置 mask。 */
    u32 MasteringChannelMask = 0u;

    /** GetChannelMask が成功し、MasteringChannelMask を利用できるか。 */
    bool bMasteringChannelMaskValid = false;

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

void DestroySlot(CXAudio2Backend::FImpl& Implementation, FVoiceSlot& Slot) noexcept;

/** mutex 取得済みの pimpl から全 voice を解放する。 */
void StopAllVoicesLocked(CXAudio2Backend::FImpl& Implementation) noexcept
{
    for (u32 Index = 0u; Index < Implementation.MaxVoices; ++Index) {
        FVoiceSlot& Slot = Implementation.Slots[Index];
        if (Slot.bActive) {
            DestroySlot(Implementation, Slot);
        }
    }
    Implementation.ActiveVoiceCount = 0u;
}

/** clip から検証済み WAVEFORMATEX を作る。 */
bool FillWaveFormat(const FAudioClipDesc& Clip, WAVEFORMATEX& WaveFormat) noexcept
{
    if (Clip.channel_count == 0u || Clip.channel_count > XAUDIO2_MAX_AUDIO_CHANNELS ||
        Clip.sample_rate < XAUDIO2_MIN_SAMPLE_RATE || Clip.sample_rate > XAUDIO2_MAX_SAMPLE_RATE) {
        return false;
    }

    WaveFormat = WAVEFORMATEX{};
    WaveFormat.nChannels = static_cast<WORD>(Clip.channel_count);
    WaveFormat.nSamplesPerSec = Clip.sample_rate;
    switch (Clip.format) {
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

    const u32 BlockAlign = (static_cast<u32>(WaveFormat.nChannels) * WaveFormat.wBitsPerSample) / 8u;
    if (BlockAlign == 0u || BlockAlign > 0xFFFFu) {
        return false;
    }

    WaveFormat.nBlockAlign = static_cast<WORD>(BlockAlign);
    WaveFormat.nAvgBytesPerSec = WaveFormat.nSamplesPerSec * BlockAlign;
    WaveFormat.cbSize = 0u;
    return true;
}

/** voice と保持中の PCM コピーを完全に破棄する。 */
void DestroySlot(CXAudio2Backend::FImpl& Implementation, FVoiceSlot& Slot) noexcept
{
    if (Slot.Voice != nullptr) {
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
    if (Implementation.ResidentBufferBytes >= Slot.ReservedBufferBytes) {
        Implementation.ResidentBufferBytes -= Slot.ReservedBufferBytes;
    } else {
        Implementation.ResidentBufferBytes = 0u;
    }
    Slot.Buffer.Empty();
    Slot.Handle = kInvalidAudioVoice;
    Slot.ReservedBufferBytes = 0u;
    Slot.SourceChannels = 0u;
    Slot.bActive = false;
    Slot.bLooped = false;
}

/** mutex 取得済みの pool から指定ハンドルを探す。 */
FVoiceSlot* FindVoiceSlot(CXAudio2Backend::FImpl& Implementation, FAudioVoiceHandle Voice) noexcept
{
    for (u32 Index = 0u; Index < Implementation.MaxVoices; ++Index) {
        FVoiceSlot& Slot = Implementation.Slots[Index];
        if (Slot.bActive && Slot.Handle == Voice) {
            return &Slot;
        }
    }
    return nullptr;
}

/** mutex 取得済みの pimpl で一発またはループ再生を開始する。 */
FAudioVoiceHandle PlayInternal(CXAudio2Backend::FImpl& Implementation, const FAudioClipDesc& Clip, f32 Volume,
                               f32 Pitch, bool bLoop) noexcept
{
    if (!Implementation.bInitialized || Implementation.XAudio2 == nullptr || Clip.pcm_data == nullptr ||
        Clip.pcm_size == 0u || Clip.pcm_size > static_cast<u64>(~u32(0))) {
        return kInvalidAudioVoice;
    }

    WAVEFORMATEX WaveFormat{};
    if (!FillWaveFormat(Clip, WaveFormat) || (Clip.pcm_size % static_cast<u64>(WaveFormat.nBlockAlign)) != 0u) {
        ACS_LOG_WARN("CXAudio2Backend::Play: invalid clip (channels=%u, rate=%u, format=%u, bytes=%llu)",
                     Clip.channel_count, Clip.sample_rate, static_cast<u32>(Clip.format),
                     static_cast<unsigned long long>(Clip.pcm_size));
        return kInvalidAudioVoice;
    }

    u32 SlotIndex = Implementation.MaxVoices;
    for (u32 Index = 0u; Index < Implementation.MaxVoices; ++Index) {
        if (!Implementation.Slots[Index].bActive) {
            SlotIndex = Index;
            break;
        }
    }
    if (SlotIndex == Implementation.MaxVoices) {
        ACS_LOG_WARN("CXAudio2Backend::Play: voice pool full (max_voices=%u)", Implementation.MaxVoices);
        return kInvalidAudioVoice;
    }

    const FAudioVoiceHandle Handle = AcquireAudioVoiceHandle();
    if (!Handle.IsValid()) {
        ACS_LOG_ERROR("CXAudio2Backend::Play: 32-bit voice handle space exhausted");
        return kInvalidAudioVoice;
    }

    FVoiceSlot& Slot = Implementation.Slots[SlotIndex];
    IXAudio2SourceVoice* SourceVoice = nullptr;
    HRESULT Result = Implementation.XAudio2->CreateSourceVoice(&SourceVoice, &WaveFormat, 0u, kMaximumPitch);
    if (FAILED(Result) || SourceVoice == nullptr) {
        if (SourceVoice != nullptr) {
            SourceVoice->DestroyVoice();
        }
        ACS_LOG_WARN("CXAudio2Backend::Play: CreateSourceVoice failed hr=0x%08x", static_cast<u32>(Result));
        return kInvalidAudioVoice;
    }

    if (Implementation.ResidentBufferBytes > kXAudio2BackendResidentBufferBudgetBytes ||
        Clip.pcm_size > kXAudio2BackendResidentBufferBudgetBytes - Implementation.ResidentBufferBytes) {
        SourceVoice->DestroyVoice();
        ACS_LOG_WARN("CXAudio2Backend::Play: resident PCM budget exceeded "
                     "(requested=%llu, resident=%llu, budget=%llu)",
                     static_cast<unsigned long long>(Clip.pcm_size),
                     static_cast<unsigned long long>(Implementation.ResidentBufferBytes),
                     static_cast<unsigned long long>(kXAudio2BackendResidentBufferBudgetBytes));
        return kInvalidAudioVoice;
    }

    const usize ByteCount = static_cast<usize>(Clip.pcm_size);
    if (!Slot.Buffer.TrySetNum(ByteCount)) {
        SourceVoice->DestroyVoice();
        Slot.Buffer.Empty();
        ACS_LOG_WARN("CXAudio2Backend::Play: PCM copy allocation failed (bytes=%llu)",
                     static_cast<unsigned long long>(Clip.pcm_size));
        return kInvalidAudioVoice;
    }
    const u64 ReservedBufferBytes = static_cast<u64>(Slot.Buffer.Max());
    if (ReservedBufferBytes > kXAudio2BackendResidentBufferBudgetBytes - Implementation.ResidentBufferBytes) {
        SourceVoice->DestroyVoice();
        Slot.Buffer.Empty();
        ACS_LOG_WARN("CXAudio2Backend::Play: allocated PCM capacity exceeds resident budget");
        return kInvalidAudioVoice;
    }
    MemCopy(Slot.Buffer.GetData(), Clip.pcm_data, ByteCount);

    XAUDIO2_BUFFER Buffer{};
    Buffer.AudioBytes = static_cast<UINT32>(ByteCount);
    Buffer.pAudioData = Slot.Buffer.GetData();
    Buffer.Flags = XAUDIO2_END_OF_STREAM;
    Buffer.LoopCount = bLoop ? XAUDIO2_LOOP_INFINITE : 0u;

    Result = SourceVoice->SubmitSourceBuffer(&Buffer);
    if (FAILED(Result)) {
        SourceVoice->DestroyVoice();
        Slot.Buffer.Empty();
        ACS_LOG_WARN("CXAudio2Backend::Play: SubmitSourceBuffer failed hr=0x%08x", static_cast<u32>(Result));
        return kInvalidAudioVoice;
    }

    Result = SourceVoice->SetVolume(ClampVolume(Volume), XAUDIO2_COMMIT_NOW);
    if (FAILED(Result)) {
        SourceVoice->DestroyVoice();
        Slot.Buffer.Empty();
        ACS_LOG_WARN("CXAudio2Backend::Play: SetVolume failed hr=0x%08x", static_cast<u32>(Result));
        return kInvalidAudioVoice;
    }
    Result = SourceVoice->SetFrequencyRatio(ClampPitch(Pitch), XAUDIO2_COMMIT_NOW);
    if (FAILED(Result)) {
        SourceVoice->DestroyVoice();
        Slot.Buffer.Empty();
        ACS_LOG_WARN("CXAudio2Backend::Play: SetFrequencyRatio failed hr=0x%08x", static_cast<u32>(Result));
        return kInvalidAudioVoice;
    }
    Result = SourceVoice->Start(0);
    if (FAILED(Result)) {
        SourceVoice->DestroyVoice();
        Slot.Buffer.Empty();
        ACS_LOG_WARN("CXAudio2Backend::Play: Start failed hr=0x%08x", static_cast<u32>(Result));
        return kInvalidAudioVoice;
    }

    Slot.Voice = SourceVoice;
    Slot.Handle = Handle;
    Slot.ReservedBufferBytes = ReservedBufferBytes;
    Slot.SourceChannels = Clip.channel_count;
    Slot.bActive = true;
    Slot.bLooped = bLoop;
    Implementation.ResidentBufferBytes += ReservedBufferBytes;
    ++Implementation.ActiveVoiceCount;
    return Handle;
}

} // namespace

CXAudio2Backend::CXAudio2Backend() noexcept = default;

CXAudio2Backend::~CXAudio2Backend() noexcept
{
    Shutdown();
}

bool CXAudio2Backend::IsShutdownRequested() const noexcept
{
    return m_ShutdownRequests.Load(EMemoryOrder::Acquire) != 0u;
}

TResult<void> CXAudio2Backend::Init(u32 MaxVoices) noexcept
{
    if (IsShutdownRequested()) {
        return ACS_ERR(Generic, kSubAudioNotInitialized, "CXAudio2Backend::Init: shutdown is in progress");
    }
    if (MaxVoices == 0u || MaxVoices > kXAudio2BackendMaximumVoiceCount) {
        return ACS_ERR(Generic, kSubAudioInvalidArgs, "CXAudio2Backend::Init: MaxVoices exceeds the supported range");
    }

    FScopedExclusiveLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested()) {
        return ACS_ERR(Generic, kSubAudioNotInitialized, "CXAudio2Backend::Init: shutdown is in progress");
    }
    if (m_Impl != nullptr) {
        return ACS_ERR(Generic, kSubAudioAlreadyInitialized, "CXAudio2Backend::Init: already initialized");
    }

    m_Impl = new (std::nothrow) FImpl();
    if (m_Impl == nullptr) {
        return ACS_ERR(Memory, kSubAudioOutOfMemory, "CXAudio2Backend::Init: state allocation failed");
    }

    HRESULT Result = ::CoIncrementMTAUsage(&m_Impl->MtaUsageCookie);
    if (FAILED(Result)) {
        ShutdownUnlocked();
        return ACS_ERR_OS(Generic, kSubAudioComInitFailed, "CXAudio2Backend::Init: CoIncrementMTAUsage failed",
                          static_cast<u32>(Result));
    }
    m_Impl->bMtaUsageAcquired = true;

    Result = ::XAudio2Create(&m_Impl->XAudio2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(Result)) {
        ShutdownUnlocked();
        return ACS_ERR_OS(Generic, kSubAudioCreateFailed, "CXAudio2Backend::Init: XAudio2Create failed",
                          static_cast<u32>(Result));
    }

    Result = m_Impl->XAudio2->CreateMasteringVoice(&m_Impl->MasteringVoice);
    if (FAILED(Result)) {
        ShutdownUnlocked();
        return ACS_ERR_OS(Generic, kSubAudioMasterVoiceFailed, "CXAudio2Backend::Init: CreateMasteringVoice failed",
                          static_cast<u32>(Result));
    }

    XAUDIO2_VOICE_DETAILS MasteringDetails{};
    m_Impl->MasteringVoice->GetVoiceDetails(&MasteringDetails);
    m_Impl->MasteringInputChannels = MasteringDetails.InputChannels;
    DWORD MasteringChannelMask = 0u;
    Result = m_Impl->MasteringVoice->GetChannelMask(&MasteringChannelMask);
    if (SUCCEEDED(Result) && MasteringChannelMask != 0u) {
        m_Impl->MasteringChannelMask = static_cast<u32>(MasteringChannelMask);
        m_Impl->bMasteringChannelMaskValid = true;
    } else if (SUCCEEDED(Result)) {
        ACS_LOG_WARN("CXAudio2Backend::Init: GetChannelMask returned zero; pan disabled");
    } else {
        ACS_LOG_WARN("CXAudio2Backend::Init: GetChannelMask failed; pan disabled hr=0x%08x", static_cast<u32>(Result));
    }

    if (!m_Impl->Slots.TrySetNum(MaxVoices)) {
        ShutdownUnlocked();
        return ACS_ERR(Memory, kSubAudioOutOfMemory, "CXAudio2Backend::Init: voice pool allocation failed");
    }
    m_Impl->MaxVoices = MaxVoices;
    m_Impl->bInitialized = true;
    ACS_LOG_INFO("CXAudio2Backend: initialized (max_voices=%u)", MaxVoices);
    return Ok();
}

void CXAudio2Backend::Shutdown() noexcept
{
    FScopedBackendShutdownRequest ShutdownRequest(m_ShutdownRequests);
    FScopedExclusiveLock LifecycleLock(m_LifecycleLock);
    if (m_Impl == nullptr) {
        return;
    }
    ShutdownUnlocked();
}

void CXAudio2Backend::ShutdownUnlocked() noexcept
{
    FImpl* const Implementation = m_Impl;
    {
        FScopedLock StateLock(Implementation->StateMutex);
        StopAllVoicesLocked(*Implementation);
        Implementation->Slots.Empty();
        Implementation->MaxVoices = 0u;
        Implementation->ResidentBufferBytes = 0u;
        Implementation->MasteringInputChannels = 0u;
        Implementation->MasteringChannelMask = 0u;
        Implementation->bMasteringChannelMaskValid = false;

        if (Implementation->MasteringVoice != nullptr) {
            Implementation->MasteringVoice->DestroyVoice();
            Implementation->MasteringVoice = nullptr;
        }
        if (Implementation->XAudio2 != nullptr) {
            Implementation->XAudio2->Release();
            Implementation->XAudio2 = nullptr;
        }
        if (Implementation->bMtaUsageAcquired) {
            HRESULT Result = S_OK;
#if defined(ACS_XAUDIO2_BACKEND_TEST_HOOKS)
            if (!Implementation->bTestMtaUsage) {
                Result = ::CoDecrementMTAUsage(Implementation->MtaUsageCookie);
            }
#else
            Result = ::CoDecrementMTAUsage(Implementation->MtaUsageCookie);
#endif
            if (FAILED(Result)) {
                ACS_LOG_ERROR("CXAudio2Backend::Shutdown: CoDecrementMTAUsage failed (hr=0x%08lx)",
                              static_cast<unsigned long>(Result));
                ACS_ASSERT(false && "CXAudio2Backend MTA usage release failed");
            }
            Implementation->MtaUsageCookie = nullptr;
            Implementation->bMtaUsageAcquired = false;
        }
        Implementation->bInitialized = false;
    }

    delete Implementation;
    m_Impl = nullptr;
}

bool CXAudio2Backend::IsInitialized() const noexcept
{
    if (IsShutdownRequested()) {
        return false;
    }
    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested()) {
        return false;
    }
    FImpl* const Implementation = m_Impl;
    if (Implementation == nullptr) {
        return false;
    }
    FScopedLock StateLock(Implementation->StateMutex);
    return Implementation->bInitialized;
}

FAudioVoiceHandle CXAudio2Backend::PlayOneShot(const FAudioClipDesc& Clip, f32 Volume, f32 Pitch) noexcept
{
    if (IsShutdownRequested()) {
        return kInvalidAudioVoice;
    }
    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested()) {
        return kInvalidAudioVoice;
    }
    FImpl* const Implementation = m_Impl;
    if (Implementation == nullptr) {
        return kInvalidAudioVoice;
    }
    FScopedLock StateLock(Implementation->StateMutex);
    return PlayInternal(*Implementation, Clip, Volume, Pitch, false);
}

FAudioVoiceHandle CXAudio2Backend::PlayLooped(const FAudioClipDesc& Clip, f32 Volume, f32 Pitch) noexcept
{
    if (IsShutdownRequested()) {
        return kInvalidAudioVoice;
    }
    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested()) {
        return kInvalidAudioVoice;
    }
    FImpl* const Implementation = m_Impl;
    if (Implementation == nullptr) {
        return kInvalidAudioVoice;
    }
    FScopedLock StateLock(Implementation->StateMutex);
    return PlayInternal(*Implementation, Clip, Volume, Pitch, true);
}

void CXAudio2Backend::StopVoice(FAudioVoiceHandle Voice) noexcept
{
    if (IsShutdownRequested() || !Voice.IsValid()) {
        return;
    }
    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested()) {
        return;
    }
    FImpl* const Implementation = m_Impl;
    if (Implementation == nullptr) {
        return;
    }
    FScopedLock StateLock(Implementation->StateMutex);
    if (!Implementation->bInitialized) {
        return;
    }
    FVoiceSlot* const Slot = FindVoiceSlot(*Implementation, Voice);
    if (Slot == nullptr) {
        return;
    }
    DestroySlot(*Implementation, *Slot);
    if (Implementation->ActiveVoiceCount > 0u) {
        --Implementation->ActiveVoiceCount;
    }
}

void CXAudio2Backend::SetVoiceVolume(FAudioVoiceHandle Voice, f32 Volume) noexcept
{
    if (IsShutdownRequested() || !Voice.IsValid()) {
        return;
    }
    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested()) {
        return;
    }
    FImpl* const Implementation = m_Impl;
    if (Implementation == nullptr) {
        return;
    }
    FScopedLock StateLock(Implementation->StateMutex);
    if (!Implementation->bInitialized) {
        return;
    }
    FVoiceSlot* const Slot = FindVoiceSlot(*Implementation, Voice);
    if (Slot == nullptr || Slot->Voice == nullptr) {
        return;
    }
    Slot->Voice->SetVolume(ClampVolume(Volume));
}

void CXAudio2Backend::StopAllVoices() noexcept
{
    if (IsShutdownRequested()) {
        return;
    }
    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested()) {
        return;
    }
    FImpl* const Implementation = m_Impl;
    if (Implementation == nullptr) {
        return;
    }
    FScopedLock StateLock(Implementation->StateMutex);
    StopAllVoicesLocked(*Implementation);
}

u32 CXAudio2Backend::ActiveVoiceCount() const noexcept
{
    if (IsShutdownRequested()) {
        return 0u;
    }
    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested()) {
        return 0u;
    }
#if defined(ACS_XAUDIO2_BACKEND_TEST_HOOKS)
    WaitForBackendLifecycleTestGate();
#endif
    FImpl* const Implementation = m_Impl;
    if (Implementation == nullptr) {
        return 0u;
    }
    FScopedLock StateLock(Implementation->StateMutex);
    return Implementation->ActiveVoiceCount;
}

void CXAudio2Backend::Tick(f32 DeltaSeconds) noexcept
{
    (void)DeltaSeconds;
    if (IsShutdownRequested()) {
        return;
    }
    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested()) {
        return;
    }
    FImpl* const Implementation = m_Impl;
    if (Implementation == nullptr) {
        return;
    }
    FScopedLock StateLock(Implementation->StateMutex);
    if (!Implementation->bInitialized) {
        return;
    }

    for (u32 Index = 0u; Index < Implementation->MaxVoices; ++Index) {
        FVoiceSlot& Slot = Implementation->Slots[Index];
        if (!Slot.bActive || Slot.bLooped || Slot.Voice == nullptr) {
            continue;
        }

        XAUDIO2_VOICE_STATE State{};
        Slot.Voice->GetState(&State);
        if (State.BuffersQueued == 0u) {
            DestroySlot(*Implementation, Slot);
            if (Implementation->ActiveVoiceCount > 0u) {
                --Implementation->ActiveVoiceCount;
            }
        }
    }
}

void CXAudio2Backend::SetVoiceParameters(FAudioVoiceHandle Voice, f32 Volume, f32 Pan, f32 Pitch) noexcept
{
    if (IsShutdownRequested() || !Voice.IsValid()) {
        return;
    }
    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested()) {
        return;
    }
#if defined(ACS_XAUDIO2_BACKEND_TEST_HOOKS)
    WaitForBackendLifecycleTestGate();
#endif
    FImpl* const Implementation = m_Impl;
    if (Implementation == nullptr) {
        return;
    }
    FScopedLock StateLock(Implementation->StateMutex);
    if (!Implementation->bInitialized) {
        return;
    }
    FVoiceSlot* const Slot = FindVoiceSlot(*Implementation, Voice);
    if (Slot == nullptr || Slot->Voice == nullptr) {
        return;
    }

    HRESULT Result = Slot->Voice->SetVolume(ClampVolume(Volume), XAUDIO2_COMMIT_NOW);
    if (FAILED(Result)) {
        ACS_LOG_WARN("CXAudio2Backend::SetVoiceParameters: SetVolume failed voice=%u hr=0x%08x", Voice.PackedValue(),
                     static_cast<u32>(Result));
    }

    Result = Slot->Voice->SetFrequencyRatio(ClampPitch(Pitch), XAUDIO2_COMMIT_NOW);
    if (FAILED(Result)) {
        ACS_LOG_WARN("CXAudio2Backend::SetVoiceParameters: SetFrequencyRatio failed voice=%u hr=0x%08x",
                     Voice.PackedValue(), static_cast<u32>(Result));
    }

    f32 Matrix[XAUDIO2_MAX_AUDIO_CHANNELS]{};
    if (Implementation->MasteringVoice == nullptr || !Implementation->bMasteringChannelMaskValid ||
        !BuildMonoPanMatrix(Slot->SourceChannels, Implementation->MasteringInputChannels,
                            Implementation->MasteringChannelMask, Pan, Matrix, XAUDIO2_MAX_AUDIO_CHANNELS)) {
        return;
    }

    Result = Slot->Voice->SetOutputMatrix(Implementation->MasteringVoice, 1u, Implementation->MasteringInputChannels,
                                          Matrix, XAUDIO2_COMMIT_NOW);
    if (FAILED(Result)) {
        ACS_LOG_WARN("CXAudio2Backend::SetVoiceParameters: SetOutputMatrix failed voice=%u hr=0x%08x",
                     Voice.PackedValue(), static_cast<u32>(Result));
    }
}

void CXAudio2Backend::SetMasterVolume(f32 Volume) noexcept
{
    if (IsShutdownRequested()) {
        return;
    }
    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested()) {
        return;
    }
    FImpl* const Implementation = m_Impl;
    if (Implementation == nullptr) {
        return;
    }
    FScopedLock StateLock(Implementation->StateMutex);
    if (Implementation->MasteringVoice != nullptr) {
        Implementation->MasteringVoice->SetVolume(ClampVolume(Volume));
    }
}

#if defined(ACS_XAUDIO2_BACKEND_TEST_HOOKS)
bool CXAudio2Backend::BuildMonoPanMatrixForTesting(u32 SourceChannels, u32 DestinationChannels,
                                                   u32 DestinationChannelMask, f32 Pan, f32* OutMatrix,
                                                   u32 MatrixCapacity) noexcept
{
    return BuildMonoPanMatrix(SourceChannels, DestinationChannels, DestinationChannelMask, Pan, OutMatrix,
                              MatrixCapacity);
}

void CXAudio2Backend::NormalizeVoiceParametersForTesting(f32 Volume, f32 Pan, f32 Pitch, f32& OutVolume, f32& OutPan,
                                                         f32& OutPitch) noexcept
{
    OutVolume = ClampVolume(Volume);
    OutPan = ClampPan(Pan);
    OutPitch = ClampPitch(Pitch);
}

bool CXAudio2Backend::DestroySlotResetsSourceChannelsForTesting() noexcept
{
    FImpl Implementation;
    FVoiceSlot Slot;
    Slot.SourceChannels = 2u;
    DestroySlot(Implementation, Slot);
    return Slot.SourceChannels == 0u;
}

TResult<void> CXAudio2Backend::InitializeLifecycleTestState() noexcept
{
    if (IsShutdownRequested()) {
        return ACS_ERR(Generic, kSubAudioNotInitialized, "CXAudio2Backend test shutdown is in progress");
    }
    FScopedExclusiveLock LifecycleLock(m_LifecycleLock);
    if (IsShutdownRequested()) {
        return ACS_ERR(Generic, kSubAudioNotInitialized, "CXAudio2Backend test shutdown is in progress");
    }
    if (m_Impl != nullptr) {
        return ACS_ERR(Generic, kSubAudioAlreadyInitialized, "CXAudio2Backend test state already initialized");
    }

    m_Impl = new (std::nothrow) FImpl();
    if (m_Impl == nullptr) {
        return ACS_ERR(Memory, kSubAudioOutOfMemory, "CXAudio2Backend test state allocation failed");
    }
    m_Impl->bMtaUsageAcquired = true;
    m_Impl->bTestMtaUsage = true;
    m_Impl->bInitialized = true;
    return Ok();
}

void CXAudio2Backend::ConfigureLifecycleOperationTestGate(TAtomic<u32>* Entered, TAtomic<u32>* Release) noexcept
{
    g_BackendLifecycleTestRelease.Store(Release, EMemoryOrder::Release);
    g_BackendLifecycleTestEntered.Store(Entered, EMemoryOrder::Release);
}

bool CXAudio2Backend::IsShutdownRequestedForTesting() const noexcept
{
    return IsShutdownRequested();
}

bool CXAudio2Backend::HasLifecycleStateForTesting() const noexcept
{
    FScopedSharedLock LifecycleLock(m_LifecycleLock);
    return m_Impl != nullptr;
}
#endif

} // namespace acs::game
