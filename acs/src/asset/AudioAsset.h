// SPDX-License-Identifier: Apache-2.0
// 音声アセット
//
// 対応拡張子:
//   wav / mp3 / flac / ogg / oga
//
// 全フォーマットを 16-bit PCM もしくは float PCM に統一して保持する。
// 再生は別モジュール (Audio: 未実装) で扱う。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "asset/Asset.h"
#include "asset/IAssetLoader.h"
#include "container/Hash.h"

namespace acs {

// サンプルフォーマット
enum class ESampleFormat : u8 {
    PCM_S16,   // 16-bit 符号付き整数
    PCM_F32,   // 32-bit 浮動小数
};

class AudioAsset : public Asset {
public:
    ACS_ASSET_TYPE("AudioAsset")

    AudioAsset() noexcept = default;
    AudioAsset(u32 sample_rate, u8 channels, ESampleFormat fmt,
               u64 frame_count, Array<byte>&& samples) noexcept
        : _sample_rate(sample_rate), _channels(channels), _format(fmt),
          _frame_count(frame_count), _samples(Move(samples)) {}

    u32          SampleRate() const noexcept { return _sample_rate; }
    u8           Channels()   const noexcept { return _channels; }
    ESampleFormat EFormat()     const noexcept { return _format; }
    u64          FrameCount() const noexcept { return _frame_count; }
    const byte*  Samples()    const noexcept { return _samples.Data(); }
    usize        SampleByteCount() const noexcept { return _samples.Size(); }

    // 再生時間 (秒)
    f32 DurationSeconds() const noexcept {
        return _sample_rate ? static_cast<f32>(_frame_count) / _sample_rate : 0.0f;
    }

private:
    u32          _sample_rate = 0;
    u8           _channels    = 0;
    ESampleFormat _format      = ESampleFormat::PCM_S16;
    u64          _frame_count = 0;     // 1 フレーム = チャンネル数分のサンプル
    Array<byte>  _samples;             // インターリーブ済み生データ
};

// 各フォーマット別のローダ（拡張子で振り分け）
class WavAssetLoader  final : public IAssetLoader {
public:
    AssetType   TypeId()    const noexcept override { return AudioAsset::StaticType(); }
    const char* Extension() const noexcept override { return "wav"; }
    Result<Rc<Asset>> LoadFromBytes(AssetId id, const Array<byte>& bytes) noexcept override;
};

class Mp3AssetLoader  final : public IAssetLoader {
public:
    AssetType   TypeId()    const noexcept override { return AudioAsset::StaticType(); }
    const char* Extension() const noexcept override { return "mp3"; }
    Result<Rc<Asset>> LoadFromBytes(AssetId id, const Array<byte>& bytes) noexcept override;
};

class FlacAssetLoader final : public IAssetLoader {
public:
    AssetType   TypeId()    const noexcept override { return AudioAsset::StaticType(); }
    const char* Extension() const noexcept override { return "flac"; }
    Result<Rc<Asset>> LoadFromBytes(AssetId id, const Array<byte>& bytes) noexcept override;
};

class OggAssetLoader  final : public IAssetLoader {
public:
    AssetType   TypeId()    const noexcept override { return AudioAsset::StaticType(); }
    const char* Extension() const noexcept override { return "ogg"; }
    Result<Rc<Asset>> LoadFromBytes(AssetId id, const Array<byte>& bytes) noexcept override;
};

} // namespace acs
