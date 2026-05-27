// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — FXAudio2Backend (Windows 用 IAudioBackend 実装)
//
// 役割:
//   `IAudioBackend` の concrete 実装 = Win32 XAudio2 (Windows SDK 同梱) を
//   叩いて実音声を出す。`FAudioDirector::SetBackend(&xaudio2)` で差し込んで
//   使う。
//
// 使い方 (典型例):
//   class FGame {
//       acs::game::FXAudio2Backend m_Audio;
//       acs::game::FAudioDirector  m_Director;
//
//       TResult<void> OnStart() noexcept override {
//           ACS_TRY(m_Audio.Init(64));      // 同時発音 64 voice
//           m_Director.SetBackend(&m_Audio);
//           return Ok();
//       }
//       void OnShutdown() noexcept override {
//           m_Director.SetBackend(nullptr);  // 先に director を切る (delegate 停止)
//           m_Audio.Shutdown();
//       }
//   };
//
// 設計選択:
//   ・**pimpl で XAudio2 ヘッダ隠蔽**: `<xaudio2.h>` は <windows.h> + COM を
//     引っ張る重いヘッダなので .cpp に閉じ込め、.h ではポインタ前方宣言だけ
//     公開する (ACS の `acs::FAudioEngine` と同じパターン)。
//   ・**固定容量 voice pool**: `Init(max_voices)` 時に確保。再 init 不可。
//     PlayOneShot / PlayLooped で空きを線形探索 (max_voices は通常 ≦ 128 で、
//     ホットパス影響は無視できる)。
//   ・**generation 付き handle**: slot 再利用時に古いハンドルで操作されても
//     generation 不一致で no-op 化、use-after-free を防ぐ。
//   ・**COM init は本 backend が責任を持つ**: 既に他所で `CoInitializeEx` 済
//     なら HRESULT が `RPC_E_CHANGED_MODE` 等で戻るので、その場合は「他所が
//     init 済」とみなして自分では Uninitialize しない (フラグで覚える)。
//   ・**Pcm32Float / Pcm16 のみ実音再生対応**: 今回は Wav 形式は asset layer
//     側で事前デコードしてから raw PCM として渡す前提 (Phase 3 で本 backend
//     内 wav parser を追加するなら拡張可)。
//   ・**Tick で完了 voice 回収**: 一発再生は `BuffersQueued == 0` を見て
//     自然回収する。回収しないと kMaxVoices 回再生でスロット枯渇する。
//
// 範囲外:
//   ・3D positional / spatial (Pillar FSpatialAudio 担当、別 backend)
//   ・stream 再生 / 動的バッファ供給
//   ・wav/ogg/mp3 デコード (raw PCM を渡す前提)
#pragma once

#include "container/Array.h"
#include "foundation/Result.h"
#include "foundation/Types.h"
#include "gameframework/audio_backend/IAudioBackend.h"

namespace acs::game {

class FXAudio2Backend final : public IAudioBackend {
public:
    FXAudio2Backend() noexcept;
    ~FXAudio2Backend() noexcept override;

    FXAudio2Backend(const FXAudio2Backend&)            = delete;
    FXAudio2Backend& operator=(const FXAudio2Backend&) = delete;
    FXAudio2Backend(FXAudio2Backend&&)                 = delete;
    FXAudio2Backend& operator=(FXAudio2Backend&&)      = delete;

    // ----- IAudioBackend 実装 -----
    TResult<void> Init(u32 max_voices = 64) noexcept override;
    void         Shutdown() noexcept override;
    bool         IsInitialized() const noexcept override;

    AudioVoiceHandle PlayOneShot(const AudioClipDesc& clip,
                                 f32 volume,
                                 f32 pitch) noexcept override;
    AudioVoiceHandle PlayLooped(const AudioClipDesc& clip,
                                f32 volume,
                                f32 pitch) noexcept override;

    void StopVoice(AudioVoiceHandle voice) noexcept override;
    void SetVoiceVolume(AudioVoiceHandle voice, f32 volume) noexcept override;
    void StopAllVoices() noexcept override;
    u32  ActiveVoiceCount() const noexcept override;
    void Tick(f32 dt) noexcept override;

    // ----- 拡張 (XAudio2 固有) -----
    // マスタリングボイス音量 (= 最終出力の master volume)。0.0〜1.0 推奨、
    // XAudio2 自体は > 1.0 で over-amplification も許容するが歪むので非推奨。
    void SetMasterVolume(f32 volume) noexcept;

    // pimpl: XAudio2 / COM の重ヘッダを .cpp に閉じ込める。
    // public に置く理由 = .cpp 内の自由関数 (PlayInternal 等) から Impl の
    // メンバを直接触りたいため (acs::FAudioEngine と同じパターン)。
    struct Impl;

private:
    Impl* m_Impl = nullptr;
};

} // namespace acs::game
