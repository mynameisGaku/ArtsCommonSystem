// SPDX-License-Identifier: Apache-2.0
// HelloAudio — FApplication 派生クラス。
//
// 動作:
//   ・カレントディレクトリの "test.wav" を読み込んで再生
//   ・Space で再生開始 / 停止、上下矢印で音量
//   ・Esc で終了
#pragma once

#include "app/Application.h"
#include "audio/AudioEngine.h"
#include "asset/Asset.h"
#include "memory/SharedPtr.h"
#include "foundation/Types.h"

namespace helloaudio {

class FHelloAudioApp : public acs::FApplication {
public:
    void OnStart()    noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnShutdown() noexcept override;

private:
    acs::CAudioEngine m_Engine;
    acs::TSharedPtr<acs::FAsset> m_Audio;
    acs::FSoundHandle    m_Handle = acs::kInvalidSound;
    acs::f32            m_Volume = 1.0f;
};

} // namespace helloaudio
