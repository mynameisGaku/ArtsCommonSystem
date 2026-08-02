// SPDX-License-Identifier: Apache-2.0
// HelloAudio — CApplication 実装。
#include "HelloAudioApp.h"

#include "app/Sample.h"
#include "platform/Input.h"
#include "asset/AudioAsset.h"
#include "foundation/Log.h"

using namespace acs;

namespace helloaudio {

void CHelloAudioApp::OnStart() noexcept {
    ACS_SAMPLE_INIT(m_Engine.Init());
    // wav / mp3 / flac / ogg のいずれでも OK（拡張子を変えるだけ）
    auto a = GetAssets().Load(L"test.wav");
    if (a.IsErr()) {
        ACS_LOG_ERROR("test.wav not found: %s", a.Error().message);
        // 音ファイルが無くてもアプリは動かせる
        return;
    }
    m_Audio = a.Value();
}

void CHelloAudioApp::OnUpdate(f32 /*dt*/) noexcept {
    if (CInput::IsKeyPressed(EKey::Escape)) Quit();

    if (CInput::IsKeyPressed(EKey::Space)) {
        if (m_Handle.IsValid()) {
            m_Engine.Stop(m_Handle);
            m_Handle = kInvalidSound;
        } else if (m_Audio) {
            auto* a = static_cast<AAudioAsset*>(m_Audio.Get());
            m_Handle = m_Engine.Play(*a, m_Volume, /*loop*/ false);
        }
    }
    if (CInput::IsKeyPressed(EKey::Up))   { m_Volume += 0.1f; if (m_Volume > 1) m_Volume = 1; m_Engine.SetVolume(m_Handle, m_Volume); }
    if (CInput::IsKeyPressed(EKey::Down)) { m_Volume -= 0.1f; if (m_Volume < 0) m_Volume = 0; m_Engine.SetVolume(m_Handle, m_Volume); }
}

void CHelloAudioApp::OnShutdown() noexcept {
    m_Engine.Shutdown();
}

} // namespace helloaudio
