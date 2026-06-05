// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — FXAudio2Backend 実装 (Windows / XAudio2 / COM)
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

// Win32 / XAudio2 ヘッダ (本 .cpp ローカル)。
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

namespace {

/**
 * volume を 0.0..1.0 に clamp する。
 *
 * @details XAudio2 自体は > 1 も受けるが歪むので抑える。
 * @param v clamp する音量。
 * @return [0.0, 1.0] に収めた音量。
 */
f32 ClampVolume(f32 v) noexcept {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/**
 * pitch を XAudio2 が受ける周波数比に clamp する。
 *
 * @details
 * XAudio2 は SetFrequencyRatio で XAUDIO2_MIN_FREQ_RATIO (= 1/1024.0) ..
 * XAUDIO2_MAX_FREQ_RATIO (= 1024.0) を受けるが、現実用途は [0.25, 4.0] で十分。
 * @param p clamp する周波数比。
 * @return [0.25, 4.0] に収めた周波数比。
 */
f32 ClampPitch(f32 p) noexcept {
    constexpr f32 kMin = 0.25f;
    constexpr f32 kMax = 4.0f;
    if (p < kMin) return kMin;
    if (p > kMax) return kMax;
    return p;
}

/** 1 個の発音 slot。 */
struct VoiceSlot {
    /** XAudio2 source voice (空きスロットは nullptr)。 */
    IXAudio2SourceVoice* voice    = nullptr;

    /** 再生中ずっと保持する PCM コピー (XAudio2 が参照し続けるため)。 */
    TArray<byte>          buffer;

    /**
     * handle 検証用 generation (1 始まり、0 は invalid 表現に予約)。
     *
     * @details
     * AudioVoiceHandle は (index | gen<<24) を packed == 0 で invalid とする規約
     * (FNodeId / FShapeId と同一)。generation=0 のまま index=0 の slot を配ると
     * packed == 0 = kInvalidAudioVoice と衝突し、確保成功が IsValid()==false となって
     * StopVoice 不能 → voice リーク + active_count 不整合を起こす。よって初期値を 1 にし、
     * DestroySlot でのラップ時も 0 をスキップする (1..255 を循環)。
     */
    u8                   generation = 1;

    /** true なら使用中。 */
    bool                 active    = false;

    /** ループ再生か (一発再生は Tick で自然回収、ループは StopVoice まで残る)。 */
    bool                 looped    = false;
};

} // namespace

/** XAudio2 / COM のハンドルと voice pool を保持する pimpl 本体。 */
struct FXAudio2Backend::Impl {
    /** XAudio2 エンジンインスタンス。 */
    IXAudio2*               xaudio2          = nullptr;

    /** マスタリングボイス (最終出力)。 */
    IXAudio2MasteringVoice* mastering        = nullptr;

    /** 自分で CoInitializeEx に成功したか (true のときだけ Shutdown で CoUninitialize)。 */
    bool                    com_initialized  = false;

    /** フル init 済か (Init 成功フラグ)。 */
    bool                    initialized      = false;

    /** 固定容量の発音 slot 配列。 */
    TArray<VoiceSlot>        slots;

    /** 同時発音数の上限 (= slot 数)。 */
    u32                     max_voices       = 0;

