// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — XAudio2Backend 実装 (Windows / XAudio2 / COM)
//
// 設計メモ:
//   ・pimpl で `<xaudio2.h>` (+ `<windows.h>`) を本 .cpp に閉じ込め、ヘッダ
//     公開面は ACS 既存型と前方宣言だけにする。
//   ・voice pool は固定容量。`Init(max_voices)` で確保し、Shutdown で全解放。
//   ・PlayOneShot/PlayLooped はサンプルデータを slot 内 TArray<byte> にコピー。
//     XAudio2 は SourceVoice 再生中に元バッファが消えると爆ぜるため、
//     ライフタイム管理を完全に自己完結させる。
//   ・generation 付き handle で slot 再利用時の use-after-free を防ぐ。
//   ・Tick で BuffersQueued==0 の一発再生 voice を回収。回収しないと
//     max_voices 回再生で slot 枯渇する。
//
// COM 初期化方針:
//   ・本 backend が `CoInitializeEx(MULTITHREADED)` を呼ぶ。
//   ・他所が既に init 済 (S_FALSE 戻り) でも問題なく動く ⇒ 自分が成功した
//     ときだけ Shutdown で `CoUninitialize` を呼ぶ。
//   ・他所が違うモード (RPC_E_CHANGED_MODE) を確立済なら、警告ログを残して
//     XAudio2 init はそのまま試みる (XAudio2 は両モードで動く)。
#include "gameframework/audio_backend/XAudio2Backend.h"

#include "container/Array.h"
#include "foundation/Log.h"

// ---- Win32 / XAudio2 ヘッダ (本 .cpp ローカル) -----------------------------
// <windows.h> マクロ汚染 (min/max など) を最小化するため WIN32_LEAN_AND_MEAN
// + NOMINMAX を付ける (ACS のビルドスクリプトは既にコマンドラインで両方定義
// しているので、ローカルの define は ifndef ガード付きで二重定義を避ける)。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>   // CoInitializeEx / CoUninitialize
#include <xaudio2.h>   // IXAudio2 / IXAudio2SourceVoice / etc.

namespace acs::game {

// ----------------------------------------------------------------------------
// 内部 helpers / 定数
// ----------------------------------------------------------------------------
namespace {

// volume を 0.0..1.0 に clamp (XAudio2 自体は > 1 も受けるが歪むので抑える)。
f32 ClampVolume(f32 v) noexcept {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

// pitch を XAudio2 が受ける ratio に clamp。XAudio2 は SetFrequencyRatio で
// XAUDIO2_MIN_FREQ_RATIO (= 1/1024.0) .. XAUDIO2_MAX_FREQ_RATIO (= 1024.0) を
// 受けるが、現実用途は [0.25, 4.0] で十分。範囲外は警告 + clamp する。
f32 ClampPitch(f32 p) noexcept {
    constexpr f32 kMin = 0.25f;
    constexpr f32 kMax = 4.0f;
    if (p < kMin) return kMin;
    if (p > kMax) return kMax;
    return p;
}

// 1 個の発音 slot。
struct VoiceSlot {
    IXAudio2SourceVoice* voice    = nullptr;  // XAudio2 source voice
    TArray<byte>          buffer;              // 再生中保持する PCM コピー
    u8                   generation = 0;      // handle 検証用 (0..255 を循環)
    bool                 active    = false;   // true なら use 中
    bool                 looped    = false;   // ループ再生か (一発再生は Tick で回収)
};

} // namespace

// ----------------------------------------------------------------------------
// pimpl
// ----------------------------------------------------------------------------
struct XAudio2Backend::Impl {
    IXAudio2*               xaudio2          = nullptr;
    IXAudio2MasteringVoice* mastering        = nullptr;
    bool                    com_initialized  = false;  // 自分で CoInit した
    bool                    initialized      = false;  // フル init 済 (Init 成功)

