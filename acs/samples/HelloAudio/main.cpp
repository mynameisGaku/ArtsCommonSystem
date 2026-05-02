// ACS の Hello Audio サンプル
//
// 動作:
//   ・カレントディレクトリの "test.wav" を読み込んで再生
//   ・Space で再生開始 / 停止、上下矢印で音量
//   ・Esc で終了

#include "app/Application.h"
#include "app/EntryPoint.h"
#include "platform/Input.h"
#include "audio/AudioEngine.h"
#include "asset/AudioAsset.h"
#include "foundation/Log.h"

using namespace acs;

class HelloAudio : public Application {
public:
    void OnStart() noexcept override {
        if (auto r = _engine.Init(); r.IsErr()) {
            ACS_LOG_ERROR("Audio init failed: %s", r.Error().message);
            Quit();
            return;
        }
        // wav / mp3 / flac / ogg のいずれでも OK（拡張子を変えるだけ）
        auto a = GetAssets().Load(L"test.wav");
        if (a.IsErr()) {
            ACS_LOG_ERROR("test.wav not found: %s", a.Error().message);
            // 音ファイルが無くてもアプリは動かせる
            return;
        }
        _audio = a.Value();
    }

    void OnUpdate(f32 /*dt*/) noexcept override {
        if (Input::IsKeyPressed(Key::Escape)) Quit();

        if (Input::IsKeyPressed(Key::Space)) {
            // 再生中なら止める、止まっていれば再生
            if (_handle.IsValid()) {
                _engine.Stop(_handle);
                _handle = kInvalidSound;
            } else if (_audio) {
                auto* a = static_cast<AudioAsset*>(_audio.Get());
                _handle = _engine.Play(*a, _volume, /*loop*/ false);
            }
        }
        if (Input::IsKeyPressed(Key::Up))   { _volume += 0.1f; if (_volume > 1) _volume = 1; _engine.SetVolume(_handle, _volume); }
        if (Input::IsKeyPressed(Key::Down)) { _volume -= 0.1f; if (_volume < 0) _volume = 0; _engine.SetVolume(_handle, _volume); }
    }

    void OnShutdown() noexcept override {
        _engine.Shutdown();
    }

private:
    AudioEngine _engine;
    Rc<Asset>   _audio;
    SoundHandle _handle = kInvalidSound;
    f32         _volume = 1.0f;
};

ACS_DEFINE_MAIN(HelloAudio)
