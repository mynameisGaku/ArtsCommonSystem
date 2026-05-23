// SPDX-License-Identifier: Apache-2.0
// ACS の Hello Window サンプル
//
// 動作:
//   ・ウィンドウを開く
//   ・背景を毎フレーム指定色でクリア
//   ・WASD で背景色を変える
//   ・Esc で終了
//   ・タイトルバーに FPS を表示

#include "app/Application.h"
#include "app/EntryPoint.h"
#include "platform/Input.h"
#include "foundation/Log.h"

#include <cwchar>

using namespace acs;

class HelloWindow : public Application {
public:
    void OnStart() noexcept override {
        // 起動時に 1 度だけ呼ばれる
        ACS_LOG_INFO("HelloWindow started");
    }

    void OnUpdate(f32 dt) noexcept override {
        // Esc 押下で終了
        if (Input::IsKeyPressed(EKey::Escape)) Quit();

        // WASD で背景色を変える（押している間ずっと変化）
        if (Input::IsKeyDown(EKey::W)) _r += dt;
        if (Input::IsKeyDown(EKey::S)) _r -= dt;
        if (Input::IsKeyDown(EKey::A)) _g += dt;
        if (Input::IsKeyDown(EKey::D)) _g -= dt;
        // 0..1 にクランプ
        if (_r < 0) _r = 0; if (_r > 1) _r = 1;
        if (_g < 0) _g = 0; if (_g > 1) _g = 1;

        // 更新した色を背景クリア色に反映する（次フレームから画面の色が変わる）
        SetClearColor(_r, _g, _b);

        // タイトルバーに FPS を表示（30 フレームごとに更新）
        if (FrameCount() % 30 == 0) {
            wchar_t title[128];
            ::swprintf_s(title, L"HelloWindow  |  FPS: %.1f  |  RGB: (%.2f, %.2f, %.2f)",
                         FPS(), _r, _g, _b);
            GetWindow().SetTitle(title);
        }
    }

    void OnRender() noexcept override {
        // 描画コマンドを追加するならここ。Hello Window では何もしない。
    }

    void OnShutdown() noexcept override {
        ACS_LOG_INFO("HelloWindow shutting down");
    }

    void OnEvent(const Event& e) noexcept override {
        // 例: × ボタン以外でも Esc キーで終了したいなら OnUpdate のフックで OK
        if (e.type == EventType::WindowResize) {
            ACS_LOG_INFO("Window resized to %ux%u", e.resize.width, e.resize.height);
        }
    }

private:
    f32 _r = 0.1f;
    f32 _g = 0.12f;
    f32 _b = 0.16f;
};

ACS_DEFINE_MAIN(HelloWindow)