    TArray<VoiceSlot>        slots;
    u32                     max_voices       = 0;
    u32                     active_count     = 0;
};

// ----------------------------------------------------------------------------
// construction / destruction
// ----------------------------------------------------------------------------

XAudio2Backend::XAudio2Backend() noexcept = default;

XAudio2Backend::~XAudio2Backend() noexcept {
    Shutdown();
}

// ----------------------------------------------------------------------------
// Init / Shutdown
// ----------------------------------------------------------------------------

TResult<void> XAudio2Backend::Init(u32 max_voices) noexcept {
    if (_impl != nullptr && _impl->initialized) {
        return ACS_ERR(Generic, kSubAudioAlreadyInitialized,
                       "XAudio2Backend::Init: already initialized");
    }
    if (max_voices == 0) {
        return ACS_ERR(Generic, kSubAudioInvalidArgs,
                       "XAudio2Backend::Init: max_voices=0 is invalid");
    }
    // slot index は 24bit に収まる必要 (handle 内で 24bit 制限)。
    // 1<<24 = 16777216 だが、現実 max_voices はせいぜい数千なので念のため check。
    if (max_voices >= (1u << 24)) {
        return ACS_ERR(Generic, kSubAudioInvalidArgs,
                       "XAudio2Backend::Init: max_voices too large (must be < 2^24)");
    }

    if (_impl == nullptr) {
        _impl = new Impl();
    }

    // ---- COM 初期化 -----
    // 既に他モジュールが COM init 済の可能性あり (S_FALSE / RPC_E_CHANGED_MODE)。
    // 自分が成功 (S_OK) したときだけ Shutdown で CoUninitialize を呼ぶ。
    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == S_OK) {
        _impl->com_initialized = true;
    } else if (hr == S_FALSE) {
        // 同モードで既に init 済。CoUninitialize は呼ばない。
        _impl->com_initialized = false;
    } else if (hr == RPC_E_CHANGED_MODE) {
        // 別モードで init 済。XAudio2 は両モードで動くので警告のみ。
        ACS_LOG_WARN("XAudio2Backend::Init: COM already initialized in a different "
                     "threading mode (RPC_E_CHANGED_MODE) — continuing");
        _impl->com_initialized = false;
    } else {
        Shutdown();
        return ACS_ERR_OS(Generic, kSubAudioComInitFailed,
                          "XAudio2Backend::Init: CoInitializeEx failed",
                          static_cast<u32>(hr));
    }

    // ---- XAudio2 エンジン作成 -----
    hr = ::XAudio2Create(&_impl->xaudio2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        Shutdown();
        return ACS_ERR_OS(Generic, kSubAudioCreateFailed,
                          "XAudio2Backend::Init: XAudio2Create failed",
                          static_cast<u32>(hr));
    }

    // ---- マスタリングボイス -----
    hr = _impl->xaudio2->CreateMasteringVoice(&_impl->mastering);
    if (FAILED(hr)) {
        Shutdown();
        return ACS_ERR_OS(Generic, kSubAudioMasterVoiceFailed,
                          "XAudio2Backend::Init: CreateMasteringVoice failed",
                          static_cast<u32>(hr));
    }

    // ---- voice pool -----
    _impl->max_voices = max_voices;
    _impl->slots.Resize(max_voices);  // VoiceSlot{} で初期化 (active=false)

