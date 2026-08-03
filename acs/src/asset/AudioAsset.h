// SPDX-License-Identifier: Apache-2.0
// 音声アセット
//
// 対応拡張子:
//   wav / mp3 / flac / ogg / oga
//
// 全フォーマットを 16-bit PCM もしくは float PCM に統一して保持する。
// 再生は CAudioEngine (XAudio2Backend) が本アセットの PCM を直接扱う。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "asset/Asset.h"
#include "asset/IAssetLoader.h"
#include "container/Hash.h"

namespace acs {

/**
 * 音声サンプルのフォーマット。
 */
enum class ESampleFormat : u8 {
    /** 16-bit 符号付き整数 PCM。 */
    PCM_S16,

    /** 32-bit 浮動小数 PCM。 */
    PCM_F32,
};

/**
 * デコード済み音声 (PCM) を保持するアセット。
 *
 * @details
 * 全フォーマットを 16-bit もしくは float PCM に統一してインターリーブ保持する。
 * 再生は CAudioEngine (XAudio2Backend) が本アセットの PCM を直接扱う。
 */
class AAudioAsset : public AAsset {
public:
    ACS_ASSET_TYPE("FAudioAsset")

    /** 空の音声アセットを構築する。 */
    AAudioAsset() noexcept = default;

    /**
     * PCM データから音声アセットを構築する。
     *
     * @param sample_rate サンプリングレート (Hz)。
     * @param channels チャンネル数。
     * @param fmt サンプルフォーマット。
     * @param frame_count フレーム数 (1 フレーム = 全チャンネル 1 サンプル)。
     * @param samples インターリーブ済み生 PCM バイト列 (ムーブで所有を奪う)。
     */
    AAudioAsset(u32 sample_rate, u8 channels, ESampleFormat fmt,
               u64 frame_count, TArray<byte>&& samples) noexcept
        : m_SampleRate(sample_rate), m_Channels(channels), m_Format(fmt),
          m_FrameCount(frame_count), m_Samples(Move(samples)) {}

    /**
     * サンプリングレートを返す。
     *
     * @return サンプリングレート (Hz)。
     */
    u32          SampleRate() const noexcept { return m_SampleRate; }

    /**
     * チャンネル数を返す。
     *
     * @return チャンネル数。
     */
    u8           Channels()   const noexcept { return m_Channels; }

    /**
     * サンプルフォーマットを返す。
     *
     * @return ESampleFormat。
     */
    ESampleFormat Format()      const noexcept { return m_Format; }

    /**
     * フレーム数を返す。
     *
     * @return フレーム数 (1 フレーム = 全チャンネル 1 サンプル)。
     */
    u64          FrameCount() const noexcept { return m_FrameCount; }

    /**
     * 生 PCM データの先頭ポインタを返す。
     *
     * @return インターリーブ済み PCM バイト列の先頭。
     */
    const byte*  Samples()    const noexcept { return m_Samples.GetData(); }

    /**
     * 生 PCM データのバイト数を返す。
     *
     * @return PCM バイト列のサイズ。
     */
    usize        SampleByteCount() const noexcept { return m_Samples.Num(); }

    /**
     * 再生時間を秒で返す。
     *
     * @return frame_count / sample_rate の秒数 (sample_rate が 0 なら 0)。
     */
    f32 DurationSeconds() const noexcept {
        return m_SampleRate ? static_cast<f32>(m_FrameCount) / m_SampleRate : 0.0f;
    }

private:
    /** サンプリングレート (Hz)。 */
    u32          m_SampleRate = 0;

    /** チャンネル数。 */
    u8           m_Channels    = 0;

    /** サンプルフォーマット。 */
    ESampleFormat m_Format      = ESampleFormat::PCM_S16;

    /** フレーム数 (1 フレーム = チャンネル数分のサンプル)。 */
    u64          m_FrameCount = 0;

