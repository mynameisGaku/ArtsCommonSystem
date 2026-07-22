// SPDX-License-Identifier: Apache-2.0
// HelloText — HelloTextApp 実装。
//
// 動作:
//   ・複数サイズの英数 + 日本語テキストを描画
//   ・FPS / フレーム数を毎フレーム更新表示
//   ・タイトル文字が左右にゆっくり揺れる
//   ・Esc 終了
#include "HelloTextApp.h"

#include "app/Sample.h"
#include "platform/Input.h"
#include "math/Math.h"
#include "memory/UniquePtr.h"
#include "foundation/Log.h"

#include <cstdio>

namespace hellotext {

using namespace acs;

void FHelloTextApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }

    ACS_SAMPLE_INIT(m_Batch.Init(*dev, GetRenderer().ColorFormat()));

    // タイトル/本文は 2048×2048 の CJK アトラス (≒ 2 万字)、小さい方は速度優先で
    // 漢字を外す。FSample::TryLoadDefaultUIFont が OS 別の標準フォント候補を順に試す。
    ACS_SAMPLE_INIT(FSample::TryLoadDefaultUIFont(m_TitleFont, *dev, 64.0f, 2048, true));
    ACS_SAMPLE_INIT(FSample::TryLoadDefaultUIFont(m_BodyFont,  *dev, 24.0f, 2048, true));
    ACS_SAMPLE_INIT(FSample::TryLoadDefaultUIFont(m_SmallFont, *dev, 16.0f, 1024, false));

    ACS_LOG_INFO("HelloText initialized");
}

void FHelloTextApp::OnUpdate(f32 dt) noexcept {
    if (FInput::IsKeyPressed(EKey::Escape)) Quit();
    m_Time += dt;
}

void FHelloTextApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    if (!cl) return;
    const u32 sw = GetRenderer().Swapchain()->Width();
    const u32 sh = GetRenderer().Swapchain()->Height();

    m_Batch.Begin(*cl, sw, sh);

    // 背景の二段パネル (外枠 + 内枠)
    m_Batch.DrawRect(0, 0, static_cast<f32>(sw), static_cast<f32>(sh),
                    FVec4{0.08f, 0.10f, 0.14f, 1.0f});
    m_Batch.DrawRect(40, 40, static_cast<f32>(sw - 80), static_cast<f32>(sh - 80),
                    FVec4{0.14f, 0.18f, 0.24f, 1.0f});

    // タイトル: sin で左右に揺らしながら中央寄せ。影は本体より +3px オフセット。
    const char* title = "Hello, ACS! 〜 始まりの一歩 〜";
    const f32 sway = Sin(m_Time * 1.5f) * 30.0f;
    const f32 title_w = m_TitleFont.MeasureWidth(title);
    const f32 title_x = (static_cast<f32>(sw) - title_w) * 0.5f + sway;
    const f32 title_y = 80.0f;
    m_Batch.DrawString(m_TitleFont, title, title_x + 3, title_y + 3,
                    FVec4{0, 0, 0, 0.5f});
    m_Batch.DrawString(m_TitleFont, title, title_x, title_y,
                    FVec4{1.0f, 0.85f, 0.4f, 1.0f});

    // サブタイトル (中央寄せ)
    const char* subtitle = "ようこそ、ACS（軽量・C++ ゲーム基盤）へ";
    const f32 sub_w = m_BodyFont.MeasureWidth(subtitle);
    m_Batch.DrawString(m_BodyFont, subtitle,
                    (static_cast<f32>(sw) - sub_w) * 0.5f, 180.0f,
                    FVec4{0.85f, 0.9f, 1.0f, 1.0f});

    // 説明テキスト (複数行、漢字混じり)
    const char* body =
        "漢字対応のテキスト描画\n"
        "・stb_truetype で 漢字 (CJK 統合 U+4E00..U+9FAF) を焼き込み\n"
        "・2048×2048 のアトラスに 約 2 万字を収録\n"
        "・FSpriteBatch::DrawString でそのまま描画\n"
        "・吾輩は猫である。名前はまだ無い。\n"
        "・春はあけぼの。やうやう白くなりゆく山際、すこし明かりて";
    m_Batch.DrawString(m_BodyFont, body, 100, 240,
                    FVec4{0.95f, 0.95f, 0.95f, 1.0f});

    // 右下: FPS / フレーム数
    char fps_buf[64];
    std::snprintf(fps_buf, sizeof(fps_buf), "FPS: %.1f  Frame: %llu",
                  static_cast<double>(FPS()),
                  static_cast<unsigned long long>(FrameCount()));
    const f32 fps_w = m_SmallFont.MeasureWidth(fps_buf);
    m_Batch.DrawString(m_SmallFont, fps_buf,
                    static_cast<f32>(sw) - fps_w - 60.0f,
                    static_cast<f32>(sh) - 60.0f,
                    FVec4{0.6f, 0.7f, 0.8f, 1.0f});

    // 左下: 操作ヒント
    m_Batch.DrawString(m_SmallFont, "Esc で終了", 60.0f,
                    static_cast<f32>(sh) - 60.0f,
                    FVec4{0.5f, 0.5f, 0.55f, 1.0f});

    m_Batch.End();
}

void FHelloTextApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    m_SmallFont.Shutdown();
    m_BodyFont.Shutdown();
    m_TitleFont.Shutdown();
    m_Batch.Shutdown();
}

} // namespace hellotext
