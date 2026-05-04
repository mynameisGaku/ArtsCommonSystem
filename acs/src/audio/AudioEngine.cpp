// AudioEngine 実装 (miniaudio backend、cross-platform)
//
// miniaudio は single-header library。本ファイルでのみ MA_IMPLEMENTATION を
// define して実装をリンク (他の TU は API 部のみ参照)。
//
// AudioAsset の raw PCM samples を ma_audio_buffer (data source) に乗せ、
// ma_sound から再生する。slot は最大 64 個 (kMaxVoices)、generation 付き
// SoundHandle で stale handle を弾く。

#include "audio/AudioEngine.h"
#include "foundation/Log.h"
#include "threading/ScopedLock.h"

// miniaudio: 警告抑制 + 機能の絞り込み (decoder/device は使う、エンコーダは不要)
#if defined(_MSC_VER)
    #pragma warning(push)
    #pragma warning(disable: 4244 4267 4456 4459 4701 4702 4456 4244)
#endif

#define MA_IMPLEMENTATION
#define MA_NO_ENCODING        // エンコード機能は不要 (再生のみ)
#define MA_NO_GENERATION      // 波形ジェネレータも不要
#include "miniaudio.h"

#if defined(_MSC_VER)
    #pragma warning(pop)
#endif

namespace acs {

namespace {

constexpr u32 kMaxVoices = 64;

// 1 個の発音スロット
struct VoiceSlot {
    ma_sound        sound{};
    ma_audio_buffer buffer{};
    Array<byte>     buffer_copy;     // PCM データを ma_audio_buffer の寿命中保持
    u32             generation = 0;
    bool            sound_init = false;
    bool            buffer_init = false;
    bool            in_use     = false;
};

ma_format ToMaFormat(SampleFormat f) noexcept {
    switch (f) {
        case SampleFormat::PCM_S16: return ma_format_s16;
        case SampleFormat::PCM_F32: return ma_format_f32;
    }
    return ma_format_s16;
}

} // namespace

struct AudioEngine::Impl {
    ma_engine               engine{};
    bool                    engine_initialized = false;