    /** インターリーブ済み生 PCM データ。 */
    TArray<byte>  m_Samples;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FAudioAsset = AAudioAsset;

/**
 * WAV ファイルを AAudioAsset にデコードするローダ (dr_wav)。
 */
class CWavAssetLoader  final : public IAssetLoader {
public:
    /**
     * 生成するアセット型 ID を返す。
     *
     * @return AAudioAsset の型 ID。
     */
    AssetType   TypeId()    const noexcept override { return AAudioAsset::StaticType(); }

    /**
     * 担当拡張子を返す。
     *
     * @return "wav"。
     */
    const char* Extension() const noexcept override { return "wav"; }

    /**
     * WAV バイト列を 16-bit PCM の AAudioAsset にデコードする。
     *
     * @param id 生成アセットに割り当てる ID。
     * @param bytes WAV ファイル全体のバイト列。
     * @return 成功なら AAudioAsset、失敗ならエラー。
     */
    TResult<TSharedPtr<AAsset>> LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept override;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FWavAssetLoader = CWavAssetLoader;

/**
 * MP3 ファイルを AAudioAsset にデコードするローダ (dr_mp3)。
 */
class CMp3AssetLoader  final : public IAssetLoader {
public:
    /**
     * 生成するアセット型 ID を返す。
     *
     * @return AAudioAsset の型 ID。
     */
    AssetType   TypeId()    const noexcept override { return AAudioAsset::StaticType(); }

    /**
     * 担当拡張子を返す。
     *
     * @return "mp3"。
     */
    const char* Extension() const noexcept override { return "mp3"; }

    /**
     * MP3 バイト列を 16-bit PCM の AAudioAsset にデコードする。
     *
     * @param id 生成アセットに割り当てる ID。
     * @param bytes MP3 ファイル全体のバイト列。
     * @return 成功なら AAudioAsset、失敗ならエラー。
     */
    TResult<TSharedPtr<AAsset>> LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept override;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FMp3AssetLoader = CMp3AssetLoader;

/**
 * FLAC ファイルを AAudioAsset にデコードするローダ (dr_flac)。
 */
class CFlacAssetLoader final : public IAssetLoader {
public:
    /**
     * 生成するアセット型 ID を返す。
     *
     * @return AAudioAsset の型 ID。
     */
    AssetType   TypeId()    const noexcept override { return AAudioAsset::StaticType(); }

    /**
     * 担当拡張子を返す。
     *
     * @return "flac"。
     */
    const char* Extension() const noexcept override { return "flac"; }

    /**
     * FLAC バイト列を 16-bit PCM の AAudioAsset にデコードする。
     *
     * @param id 生成アセットに割り当てる ID。
     * @param bytes FLAC ファイル全体のバイト列。
     * @return 成功なら AAudioAsset、失敗ならエラー。
     */
    TResult<TSharedPtr<AAsset>> LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept override;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FFlacAssetLoader = CFlacAssetLoader;

/**
 * OGG Vorbis ファイルを AAudioAsset にデコードするローダ (stb_vorbis)。
 */
class COggAssetLoader  final : public IAssetLoader {
public:
    /**
     * 生成するアセット型 ID を返す。
     *
     * @return AAudioAsset の型 ID。
     */
    AssetType   TypeId()    const noexcept override { return AAudioAsset::StaticType(); }

    /**
     * 担当拡張子を返す。
     *
     * @return "ogg"。
     */
    const char* Extension() const noexcept override { return "ogg"; }

    /**
     * OGG Vorbis バイト列を 16-bit PCM の AAudioAsset にデコードする。
     *
     * @param id 生成アセットに割り当てる ID。
     * @param bytes OGG ファイル全体のバイト列。
     * @return 成功なら AAudioAsset、失敗ならエラー。
     */
    TResult<TSharedPtr<AAsset>> LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept override;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FOggAssetLoader = COggAssetLoader;

} // namespace acs
