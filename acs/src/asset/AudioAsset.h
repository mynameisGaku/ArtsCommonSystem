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

class FAudioAsset : public Asset {
public:
    ACS_ASSET_TYPE("FAudioAsset")

    FAudioAsset() noexcept = default;
    FAudioAsset(u32 sample_rate, u8 channels, ESampleFormat fmt,
               u64 frame_count, TArray<byte>&& samples) noexcept
        : m_SampleRate(sample_rate), m_Channels(channels), m_Format(fmt),
          m_FrameCount(frame_count), m_Samples(Move(samples)) {}

    u32          SampleRate() const noexcept { return m_SampleRate; }
    u8           Channels()   const noexcept { return m_Channels; }
    ESampleFormat EFormat()     const noexcept { return m_Format; }
    u64          FrameCount() const noexcept { return m_FrameCount; }
    const byte*  Samples()    const noexcept { return m_Samples.Data(); }
    usize        SampleByteCount() const noexcept { return m_Samples.Size(); }

    // 再生時間 (秒)
    f32 DurationSeconds() const noexcept {
        return m_SampleRate ? static_cast<f32>(m_FrameCount) / m_SampleRate : 0.0f;
    }

private:
    u32          m_SampleRate = 0;
    u8           m_Channels    = 0;
    ESampleFormat m_Format      = ESampleFormat::PCM_S16;
    u64          m_FrameCount = 0;     // 1 フレーム = チャンネル数分のサンプル
    TArray<byte>  m_Samples;             // インターリーブ済み生データ
};

// 各フォーマット別のローダ（拡張子で振り分け）
class WavAssetLoader  final : public IAssetLoader {
public:
    AssetType   TypeId()    const noexcept override { return FAudioAsset::StaticType(); }
    const char* Extension() const noexcept override { return "wav"; }
    TResult<TRc<Asset>> LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept override;
};

class Mp3AssetLoader  final : public IAssetLoader {
public:
    AssetType   TypeId()    const noexcept override { return FAudioAsset::StaticType(); }
    const char* Extension() const noexcept override { return "mp3"; }
    TResult<TRc<Asset>> LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept override;
};

class FlacAssetLoader final : public IAssetLoader {
public:
    AssetType   TypeId()    const noexcept override { return FAudioAsset::StaticType(); }
    const char* Extension() const noexcept override { return "flac"; }
    TResult<TRc<Asset>> LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept override;
};

class OggAssetLoader  final : public IAssetLoader {
public:
    AssetType   TypeId()    const noexcept override { return FAudioAsset::StaticType(); }
    const char* Extension() const noexcept override { return "ogg"; }
    TResult<TRc<Asset>> LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept override;
};

} // namespace acs
