// SPDX-License-Identifier: Apache-2.0
// XAudio2 ベースの音声エンジン実装
#include "audio/AudioEngine.h"
#include "foundation/Assert.h"
#include "foundation/Platform.h"
#include "foundation/Log.h"
#include "threading/ScopedLock.h"

#include <xaudio2.h>

namespace acs {

namespace {

/** 同時に確保できる発音スロット (ソースボイス) の上限。 */
constexpr u32 kMaxVoices = 64;

/**
 * 1 個の発音スロット (XAudio2 ソースボイス + 再生用データ)。
 */
struct VoiceSlot {
    /** XAudio2 ソースボイス (未使用時は nullptr)。 */
    IXAudio2SourceVoice* voice      = nullptr;

    /** 再生中に参照され続けるサンプルデータのコピー。 */
    TArray<byte>          buffer_copy;

    /** スロットの世代 (解放のたびに加算し、古いハンドルを無効化)。 */
    u32                  generation = 0;

    /** このスロットが現在使用中か。 */
    bool                 in_use     = false;
};

} // namespace

/**
 * FAudioEngine の pimpl 実装 (XAudio2 ハンドルとスロット表を保持)。
 */
struct FAudioEngine::Impl {
    /** XAudio2 エンジン本体。 */
    IXAudio2*               xaudio2          = nullptr;

    /** 最終出力のマスタリングボイス。 */
    IXAudio2MasteringVoice* mastering        = nullptr;

    /** このインスタンスが CoInitializeEx を成功させたか (後始末判定用)。 */
    bool                    com_initialized  = false;

    /** COM 参照数を増やしたスレッド。CoUninitialize は同じスレッドでのみ呼べる。 */
    DWORD com_thread_identifier = 0;

    /** スロット表と active_count を保護する mutex。 */
    FMutex                   lock;

    /** 発音スロット表 (最大 kMaxVoices 個)。 */
    VoiceSlot               slots[kMaxVoices] {};

    /** 使用中スロット数。 */
    u32                     active_count     = 0;
};

FAudioEngine::~FAudioEngine() noexcept {
    Shutdown();
}

TResult<void> FAudioEngine::Init() noexcept {
    if (m_Impl) return ACS_ERR(Generic, 1, "FAudioEngine already initialized");
    m_Impl = new Impl();

    // COM 初期化（マルチスレッド形式、XAudio2 が要求）
    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        // S_FALSE でも COM の参照数は増えるため、同じスレッドで解放が必要。
        m_Impl->com_initialized = true;
        m_Impl->com_thread_identifier = ::GetCurrentThreadId();
    } else if (hr == RPC_E_CHANGED_MODE) {
        ACS_LOG_WARN("FAudioEngine::Init: COM already uses a different threading mode");
    } else {
        Shutdown();
        return ACS_ERR_OS(Generic, 4, "CoInitializeEx failed", static_cast<u32>(hr));
    }

    // XAudio2 エンジン作成
    hr = ::XAudio2Create(&m_Impl->xaudio2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        Shutdown();
        return ACS_ERR_OS(Generic, 2, "XAudio2Create failed", static_cast<u32>(hr));
    }

    // マスタリングボイス（最終出力）
    hr = m_Impl->xaudio2->CreateMasteringVoice(&m_Impl->mastering);
    if (FAILED(hr)) {
        Shutdown();
        return ACS_ERR_OS(Generic, 3, "CreateMasteringVoice failed", static_cast<u32>(hr));
    }

    ACS_LOG_INFO("FAudioEngine initialized (XAudio2)");
    return Ok();
}