    /** 現在アクティブな voice 数。 */
    u32                     active_count     = 0;
};

FXAudio2Backend::FXAudio2Backend() noexcept = default;

FXAudio2Backend::~FXAudio2Backend() noexcept {
    Shutdown();
}

TResult<void> FXAudio2Backend::Init(u32 max_voices) noexcept {
    if (m_Impl != nullptr && m_Impl->initialized) {
        return ACS_ERR(Generic, kSubAudioAlreadyInitialized,
                       "FXAudio2Backend::Init: already initialized");
    }
    if (max_voices == 0) {
        return ACS_ERR(Generic, kSubAudioInvalidArgs,
                       "FXAudio2Backend::Init: max_voices=0 is invalid");
    }
    // slot index は 24bit に収まる必要 (handle 内で 24bit 制限)。
    // 1<<24 = 16777216 だが、現実 max_voices はせいぜい数千なので念のため check。
    if (max_voices >= (1u << 24)) {
        return ACS_ERR(Generic, kSubAudioInvalidArgs,
                       "FXAudio2Backend::Init: max_voices too large (must be < 2^24)");
    }

    if (m_Impl == nullptr) {
        m_Impl = new Impl();
    }

    // COM 初期化。
    // 既に他モジュールが COM init 済の可能性あり (S_FALSE / RPC_E_CHANGED_MODE)。
    // 自分が成功 (S_OK) したときだけ Shutdown で CoUninitialize を呼ぶ。
    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == S_OK) {
        m_Impl->com_initialized = true;
    } else if (hr == S_FALSE) {
        // 同モードで既に init 済。CoUninitialize は呼ばない。
        m_Impl->com_initialized = false;
    } else if (hr == RPC_E_CHANGED_MODE) {
        // 別モードで init 済。XAudio2 は両モードで動くので警告のみ。
        ACS_LOG_WARN("FXAudio2Backend::Init: COM already initialized in a different "
                     "threading mode (RPC_E_CHANGED_MODE) — continuing");
        m_Impl->com_initialized = false;
    } else {
        Shutdown();
        return ACS_ERR_OS(Generic, kSubAudioComInitFailed,
                          "FXAudio2Backend::Init: CoInitializeEx failed",
                          static_cast<u32>(hr));
    }

    // XAudio2 エンジン作成。
    hr = ::XAudio2Create(&m_Impl->xaudio2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        Shutdown();
        return ACS_ERR_OS(Generic, kSubAudioCreateFailed,
                          "FXAudio2Backend::Init: XAudio2Create failed",
                          static_cast<u32>(hr));
    }

    // マスタリングボイス。
    hr = m_Impl->xaudio2->CreateMasteringVoice(&m_Impl->mastering);
    if (FAILED(hr)) {
        Shutdown();
        return ACS_ERR_OS(Generic, kSubAudioMasterVoiceFailed,
                          "FXAudio2Backend::Init: CreateMasteringVoice failed",
                          static_cast<u32>(hr));
    }

    // voice pool。
    m_Impl->max_voices = max_voices;
    m_Impl->slots.Resize(max_voices);  // VoiceSlot{} で初期化 (active=false)

    m_Impl->initialized = true;
    ACS_LOG_INFO("FXAudio2Backend: initialized (max_voices=%u)", max_voices);
    return Ok();
}

void FXAudio2Backend::Shutdown() noexcept {
    if (m_Impl == nullptr) return;

    // 全 voice 停止 + destroy。
    StopAllVoices();
    // slot TArray 自体は Impl のデストラクタで TArray<VoiceSlot> 経由解放される。
    m_Impl->slots.Clear();
    m_Impl->max_voices = 0;

    // mastering voice。
    if (m_Impl->mastering != nullptr) {
        m_Impl->mastering->DestroyVoice();
        m_Impl->mastering = nullptr;
    }

    // XAudio2 engine。
    if (m_Impl->xaudio2 != nullptr) {
        m_Impl->xaudio2->Release();
        m_Impl->xaudio2 = nullptr;
    }

    // COM。
    if (m_Impl->com_initialized) {
        ::CoUninitialize();
        m_Impl->com_initialized = false;
    }

    m_Impl->initialized = false;
    delete m_Impl;
    m_Impl = nullptr;
}

bool FXAudio2Backend::IsInitialized() const noexcept {
    return m_Impl != nullptr && m_Impl->initialized;
}

