// SPDX-License-Identifier: Apache-2.0
// XAudio2 ベースの音声エンジン実装
#include "audio/AudioEngine.h"
#include "foundation/Platform.h"
#include "foundation/Log.h"
#include "threading/ScopedLock.h"

#include <xaudio2.h>

namespace acs {

namespace {

constexpr u32 kMaxVoices = 64;  // 同時再生上限

// 1 個の発音スロット
struct VoiceSlot {
    IXAudio2SourceVoice* voice      = nullptr;
    TArray<byte>          buffer_copy;     // XAudio2 が再生中はバッファを保持
    u32                  generation = 0;
    bool                 in_use     = false;
};

} // namespace

struct FAudioEngine::Impl {
    IXAudio2*               xaudio2          = nullptr;
    IXAudio2MasteringVoice* mastering        = nullptr;
    bool                    com_initialized  = false;

    FMutex                   lock;
    VoiceSlot               slots[kMaxVoices] {};
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
    if (SUCCEEDED(hr)) m_Impl->com_initialized = true;

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
        ::CoUninitialize();
        m_Impl->com_initialized = false;
    }
    delete m_Impl;
    m_Impl = nullptr;
}

namespace {
void DestroySlot(VoiceSlot& slot) noexcept;   // 前方宣言（FindFreeSlot が回収に使う）

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

    u32 idx = FindFreeSlot(*m_Impl);
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
    if (FAILED(hr)) return kInvalidSound;

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

    slot.voice->SubmitSourceBuffer(&xb);
    slot.voice->SetVolume(volume);
    slot.voice->Start(0);
    slot.in_use = true;
    ++m_Impl->active_count;

    SoundHandle h{ idx, slot.generation };
    return h;
}

namespace {
void DestroySlot(VoiceSlot& slot) noexcept {
    if (slot.voice) {
        slot.voice->Stop(0);
        slot.voice->FlushSourceBuffers();
        slot.voice->DestroyVoice();
        slot.voice = nullptr;
    }
    slot.buffer_copy.Clear();
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
