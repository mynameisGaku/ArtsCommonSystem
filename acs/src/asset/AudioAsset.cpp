// SPDX-License-Identifier: Apache-2.0
// 音声アセット実装（dr_wav / dr_mp3 / dr_flac / stb_vorbis）
#include "asset/AudioAsset.h"
#include "memory/Memory.h"

#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>

#define DR_MP3_IMPLEMENTATION
#include <dr_mp3.h>

#define DR_FLAC_IMPLEMENTATION
#include <dr_flac.h>

#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
#undef STB_VORBIS_HEADER_ONLY
// .c ファイルの実装部だけを再 include で取り込む（stb_vorbis 標準パターン）
#include <stb_vorbis.c>

namespace acs {

namespace {
// dr_libs / stb_vorbis から取得したサンプル列を AudioAsset に詰める共通処理
TRc<Asset> MakeAudio(FAssetId id, u32 sr, u8 ch, ESampleFormat fmt, u64 frames,
                    const void* src, usize src_bytes) noexcept {
    TArray<byte> samples;
    samples.Resize(src_bytes);
    MemCopy(samples.Data(), src, src_bytes);
    TRc<AudioAsset> a = MakeRc<AudioAsset>(sr, ch, fmt, frames, Move(samples));
    a->SetId(id);
    a->SetState(EAssetState::Ready);
    return TRc<Asset>(Move(a));
}
} // namespace

// ---- WAV (dr_wav) -------------------------------------------------------
TResult<TRc<Asset>> WavAssetLoader::LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept {
    drwav wav{};
    if (!::drwav_init_memory(&wav, bytes.Data(), bytes.Size(), nullptr))
        return ACS_ERR(Asset, 200, "drwav_init_memory failed");
    u64 frames = wav.totalPCMFrameCount;
    u8  ch     = static_cast<u8>(wav.channels);
    u32 sr     = wav.sampleRate;
    usize byte_count = static_cast<usize>(frames) * ch * sizeof(i16);
    TArray<byte> tmp;
    tmp.Resize(byte_count);
    u64 read = ::drwav_read_pcm_frames_s16(&wav, frames, reinterpret_cast<drwav_int16*>(tmp.Data()));
    ::drwav_uninit(&wav);
    if (read == 0) return ACS_ERR(Asset, 201, "drwav_read_pcm_frames_s16 failed");
    return TResult<TRc<Asset>>(OkInit,
        MakeAudio(id, sr, ch, ESampleFormat::PCM_S16, read, tmp.Data(), tmp.Size()));
}

// ---- MP3 (dr_mp3) -------------------------------------------------------
TResult<TRc<Asset>> Mp3AssetLoader::LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept {
    drmp3 mp3{};
    if (!::drmp3_init_memory(&mp3, bytes.Data(), bytes.Size(), nullptr))
        return ACS_ERR(Asset, 210, "drmp3_init_memory failed");
    u64 frames = ::drmp3_get_pcm_frame_count(&mp3);
    u8  ch     = static_cast<u8>(mp3.channels);
    u32 sr     = mp3.sampleRate;
    usize byte_count = static_cast<usize>(frames) * ch * sizeof(i16);
    TArray<byte> tmp;
    tmp.Resize(byte_count);
    u64 read = ::drmp3_read_pcm_frames_s16(&mp3, frames, reinterpret_cast<drmp3_int16*>(tmp.Data()));
    ::drmp3_uninit(&mp3);
    if (read == 0) return ACS_ERR(Asset, 211, "drmp3_read_pcm_frames_s16 failed");
    return TResult<TRc<Asset>>(OkInit,
        MakeAudio(id, sr, ch, ESampleFormat::PCM_S16, read, tmp.Data(), tmp.Size()));
}

// ---- FLAC (dr_flac) -----------------------------------------------------
TResult<TRc<Asset>> FlacAssetLoader::LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept {
    drflac* flac = ::drflac_open_memory(bytes.Data(), bytes.Size(), nullptr);
    if (!flac) return ACS_ERR(Asset, 220, "drflac_open_memory failed");
    u64 frames = flac->totalPCMFrameCount;
    u8  ch     = static_cast<u8>(flac->channels);
    u32 sr     = flac->sampleRate;
    usize byte_count = static_cast<usize>(frames) * ch * sizeof(i16);
    TArray<byte> tmp;
    tmp.Resize(byte_count);
    u64 read = ::drflac_read_pcm_frames_s16(flac, frames, reinterpret_cast<drflac_int16*>(tmp.Data()));
    ::drflac_close(flac);
    if (read == 0) return ACS_ERR(Asset, 221, "drflac_read_pcm_frames_s16 failed");
    return TResult<TRc<Asset>>(OkInit,
        MakeAudio(id, sr, ch, ESampleFormat::PCM_S16, read, tmp.Data(), tmp.Size()));
}

// ---- OGG Vorbis (stb_vorbis) -------------------------------------------
TResult<TRc<Asset>> OggAssetLoader::LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept {
    int err = 0;
    stb_vorbis* v = ::stb_vorbis_open_memory(
        reinterpret_cast<const unsigned char*>(bytes.Data()),
        static_cast<int>(bytes.Size()), &err, nullptr);
    if (!v) return ACS_ERR(Asset, 230, "stb_vorbis_open_memory failed");
    stb_vorbis_info info = ::stb_vorbis_get_info(v);
    u32 frames = ::stb_vorbis_stream_length_in_samples(v);
    u8  ch     = static_cast<u8>(info.channels);
    u32 sr     = info.sample_rate;
    usize byte_count = static_cast<usize>(frames) * ch * sizeof(i16);
    TArray<byte> tmp;
    tmp.Resize(byte_count);
    int got = ::stb_vorbis_get_samples_short_interleaved(
        v, ch, reinterpret_cast<short*>(tmp.Data()), static_cast<int>(frames * ch));
    ::stb_vorbis_close(v);
    if (got <= 0) return ACS_ERR(Asset, 231, "stb_vorbis_get_samples_short_interleaved failed");
    return TResult<TRc<Asset>>(OkInit,
        MakeAudio(id, sr, ch, ESampleFormat::PCM_S16, static_cast<u64>(got), tmp.Data(), tmp.Size()));
}

} // namespace acs