namespace {

/**
 * clip のフォーマットから WAVEFORMATEX を組み立てる (Pcm16 / Pcm32Float のみ対応)。
 *
 * @details
 * Wav 形式はここでは受け付けない (将来 wav parser を入れる場合は別 path で処理する)。
 * @param clip 入力 clip 記述子。
 * @param out 書き込み先の WAVEFORMATEX。
 * @return 対応フォーマットで有効な値を組めたら true、未対応 fmt や不正値なら false。
 */
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
        // 本 backend では未対応。FAudioDirector / asset layer 側で事前に
        // raw PCM (Pcm16 / Pcm32Float) に展開してから渡してもらう想定。
        return false;
    }

    out.nBlockAlign     = static_cast<WORD>((out.nChannels * out.wBitsPerSample) / 8);
    out.nAvgBytesPerSec = out.nSamplesPerSec * out.nBlockAlign;
    return out.nBlockAlign != 0 && out.nChannels != 0 && out.nSamplesPerSec != 0;
}

/**
 * VoiceSlot を完全破棄する (XAudio2 voice + バッファコピー解放)。
 *
 * @details
 * generation を increment して古いハンドルでの再アクセスを無効化する。0 にラップ
 * したら 1 にスキップする (0 は invalid 表現に予約)。active=false に戻す。
 * @param slot 破棄する発音 slot。
 */
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
    // generation を進めて古いハンドルを無効化。0 にラップしたら 1 にスキップする
    // (0 は invalid 表現に予約 = FNodePool::RegisterExistingNode と同じ規約)。
    ++slot.generation;  // u8 wrap (オーバーフローは UB ではない)
    if (slot.generation == 0u) slot.generation = 1u;
}

} // namespace

/**
 * PlayOneShot / PlayLooped の共通実装 (一発 / ループの分岐は loop 引数で吸収)。
 *
 * @details
 * 空きスロットを線形探索し、SourceVoice を作って PCM を slot 内バッファにコピー
 * してから再生する。未 init / データ不正 / 未対応 fmt / pool 満杯 / XAudio2 失敗の
 * いずれでも kInvalidAudioVoice を返す。
 * @param impl 操作対象の pimpl。
 * @param clip 再生する PCM clip 記述子。
 * @param volume 音量 (clamp される)。
 * @param pitch 周波数比 (clamp される)。
 * @param loop true ならループ再生 (XAUDIO2_LOOP_INFINITE)。
 * @return 再生中 voice のハンドル (失敗時は kInvalidAudioVoice)。
 */