    _impl->initialized = true;
    ACS_LOG_INFO("XAudio2Backend: initialized (max_voices=%u)", max_voices);
    return Ok();
}

void XAudio2Backend::Shutdown() noexcept {
    if (_impl == nullptr) return;

    // ---- 全 voice 停止 + destroy -----
    StopAllVoices();
    // slot TArray 自体は Impl のデストラクタで TArray<VoiceSlot> 経由解放される。
    _impl->slots.Clear();
    _impl->max_voices = 0;

    // ---- mastering voice -----
    if (_impl->mastering != nullptr) {
        _impl->mastering->DestroyVoice();
        _impl->mastering = nullptr;
    }

    // ---- XAudio2 engine -----
    if (_impl->xaudio2 != nullptr) {
        _impl->xaudio2->Release();
        _impl->xaudio2 = nullptr;
    }

    // ---- COM -----
    if (_impl->com_initialized) {
        ::CoUninitialize();
        _impl->com_initialized = false;
    }

    _impl->initialized = false;
    delete _impl;
    _impl = nullptr;
}

bool XAudio2Backend::IsInitialized() const noexcept {
    return _impl != nullptr && _impl->initialized;
}

// ----------------------------------------------------------------------------
// Play 系の共通実装 (一発 / ループ)
// ----------------------------------------------------------------------------
namespace {

// fmt → WAVEFORMATEX (Pcm16 / Pcm32Float のみ対応)。Wav 形式はここでは
// 受け付けない (将来 wav parser を入れる場合は別 path で処理する)。
// 成功時 true、未対応 fmt は false (呼び出し側で InvalidHandle を返す)。
bool FillWaveFormat(const AudioClipDesc& clip, WAVEFORMATEX& out) noexcept {
    out = WAVEFORMATEX{};
    out.nChannels      = static_cast<WORD>(clip.channel_count);
    out.nSamplesPerSec = clip.sample_rate;
    out.cbSize         = 0;

    switch (clip.format) {
    case EAudioFormat::Pcm16:
        out.wFormatTag     = WAVE_FORMAT_PCM;
        out.wBitsPerSample = 16;
        break;
    case EAudioFormat::Pcm32Float:
        out.wFormatTag     = WAVE_FORMAT_IEEE_FLOAT;
        out.wBitsPerSample = 32;
        break;
    case EAudioFormat::Wav:
        // 本 backend では未対応。AudioDirector / asset layer 側で事前に
        // raw PCM (Pcm16 / Pcm32Float) に展開してから渡してもらう想定。
        return false;
    }

    out.nBlockAlign     = static_cast<WORD>((out.nChannels * out.wBitsPerSample) / 8);
    out.nAvgBytesPerSec = out.nSamplesPerSec * out.nBlockAlign;
    return out.nBlockAlign != 0 && out.nChannels != 0 && out.nSamplesPerSec != 0;
}

// VoiceSlot を完全破棄する (XAudio2 voice + バッファコピー解放)。
// generation は increment して、古いハンドルでの再アクセスを無効化。
void DestroySlot(VoiceSlot& slot) noexcept {
    if (slot.voice != nullptr) {
        slot.voice->Stop(0);
        slot.voice->FlushSourceBuffers();
        slot.voice->DestroyVoice();
        slot.voice = nullptr;
    }
    slot.buffer.Clear();
    slot.active = false;
    slot.looped = false;
    ++slot.generation;  // 0..255 循環 (オーバーフローは UB ではない / u8 wrap)
}

} // namespace

// 共通の Play 関数 (一発 / ループの分岐は loop 引数で吸収)。
static AudioVoiceHandle PlayInternal(XAudio2Backend::Impl& impl,
                                     const AudioClipDesc& clip,
                                     f32 volume,
                                     f32 pitch,
                                     bool loop) noexcept {
    if (!impl.initialized) return kInvalidAudioVoice;
    if (clip.pcm_data == nullptr || clip.pcm_size == 0) return kInvalidAudioVoice;
    if (clip.channel_count == 0 || clip.sample_rate == 0) return kInvalidAudioVoice;

    WAVEFORMATEX wf{};
    if (!FillWaveFormat(clip, wf)) {
        ACS_LOG_WARN("XAudio2Backend::Play: unsupported clip format (channels=%u, "
                     "rate=%u, fmt=%u)", clip.channel_count, clip.sample_rate,
                     static_cast<u32>(clip.format));
        return kInvalidAudioVoice;
    }

    // 空きスロットを探す (Tick で完了済が回収されている前提だが、念のため
    // ここでも未 active を最優先で拾う)。
    u32 idx = impl.max_voices;
    for (u32 i = 0; i < impl.max_voices; ++i) {
        if (!impl.slots[i].active) { idx = i; break; }
    }
    if (idx == impl.max_voices) {
        // 全 slot 使用中 ⇒ ring overwrite はしない (StopVoice を呼んでもらう)。
        // 呼び出し側 (AudioDirector) が独自にエージング判断する余地を残す。
        ACS_LOG_WARN("XAudio2Backend::Play: voice pool full (max_voices=%u)",
                     impl.max_voices);
        return kInvalidAudioVoice;
    }
    VoiceSlot& slot = impl.slots[idx];

    // SourceVoice 作成。
    IXAudio2SourceVoice* source = nullptr;
    HRESULT hr = impl.xaudio2->CreateSourceVoice(&source, &wf);
    if (FAILED(hr) || source == nullptr) {
        ACS_LOG_WARN("XAudio2Backend::Play: CreateSourceVoice failed hr=0x%08x",
                     static_cast<u32>(hr));
        return kInvalidAudioVoice;
    }

    // PCM データを slot 内バッファにコピー (XAudio2 が再生中ずっと参照する)。
    const usize bytes = static_cast<usize>(clip.pcm_size);
    slot.buffer.Resize(bytes);
    const byte* src = static_cast<const byte*>(clip.pcm_data);
    for (usize i = 0; i < bytes; ++i) slot.buffer[i] = src[i];

    XAUDIO2_BUFFER xb{};
    xb.AudioBytes = static_cast<UINT32>(bytes);
    xb.pAudioData = slot.buffer.Data();
    xb.Flags      = XAUDIO2_END_OF_STREAM;
    xb.LoopCount  = loop ? XAUDIO2_LOOP_INFINITE : 0u;

    hr = source->SubmitSourceBuffer(&xb);
    if (FAILED(hr)) {
        source->DestroyVoice();
        slot.buffer.Clear();
        ACS_LOG_WARN("XAudio2Backend::Play: SubmitSourceBuffer failed hr=0x%08x",
                     static_cast<u32>(hr));
        return kInvalidAudioVoice;
    }

    source->SetVolume(ClampVolume(volume));
    source->SetFrequencyRatio(ClampPitch(pitch));

    hr = source->Start(0);
    if (FAILED(hr)) {
        source->DestroyVoice();
        slot.buffer.Clear();
        ACS_LOG_WARN("XAudio2Backend::Play: Start failed hr=0x%08x",
                     static_cast<u32>(hr));
        return kInvalidAudioVoice;
    }

    slot.voice  = source;
    slot.active = true;
    slot.looped = loop;
    ++impl.active_count;

    return AudioVoiceHandle{ idx, slot.generation };
}

AudioVoiceHandle XAudio2Backend::PlayOneShot(const AudioClipDesc& clip,
                                             f32 volume,
                                             f32 pitch) noexcept {
    if (_impl == nullptr) return kInvalidAudioVoice;
    return PlayInternal(*_impl, clip, volume, pitch, /*loop=*/false);
}

AudioVoiceHandle XAudio2Backend::PlayLooped(const AudioClipDesc& clip,
                                            f32 volume,
                                            f32 pitch) noexcept {
    if (_impl == nullptr) return kInvalidAudioVoice;
    return PlayInternal(*_impl, clip, volume, pitch, /*loop=*/true);
}

// ----------------------------------------------------------------------------
// Stop / volume / 状態
// ----------------------------------------------------------------------------

void XAudio2Backend::StopVoice(AudioVoiceHandle voice) noexcept {
    if (_impl == nullptr || !_impl->initialized) return;
    if (!voice.IsValid()) return;
    const u32 idx = voice.Index();
    if (idx >= _impl->max_voices) return;
    VoiceSlot& slot = _impl->slots[idx];
    if (!slot.active) return;
    if (slot.generation != voice.Generation()) return;  // 古いハンドル、無視

    DestroySlot(slot);
    if (_impl->active_count > 0) --_impl->active_count;
}

void XAudio2Backend::SetVoiceVolume(AudioVoiceHandle voice, f32 volume) noexcept {
    if (_impl == nullptr || !_impl->initialized) return;
    if (!voice.IsValid()) return;
    const u32 idx = voice.Index();
    if (idx >= _impl->max_voices) return;
    VoiceSlot& slot = _impl->slots[idx];
    if (!slot.active || slot.voice == nullptr) return;
    if (slot.generation != voice.Generation()) return;

    slot.voice->SetVolume(ClampVolume(volume));
}

void XAudio2Backend::StopAllVoices() noexcept {
    if (_impl == nullptr) return;
    for (u32 i = 0; i < _impl->max_voices; ++i) {
        VoiceSlot& s = _impl->slots[i];
        if (s.active) DestroySlot(s);
    }
    _impl->active_count = 0;
}

u32 XAudio2Backend::ActiveVoiceCount() const noexcept {
    return _impl != nullptr ? _impl->active_count : 0u;
}

void XAudio2Backend::Tick(f32 /*dt*/) noexcept {
    if (_impl == nullptr || !_impl->initialized) return;
    // 一発再生の完了 voice (BuffersQueued == 0) を回収する。ループ再生は
    // BuffersQueued が常に >= 1 のままなので、自然回収されない (StopVoice
    // を呼ばないと止まらない、これは仕様)。
    for (u32 i = 0; i < _impl->max_voices; ++i) {
        VoiceSlot& s = _impl->slots[i];
        if (!s.active || s.looped || s.voice == nullptr) continue;
        XAUDIO2_VOICE_STATE st{};
        s.voice->GetState(&st);
        if (st.BuffersQueued == 0) {
            DestroySlot(s);
            if (_impl->active_count > 0) --_impl->active_count;
        }
    }
}

// ----------------------------------------------------------------------------
// 拡張 (XAudio2 固有)
// ----------------------------------------------------------------------------

void XAudio2Backend::SetMasterVolume(f32 volume) noexcept {
    if (_impl == nullptr || _impl->mastering == nullptr) return;
    _impl->mastering->SetVolume(ClampVolume(volume));
}

} // namespace acs::game