void FAudioEngine::Shutdown() noexcept {
    if (!m_Impl) return;
    StopAll();
    if (m_Impl->mastering) {
        m_Impl->mastering->DestroyVoice();
        m_Impl->mastering = nullptr;
    }
    if (m_Impl->xaudio2) {
        m_Impl->xaudio2->Release();
        m_Impl->xaudio2 = nullptr;
    }
    if (m_Impl->com_initialized) {
        const DWORD current_thread_identifier = ::GetCurrentThreadId();
        if (current_thread_identifier == m_Impl->com_thread_identifier) {
            ::CoUninitialize();
        } else {
            ACS_LOG_ERROR("FAudioEngine::Shutdown must run on the Init thread "
                          "(init=%lu, current=%lu)",
                          static_cast<unsigned long>(m_Impl->com_thread_identifier),
                          static_cast<unsigned long>(current_thread_identifier));
            ACS_ASSERT(false && "FAudioEngine COM shutdown thread mismatch");
        }
        m_Impl->com_initialized = false;
        m_Impl->com_thread_identifier = 0;
    }
    delete m_Impl;
    m_Impl = nullptr;
}

namespace {
/**
 * スロットのボイスを停止・破棄し、世代を進めて空きに戻す。
 *
 * @param slot 解放するスロット。
 */
void DestroySlot(VoiceSlot& slot) noexcept;   // 前方宣言（FindFreeSlot が回収に使う）

/**
 * 再生終了した一発再生スロットを回収しつつ、空きスロット番号を探す。
 *
 * @details
 * まず BuffersQueued==0 になった一発再生ボイスを破棄して空きに戻す
 * (これを怠ると kMaxVoices 回再生した時点でスロットが尽きる)。ループ再生は
 * BuffersQueued が 0 にならないため回収対象外。
 * @param impl 対象エンジンの pimpl。
 * @return 使える空きスロット番号 (満杯なら 0xFFFFFFFF)。
 */
u32 FindFreeSlot(FAudioEngine::Impl& impl) noexcept {
    // 自然に再生が終わった一発再生ボイスを回収する。これをしないと、
    // 一発再生を kMaxVoices 回呼んだ時点でスロットが尽き、以降の Play が
    // 無音になる。ループ再生は BuffersQueued が 0 にならず回収されない。
    for (u32 i = 0; i < kMaxVoices; ++i) {
        VoiceSlot& s = impl.slots[i];
        if (s.in_use && s.voice) {
            XAUDIO2_VOICE_STATE st{};
            s.voice->GetState(&st);
            if (st.BuffersQueued == 0) {
                DestroySlot(s);
                if (impl.active_count > 0) --impl.active_count;
            }
        }
    }
    for (u32 i = 0; i < kMaxVoices; ++i) {
        if (!impl.slots[i].in_use) return i;
    }
    return 0xFFFFFFFFu;
}
} // namespace

SoundHandle FAudioEngine::Play(const FAudioAsset& asset, f32 volume, bool loop) noexcept {
    if (!m_Impl || !m_Impl->xaudio2) return kInvalidSound;
    if (asset.SampleByteCount() == 0) return kInvalidSound;

    FScopedLock lk(m_Impl->lock);

    const u32 idx = FindFreeSlot(*m_Impl);
    if (idx == 0xFFFFFFFFu) return kInvalidSound;
    VoiceSlot& slot = m_Impl->slots[idx];

    // ソースボイスのフォーマット設定
    WAVEFORMATEX wf{};
    wf.wFormatTag      = (asset.EFormat() == ESampleFormat::PCM_F32) ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    wf.nChannels       = asset.Channels();
    wf.nSamplesPerSec  = asset.SampleRate();
    wf.wBitsPerSample  = (asset.EFormat() == ESampleFormat::PCM_F32) ? 32 : 16;
    wf.nBlockAlign     = (wf.nChannels * wf.wBitsPerSample) / 8;
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    wf.cbSize          = 0;

    HRESULT hr = m_Impl->xaudio2->CreateSourceVoice(&slot.voice, &wf);
    if (FAILED(hr) || slot.voice == nullptr) {
        if (slot.voice != nullptr) DestroySlot(slot);
        return kInvalidSound;
    }

    // サンプルデータを再生中保持するためコピー
    slot.buffer_copy.Resize(asset.SampleByteCount());
    for (usize i = 0; i < asset.SampleByteCount(); ++i)
        slot.buffer_copy[i] = asset.Samples()[i];

    XAUDIO2_BUFFER xb{};
    xb.AudioBytes = static_cast<UINT32>(slot.buffer_copy.Size());
    xb.pAudioData = slot.buffer_copy.Data();
    xb.Flags = XAUDIO2_END_OF_STREAM;
    xb.LoopCount = loop ? XAUDIO2_LOOP_INFINITE : 0;

    if (volume < 0) volume = 0;
    if (volume > 1) volume = 1;

    hr = slot.voice->SubmitSourceBuffer(&xb);
    if (FAILED(hr)) {
        DestroySlot(slot);
        return kInvalidSound;
    }
    slot.voice->SetVolume(volume);
    hr = slot.voice->Start(0);
    if (FAILED(hr)) {
        DestroySlot(slot);
        return kInvalidSound;
    }
    slot.in_use = true;
    ++m_Impl->active_count;

    const SoundHandle h{ idx, slot.generation };
    return h;
}