    Mutex                   lock;
    VoiceSlot               slots[kMaxVoices] {};
    u32                     active_count       = 0;
};

AudioEngine::~AudioEngine() noexcept {
    Shutdown();
}

Result<void> AudioEngine::Init() noexcept {
    if (_impl) return ACS_ERR(Generic, 1, "AudioEngine already initialized");
    _impl = new Impl();

    ma_engine_config cfg = ma_engine_config_init();
    ma_result r = ma_engine_init(&cfg, &_impl->engine);
    if (r != MA_SUCCESS) {
        delete _impl; _impl = nullptr;
        return ACS_ERR_OS(Generic, 2, "ma_engine_init failed", static_cast<u32>(r));
    }
    _impl->engine_initialized = true;

    ACS_LOG_INFO("AudioEngine initialized (miniaudio)");
    return Ok();
}

void AudioEngine::Shutdown() noexcept {
    if (!_impl) return;
    StopAll();
    if (_impl->engine_initialized) {
        ma_engine_uninit(&_impl->engine);
        _impl->engine_initialized = false;
    }
    delete _impl;
    _impl = nullptr;
}

namespace {
u32 FindFreeSlot(AudioEngine::Impl& impl) noexcept {
    for (u32 i = 0; i < kMaxVoices; ++i) {
        if (!impl.slots[i].in_use) return i;
    }
    return 0xFFFFFFFFu;
}

void DestroySlot(VoiceSlot& slot) noexcept {
    if (slot.sound_init) {
        ma_sound_uninit(&slot.sound);
        slot.sound_init = false;
    }
    if (slot.buffer_init) {
        ma_audio_buffer_uninit(&slot.buffer);
        slot.buffer_init = false;
    }
    slot.buffer_copy.Clear();
    ++slot.generation;
    slot.in_use = false;
}
} // namespace

SoundHandle AudioEngine::Play(const AudioAsset& asset, f32 volume, bool loop) noexcept {
    if (!_impl || !_impl->engine_initialized) return kInvalidSound;
    if (asset.SampleByteCount() == 0) return kInvalidSound;

    ScopedLock lk(_impl->lock);

    u32 idx = FindFreeSlot(*_impl);
    if (idx == 0xFFFFFFFFu) return kInvalidSound;
    VoiceSlot& slot = _impl->slots[idx];

    // PCM データをコピーしておく (asset の寿命に依存しないように)
    slot.buffer_copy.Resize(asset.SampleByteCount());
    for (usize i = 0; i < asset.SampleByteCount(); ++i)
        slot.buffer_copy[i] = asset.Samples()[i];

    // 1 サンプル当たりのバイト数 = チャンネル × フォーマットサイズ
    ma_format fmt = ToMaFormat(asset.Format());
    u32 frame_bytes = ma_get_bytes_per_frame(fmt, asset.Channels());
    u64 frame_count = frame_bytes > 0 ? slot.buffer_copy.Size() / frame_bytes : 0;
    if (frame_count == 0) return kInvalidSound;

    // ma_audio_buffer に raw PCM を乗せる
    ma_audio_buffer_config bcfg = ma_audio_buffer_config_init(
        fmt, asset.Channels(), frame_count, slot.buffer_copy.Data(), nullptr);
    if (ma_audio_buffer_init(&bcfg, &slot.buffer) != MA_SUCCESS) return kInvalidSound;
    slot.buffer_init = true;

    // ma_sound として登録 (data source = audio_buffer)
    if (ma_sound_init_from_data_source(
            &_impl->engine, &slot.buffer, 0, nullptr, &slot.sound) != MA_SUCCESS) {
        DestroySlot(slot);
        return kInvalidSound;
    }
    slot.sound_init = true;

    if (volume < 0) volume = 0;
    if (volume > 1) volume = 1;
    ma_sound_set_volume(&slot.sound, volume);
    ma_sound_set_looping(&slot.sound, loop ? MA_TRUE : MA_FALSE);

    if (ma_sound_start(&slot.sound) != MA_SUCCESS) {
        DestroySlot(slot);
        return kInvalidSound;
    }

    slot.in_use = true;
    ++_impl->active_count;
    return SoundHandle{ idx, slot.generation };
}

void AudioEngine::Stop(SoundHandle h) noexcept {
    if (!_impl || !h.IsValid() || h.index >= kMaxVoices) return;
    ScopedLock lk(_impl->lock);
    VoiceSlot& slot = _impl->slots[h.index];
    if (!slot.in_use || slot.generation != h.generation) return;
    if (slot.sound_init) ma_sound_stop(&slot.sound);
    DestroySlot(slot);
    if (_impl->active_count > 0) --_impl->active_count;
}

void AudioEngine::SetVolume(SoundHandle h, f32 volume) noexcept {
    if (!_impl || !h.IsValid() || h.index >= kMaxVoices) return;
    ScopedLock lk(_impl->lock);
    VoiceSlot& slot = _impl->slots[h.index];
    if (!slot.in_use || slot.generation != h.generation) return;
    if (volume < 0) volume = 0;
    if (volume > 1) volume = 1;
    if (slot.sound_init) ma_sound_set_volume(&slot.sound, volume);
}

void AudioEngine::StopAll() noexcept {
    if (!_impl) return;
    ScopedLock lk(_impl->lock);
    for (u32 i = 0; i < kMaxVoices; ++i) {
        if (_impl->slots[i].in_use) {
            if (_impl->slots[i].sound_init)
                ma_sound_stop(&_impl->slots[i].sound);
            DestroySlot(_impl->slots[i]);
        }
    }
    _impl->active_count = 0;
}

u32 AudioEngine::ActiveCount() const noexcept {
    return _impl ? _impl->active_count : 0;
}

} // namespace acs
