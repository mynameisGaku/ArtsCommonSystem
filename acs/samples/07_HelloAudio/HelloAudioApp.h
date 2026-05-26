// SPDX-License-Identifier: Apache-2.0
// HelloAudio — Application 派生クラス。
//
// 動作:
//   ・カレントディレクトリの "test.wav" を読み込んで再生
//   ・Space で再生開始 / 停止、上下矢印で音量
//   ・Esc で終了
#pragma once

#include "app/Application.h"
#include "audio/AudioEngine.h"
#include "asset/Asset.h"
#include "memory/Rc.h"
#include "foundation/Types.h"

namespace helloaudio {

class HelloAudioApp : public acs::Application {
public:
    void OnStart()    noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnShutdown() noexcept override;

private:
    acs::AudioEngine _engine;
    acs::Rc<acs::Asset> _audio;
    acs::SoundHandle    _handle = acs::kInvalidSound;
    acs::f32            _volume = 1.0f;
};

} // namespace helloaudio
