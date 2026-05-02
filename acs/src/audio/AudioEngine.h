// XAudio2 ベースの音声エンジン
//
// 使い方:
//   AudioEngine engine;
//   engine.Init();
//
//   // AudioAsset (wav/mp3/flac/ogg) を Asset モジュールから取得
//   Rc<Asset> asset = registry.Load(L"sound/bgm.ogg").Value();
//   auto* audio = static_cast<AudioAsset*>(asset.Get());
//
//   // 再生 (volume 0..1, loop は繰り返し)
//   SoundHandle h = engine.Play(*audio, 1.0f, /*loop=*/true);
//
//   // 停止 / 音量変更
//   engine.SetVolume(h, 0.5f);
//   engine.Stop(h);
//
//   engine.Shutdown();
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "memory/Rc.h"
#include "threading/Mutex.h"
#include "container/Array.h"
#include "audio/SoundHandle.h"
#include "asset/AudioAsset.h"

namespace acs {

class AudioEngine {
public:
    AudioEngine() noexcept = default;
    ~AudioEngine() noexcept;

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // XAudio2 を初期化
    Result<void> Init() noexcept;
    // 全停止 + 解放
    void Shutdown() noexcept;

    // 音声を再生開始
    //   volume: 0.0..1.0
    //   loop  : true なら無限ループ
    SoundHandle Play(const AudioAsset& asset, f32 volume = 1.0f, bool loop = false) noexcept;

    // 停止 (内部スロットも解放)
    void Stop(SoundHandle h) noexcept;

    // 音量変更 (0.0..1.0、超過は内部でクランプ)
    void SetVolume(SoundHandle h, f32 volume) noexcept;

    // 全音停止
    void StopAll() noexcept;

    // 現在再生中のスロット数 (デバッグ用)
    u32 ActiveCount() const noexcept;

private:
    struct Impl;
    Impl* _impl = nullptr;  // XAudio2 ハンドル等を隠蔽 (XAudio2 ヘッダを公開しないため)
};

} // namespace acs
