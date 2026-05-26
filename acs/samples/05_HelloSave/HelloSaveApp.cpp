// SPDX-License-Identifier: Apache-2.0
// HelloSave — FApplication 実装。
#include "HelloSaveApp.h"

#include "app/Sample.h"
#include "platform/Input.h"

#include "memory/UniquePtr.h"
#include "foundation/Log.h"

#include <cstdio>

using namespace acs;

namespace hellosave {

void HelloSaveApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }

    // %APPDATA% 配下なら UAC でもユーザ書き込み可。Load 失敗は初回起動の正常系。
    ACS_SAMPLE_INIT(FStorage::GetAppDataPath(L"acs_demo", L"hello_save.ini",
                                             _save_path, 260));
    ACS_LOG_INFO("Save file: %ls", _save_path);
    if (auto r = _store.Load(_save_path); r.IsErr()) {
        ACS_LOG_WARN("FStorage::Load failed: %s (continuing with empty)", r.Error().message);
    }

    i64 launches = _store.GetInt("launches", 0) + 1;
    _store.SetInt("launches", launches);

    _clicks    = _store.GetInt("clicks", 0);
    _high_score = _store.GetInt("high_score", 0);
    const char* name = _store.GetString("player_name", "");
    if (name[0] == 0) {
        _store.SetString("player_name", "プレイヤー");
    }

    ACS_SAMPLE_INIT(_batch.Init(*dev, GetRenderer().ColorFormat()));
    (void)Sample::TryLoadDefaultUIFont(_font_big,   *dev, 32.0f, 1024, true);
    (void)Sample::TryLoadDefaultUIFont(_font_small, *dev, 18.0f, 1024, true);

    ACS_LOG_INFO("HelloSave: launches=%lld, clicks=%lld, high_score=%lld",
                 launches, static_cast<long long>(_clicks),
                 static_cast<long long>(_high_score));
}

void HelloSaveApp::OnUpdate(f32 /*dt*/) noexcept {
    if (FInput::IsKeyPressed(EKey::Escape)) Quit();

    if (FInput::IsKeyPressed(EKey::Space)) {
        ++_clicks;
        if (_clicks > _high_score) _high_score = _clicks;
        _dirty = true;
    }
    if (FInput::IsKeyPressed(EKey::R)) {
        // Clear() は launches も消すので、本セッションを 1 回目として再播種する。
        _store.Clear();
        _clicks = 0;
        _high_score = 0;
        _store.SetInt("launches", 1);
        _store.SetString("player_name", "プレイヤー");
        _dirty = true;
    }
    if (FInput::IsKeyPressed(EKey::S)) {
        FlushAndSave();
    }
}

void HelloSaveApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    if (!cl) return;
    const u32 sw = GetRenderer().Swapchain()->Width();
    const u32 sh = GetRenderer().Swapchain()->Height();

    _batch.Begin(*cl, sw, sh);
    _batch.DrawRect(0, 0, static_cast<f32>(sw), static_cast<f32>(sh),
                    FVec4{0.10f, 0.13f, 0.18f, 1});
    _batch.DrawRect(40, 40, static_cast<f32>(sw - 80), static_cast<f32>(sh - 80),
                    FVec4{0.16f, 0.20f, 0.28f, 1});

    if (_font_big.AtlasTexture()) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s さん、ようこそ！",
                      _store.GetString("player_name", "プレイヤー"));
        _batch.DrawString(_font_big, buf, 80, 80, FVec4{1, 0.95f, 0.7f, 1});
    }
    if (_font_small.AtlasTexture()) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "起動回数: %lld",
                      static_cast<long long>(_store.GetInt("launches", 0)));
        _batch.DrawString(_font_small, buf, 80, 150, FVec4{0.9f,0.95f,1,1});

        std::snprintf(buf, sizeof(buf), "今回のクリック数: %lld",
                      static_cast<long long>(_clicks));
        _batch.DrawString(_font_small, buf, 80, 180, FVec4{0.9f,0.95f,1,1});

        std::snprintf(buf, sizeof(buf), "ハイスコア: %lld",
                      static_cast<long long>(_high_score));
        _batch.DrawString(_font_small, buf, 80, 210,
                        (_clicks == _high_score && _clicks > 0)
                        ? FVec4{1, 0.85f, 0.4f, 1}    // ハイスコア更新中は黄色で強調
                        : FVec4{0.9f, 0.95f, 1, 1});

        _batch.DrawString(_font_small,
                        "Space: クリック  S: 手動保存  R: リセット  Esc: 終了 (自動保存)",
                        80, static_cast<f32>(sh - 80),
                        FVec4{0.6f, 0.7f, 0.85f, 1});

        if (_dirty) {
            _batch.DrawString(_font_small, "● 未保存（終了か S で保存）",
                            80, static_cast<f32>(sh - 110),
                            FVec4{1, 0.5f, 0.5f, 1});
        } else {
            _batch.DrawString(_font_small, "○ 保存済み",
                            80, static_cast<f32>(sh - 110),
                            FVec4{0.5f, 1, 0.5f, 1});
        }
    }
    _batch.End();
}

void HelloSaveApp::OnShutdown() noexcept {
    // GPU リソース破棄前に保存しておけば、保存中クラッシュでもプレイデータは無事。
    FlushAndSave();
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    _font_small.Shutdown();
    _font_big.Shutdown();
    _batch.Shutdown();
}

void HelloSaveApp::FlushAndSave() noexcept {
    _store.SetInt("clicks",     _clicks);
    _store.SetInt("high_score", _high_score);
    if (auto r = _store.Save(_save_path); r.IsErr()) {
        ACS_LOG_ERROR("Save failed: %s", r.Error().message);
    } else {
        _dirty = false;
        ACS_LOG_INFO("Saved to %ls", _save_path);
    }
}

} // namespace hellosave