namespace {
/** スロットのボイスを停止・破棄し、世代を進めて空きに戻す。 */
void DestroySlot(VoiceSlot& slot) noexcept {
    if (slot.voice) {
        slot.voice->Stop(0);
        slot.voice->FlushSourceBuffers();
        slot.voice->DestroyVoice();
        slot.voice = nullptr;
    }
    slot.buffer_copy.ReleaseStorage();
    ++slot.generation;
    slot.in_use = false;
}
} // namespace

void FAudioEngine::Stop(SoundHandle h) noexcept {
    if (!m_Impl || !h.IsValid() || h.index >= kMaxVoices) return;
    FScopedLock lk(m_Impl->lock);
    VoiceSlot& slot = m_Impl->slots[h.index];
    if (!slot.in_use || slot.generation != h.generation) return;
    DestroySlot(slot);
    if (m_Impl->active_count > 0) --m_Impl->active_count;
}

void FAudioEngine::SetVolume(SoundHandle h, f32 volume) noexcept {
    if (!m_Impl || !h.IsValid() || h.index >= kMaxVoices) return;
    FScopedLock lk(m_Impl->lock);
    VoiceSlot& slot = m_Impl->slots[h.index];
    if (!slot.in_use || slot.generation != h.generation) return;
    if (volume < 0) volume = 0;
    if (volume > 1) volume = 1;
    if (slot.voice) slot.voice->SetVolume(volume);
}

void FAudioEngine::StopAll() noexcept {
    if (!m_Impl) return;
    FScopedLock lk(m_Impl->lock);
    for (u32 i = 0; i < kMaxVoices; ++i) {
        if (m_Impl->slots[i].in_use) DestroySlot(m_Impl->slots[i]);
    }
    m_Impl->active_count = 0;
}

void FAudioEngine::SetMasterVolume(f32 volume) noexcept {
    if (!m_Impl || !m_Impl->mastering) return;
    if (volume < 0) volume = 0;
    if (volume > 1) volume = 1;
    m_Impl->mastering->SetVolume(volume);
}

void FAudioEngine::PauseAll() noexcept {
    if (!m_Impl) return;
    FScopedLock lk(m_Impl->lock);
    for (u32 i = 0; i < kMaxVoices; ++i) {
        VoiceSlot& s = m_Impl->slots[i];
        if (s.in_use && s.voice) s.voice->Stop(0);
    }
}

void FAudioEngine::ResumeAll() noexcept {
    if (!m_Impl) return;
    FScopedLock lk(m_Impl->lock);
    for (u32 i = 0; i < kMaxVoices; ++i) {
        VoiceSlot& s = m_Impl->slots[i];
        if (s.in_use && s.voice) s.voice->Start(0);
    }
}

u32 FAudioEngine::ActiveCount() const noexcept {
    return m_Impl ? m_Impl->active_count : 0;
}

} // namespace acs
