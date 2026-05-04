// ACS の HelloText サンプル
//
// 動作:
//   ・複数サイズの英数 + 日本語テキストを描画
//   ・FPS / フレーム数を毎フレーム更新表示
//   ・タイトル文字が左右にゆっくり揺れる
//   ・Esc 終了
//
// 学習ポイント:
//   ・Font::LoadFromFile で TTF を読み込みアトラス焼き
//   ・SpriteBatch::DrawString で UTF-8 文字列を描画
//   ・Font::MeasureWidth でテキスト幅を取って中央寄せ
//
// 注: Windows 標準フォント (meiryo.ttc) を使うので、別 OS では別パス指定が必要

#include "app/Application.h"
#include "app/EntryPoint.h"
#include "app/Sample.h"
#include "platform/Input.h"

#include "render/SpriteBatch.h"
#include "render/Font.h"

#include "math/Math.h"
#include "memory/UniquePtr.h"
#include "foundation/Log.h"

#include <cstdio>

using namespace acs;

class HelloText : public Application {
public:
    void OnStart() noexcept override {
        IRhiDevice* dev = GetRenderer().Device();
        if (!dev) { Quit(); return; }

        // === SpriteBatch ===
        if (auto r = _batch.Init(*dev, GetRenderer().ColorFormat()); r.IsErr()) {
            ACS_LOG_ERROR("SpriteBatch init: %s", r.Error().message);
            Quit(); return;
        }

        // === フォント (3 サイズ) ===
        // Sample::TryLoadDefaultUIFont が OS 別の標準フォント候補を順に試す。
        // タイトル/本文は漢字込み (include_cjk=true、atlas 2048)、小さい方は速度優先で外す。
        if (Sample::TryLoadDefaultUIFont(_title_font, *dev, 64.0f, 2048, true).IsErr())  { Quit(); return; }
        if (Sample::TryLoadDefaultUIFont(_body_font,  *dev, 24.0f, 2048, true).IsErr())  { Quit(); return; }
        if (Sample::TryLoadDefaultUIFont(_small_font, *dev, 16.0f, 1024, false).IsErr()) { Quit(); return; }

        ACS_LOG_INFO("HelloText initialized");
    }

    void OnUpdate(f32 dt) noexcept override {
        if (Input::IsKeyPressed(Key::Escape)) Quit();
        _time += dt;
    }

    void OnRender() noexcept override {
        IRhiCommandList* cl = GetRenderer().CommandList();
        if (!cl) return;
        const u32 sw = GetRenderer().Swapchain()->Width();
        const u32 sh = GetRenderer().Swapchain()->Height();

        _batch.Begin(*cl, sw, sh);

        // === 背景の半透明パネル ===
        _batch.DrawRect(0, 0, static_cast<f32>(sw), static_cast<f32>(sh),
                        Vec4{0.08f, 0.10f, 0.14f, 1.0f});
        _batch.DrawRect(40, 40, static_cast<f32>(sw - 80), static_cast<f32>(sh - 80),
                        Vec4{0.14f, 0.18f, 0.24f, 1.0f});

        // === タイトル（揺れる）===
        const char* title = "Hello, ACS! 〜 始まりの一歩 〜";
        const f32 sway = Sin(_time * 1.5f) * 30.0f;
        const f32 title_w = _title_font.MeasureWidth(title);
        const f32 title_x = (static_cast<f32>(sw) - title_w) * 0.5f + sway;
        const f32 title_y = 80.0f;
        _batch.DrawString(_title_font, title, title_x + 3, title_y + 3,
                        Vec4{0, 0, 0, 0.5f});  // 影
        _batch.DrawString(_title_font, title, title_x, title_y,
                        Vec4{1.0f, 0.85f, 0.4f, 1.0f});

        // === サブタイトル（日本語混じり）===
        const char* subtitle = "ようこそ、ACS（軽量・C++ ゲーム基盤）へ";
        const f32 sub_w = _body_font.MeasureWidth(subtitle);
        _batch.DrawString(_body_font, subtitle,
                        (static_cast<f32>(sw) - sub_w) * 0.5f, 180.0f,
                        Vec4{0.85f, 0.9f, 1.0f, 1.0f});

        // === 説明テキスト（複数行、漢字混じり）===
        const char* body =
            "Phase 9: 漢字対応のテキスト描画\n"
            "・stb_truetype で 漢字 (CJK 統合 U+4E00..U+9FAF) を焼き込み\n"
            "・2048×2048 のアトラスに 約 2 万字を収録\n"
            "・SpriteBatch::DrawString でそのまま描画\n"
            "・吾輩は猫である。名前はまだ無い。\n"
            "・春はあけぼの。やうやう白くなりゆく山際、すこし明かりて";
        _batch.DrawString(_body_font, body, 100, 240,
                        Vec4{0.95f, 0.95f, 0.95f, 1.0f});

        // === 右下に FPS 表示 ===
        char fps_buf[64];
        std::snprintf(fps_buf, sizeof(fps_buf), "FPS: %.1f  Frame: %llu",
                      static_cast<double>(FPS()),
                      static_cast<unsigned long long>(FrameCount()));
        const f32 fps_w = _small_font.MeasureWidth(fps_buf);
        _batch.DrawString(_small_font, fps_buf,
                        static_cast<f32>(sw) - fps_w - 60.0f,
                        static_cast<f32>(sh) - 60.0f,
                        Vec4{0.6f, 0.7f, 0.8f, 1.0f});

        // === 操作ヒント ===
        _batch.DrawString(_small_font, "Esc で終了", 60.0f,
                        static_cast<f32>(sh) - 60.0f,
                        Vec4{0.5f, 0.5f, 0.55f, 1.0f});

        _batch.End();
    }

    void OnShutdown() noexcept override {
        if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
        _small_font.Shutdown();
        _body_font.Shutdown();
        _title_font.Shutdown();
        _batch.Shutdown();
    }

private:
    SpriteBatch _batch;
    Font _title_font, _body_font, _small_font;
    f32 _time = 0.0f;
};

ACS_DEFINE_MAIN(HelloText)