static AudioVoiceHandle PlayInternal(FXAudio2Backend::Impl& impl,
                                     const AudioClipDesc& clip,
                                     f32 volume,
                                     f32 pitch,
                                     bool loop) noexcept {
    if (!impl.initialized) return kInvalidAudioVoice;
    if (clip.pcm_data == nullptr || clip.pcm_size == 0) return kInvalidAudioVoice;
    if (clip.channel_count == 0 || clip.sample_rate == 0) return kInvalidAudioVoice;

    WAVEFORMATEX wf{};
    if (!FillWaveFormat(clip, wf)) {
        ACS_LOG_WARN("FXAudio2Backend::Play: unsupported clip format (channels=%u, "
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
        // 呼び出し側 (FAudioDirector) が独自にエージング判断する余地を残す。
        ACS_LOG_WARN("FXAudio2Backend::Play: voice pool full (max_voices=%u)",
                     impl.max_voices);
        return kInvalidAudioVoice;
    }
    VoiceSlot& slot = impl.slots[idx];

    // SourceVoice 作成。
    IXAudio2SourceVoice* source = nullptr;
    HRESULT hr = impl.xaudio2->CreateSourceVoice(&source, &wf);
    if (FAILED(hr) || source == nullptr) {
        ACS_LOG_WARN("FXAudio2Backend::Play: CreateSourceVoice failed hr=0x%08x",
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
        ACS_LOG_WARN("FXAudio2Backend::Play: SubmitSourceBuffer failed hr=0x%08x",
                     static_cast<u32>(hr));
        return kInvalidAudioVoice;
    }

    source->SetVolume(ClampVolume(volume));
    source->SetFrequencyRatio(ClampPitch(pitch));

    hr = source->Start(0);
    if (FAILED(hr)) {
        source->DestroyVoice();
        slot.buffer.Clear();
        ACS_LOG_WARN("FXAudio2Backend::Play: Start failed hr=0x%08x",
                     static_cast<u32>(hr));
        return kInvalidAudioVoice;
    }

    slot.voice  = source;
    slot.active = true;
    slot.looped = loop;
    ++impl.active_count;

    return AudioVoiceHandle{ idx, slot.generation };
}

/** 一発再生する (PlayInternal に loop=false で委譲)。 */
AudioVoiceHandle FXAudio2Backend::PlayOneShot(const AudioClipDesc& clip,
                                             f32 volume,
                                             f32 pitch) noexcept {
    if (m_Impl == nullptr) return kInvalidAudioVoice;
    return PlayInternal(*m_Impl, clip, volume, pitch, /*loop=*/false);
}

/** ループ再生する (PlayInternal に loop=true で委譲)。 */
AudioVoiceHandle FXAudio2Backend::PlayLooped(const AudioClipDesc& clip,
                                            f32 volume,
                                            f32 pitch) noexcept {
    if (m_Impl == nullptr) return kInvalidAudioVoice;
    return PlayInternal(*m_Impl, clip, volume, pitch, /*loop=*/true);
}

void FXAudio2Backend::StopVoice(AudioVoiceHandle voice) noexcept {
    if (m_Impl == nullptr || !m_Impl->initialized) return;
    if (!voice.IsValid()) return;
    const u32 idx = voice.Index();
    if (idx >= m_Impl->max_voices) return;
    VoiceSlot& slot = m_Impl->slots[idx];
    if (!slot.active) return;
    if (slot.generation != voice.Generation()) return;  // 古いハンドル、無視

    DestroySlot(slot);
    if (m_Impl->active_count > 0) --m_Impl->active_count;
}

void FXAudio2Backend::SetVoiceVolume(AudioVoiceHandle voice, f32 volume) noexcept {
    if (m_Impl == nullptr || !m_Impl->initialized) return;
    if (!voice.IsValid()) return;
    const u32 idx = voice.Index();
    if (idx >= m_Impl->max_voices) return;
    VoiceSlot& slot = m_Impl->slots[idx];
    if (!slot.active || slot.voice == nullptr) return;
    if (slot.generation != voice.Generation()) return;

    slot.voice->SetVolume(ClampVolume(volume));
}

void FXAudio2Backend::StopAllVoices() noexcept {
    if (m_Impl == nullptr) return;
    for (u32 i = 0; i < m_Impl->max_voices; ++i) {
        VoiceSlot& s = m_Impl->slots[i];
        if (s.active) DestroySlot(s);
    }
    m_Impl->active_count = 0;
}

u32 FXAudio2Backend::ActiveVoiceCount() const noexcept {
    return m_Impl != nullptr ? m_Impl->active_count : 0u;
}

void FXAudio2Backend::Tick(f32 /*dt*/) noexcept {
    if (m_Impl == nullptr || !m_Impl->initialized) return;
    // 一発再生の完了 voice (BuffersQueued == 0) を回収する。ループ再生は
    // BuffersQueued が常に >= 1 のままなので、自然回収されない (StopVoice
    // を呼ばないと止まらない、これは仕様)。
    for (u32 i = 0; i < m_Impl->max_voices; ++i) {
        VoiceSlot& s = m_Impl->slots[i];
        if (!s.active || s.looped || s.voice == nullptr) continue;
        XAUDIO2_VOICE_STATE st{};
        s.voice->GetState(&st);
        if (st.BuffersQueued == 0) {
            DestroySlot(s);
            if (m_Impl->active_count > 0) --m_Impl->active_count;
        }
    }
}

/** マスタリングボイス音量を設定する (volume は clamp される)。 */
void FXAudio2Backend::SetMasterVolume(f32 volume) noexcept {
    if (m_Impl == nullptr || m_Impl->mastering == nullptr) return;
    m_Impl->mastering->SetVolume(ClampVolume(volume));
}

} // namespace acs::game
