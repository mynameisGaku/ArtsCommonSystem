// SPDX-License-Identifier: Apache-2.0
// HelloAudio — FApplication 実装。
#include "HelloAudioApp.h"

#include "app/Sample.h"
#include "platform/Input.h"
#include "asset/AudioAsset.h"
#include "foundation/Log.h"

using namespace acs;

namespace helloaudio {

void HelloAudioApp::OnStart() noexcept {
    ACS_SAMPLE_INIT(_engine.Init());
    // wav / mp3 / flac / ogg のいずれでも OK（拡張子を変えるだけ）
    auto a = GetAssets().Load(L"test.wav");
    if (a.IsErr()) {
        ACS_LOG_ERROR("test.wav not found: %s", a.Error().message);
        // 音ファイルが無くてもアプリは動かせる
        return;
    }
    _audio = a.Value();
}

void HelloAudioApp::OnUpdate(f32 /*dt*/) noexcept {
    if (FInput::IsKeyPressed(EKey::Escape)) Quit();

    if (FInput::IsKeyPressed(EKey::Space)) {
        if (_handle.IsValid()) {
            _engine.Stop(_handle);
            _handle = kInvalidSound;
        } else if (_audio) {
            auto* a = static_cast<FAudioAsset*>(_audio.Get());
            _handle = _engine.Play(*a, _volume, /*loop*/ false);
        }
    }
    if (FInput::IsKeyPressed(EKey::Up))   { _volume += 0.1f; if (_volume > 1) _volume = 1; _engine.SetVolume(_handle, _volume); }
    if (FInput::IsKeyPressed(EKey::Down)) { _volume -= 0.1f; if (_volume < 0) _volume = 0; _engine.SetVolume(_handle, _volume); }
}

void HelloAudioApp::OnShutdown() noexcept {
    _engine.Shutdown();
}

} // namespace helloaudio
