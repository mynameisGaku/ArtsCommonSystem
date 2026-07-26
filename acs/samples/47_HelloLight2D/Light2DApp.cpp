// SPDX-License-Identifier: Apache-2.0
// HelloLight2D 実装。
#include "Light2DApp.h"

#include "app/Sample.h"
#include "platform/Input.h"
#include "render/IRhiSwapchain.h"
#include "foundation/Log.h"

#include <cstdio>
#include <cmath>

using namespace acs;

namespace hellolight2d {

namespace {

// 柱 (occluder) の配置。画面サイズに対する割合で持ち、scene/occluder 両パスで
// 同じ式から画素位置を出す (ズレ防止)。
struct FPillarFrac { f32 fx, fy, size; };
constexpr FPillarFrac kPillars[] = {
    {0.28f, 0.34f, 70.0f},
    {0.64f, 0.28f, 84.0f},
    {0.48f, 0.60f, 64.0f},
    {0.80f, 0.66f, 76.0f},
    {0.18f, 0.72f, 58.0f},
};
constexpr u32 kPillarCount = sizeof(kPillars) / sizeof(kPillars[0]);

u32 HashU32(u32 x) noexcept {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16; return x;
}

// 円形シルエット (柱/キャラ) を buf に焼く。color_rgb は内部色、外は alpha=0。
void FillCircle(u8* buf, u32 res, u8 cr, u8 cg, u8 cb) noexcept {
    const f32 c = (static_cast<f32>(res) - 1.0f) * 0.5f;
    const f32 rad = static_cast<f32>(res) * 0.5f - 1.0f;
    for (u32 y = 0; y < res; ++y) {
        for (u32 x = 0; x < res; ++x) {
            const f32 dx = static_cast<f32>(x) - c;
            const f32 dy = static_cast<f32>(y) - c;
            const f32 d  = std::sqrt(dx * dx + dy * dy);
            f32 a = rad - d;                 // 内側 +、外側 -
            a = a < 0.0f ? 0.0f : (a > 1.5f ? 1.0f : a / 1.5f);  // 1.5px の縁
            const u32 i = (y * res + x) * 4u;
            buf[i + 0] = cr; buf[i + 1] = cg; buf[i + 2] = cb;
            buf[i + 3] = static_cast<u8>(a * 255.0f + 0.5f);
        }
    }
}

} // namespace

void FLight2DApp::OnStart() noexcept {
    IRhiDevice*    dev = GetRenderer().Device();
    IRhiSwapchain* sc  = GetRenderer().Swapchain();
    if (!dev || !sc) { Quit(); return; }

    m_Width  = sc->Width();
    m_Height = sc->Height();
    const EFormat cf = GetRenderer().ColorFormat();

    ACS_SAMPLE_INIT(m_SceneBatch.Init(*dev, cf));
    ACS_SAMPLE_INIT(m_OccBatch.Init(*dev, cf));
    ACS_SAMPLE_INIT(m_HudBatch.Init(*dev, cf));
    ACS_SAMPLE_INIT(m_Lighting.Init(*dev, cf, m_Width, m_Height));
    ACS_SAMPLE_INIT(m_Blob.Init(*dev, 64));
    ACS_SAMPLE_INIT(BuildTextures(*dev));

    m_bFontReady = FSample::TryLoadDefaultUIFont(m_Font, *dev, 18.0f, 1024, false).IsOk();

    m_CharPos  = FVec2{static_cast<f32>(m_Width) * 0.5f, static_cast<f32>(m_Height) * 0.5f};
    m_TorchPos = m_CharPos;

    // Core Keeper 風: 暗い洞窟。柔らかめのソフト影品質。
    m_Lighting.SetAmbient(FVec3{0.05f, 0.05f, 0.08f});
    m_Lighting.SetShadowQuality(28, 7);
}

TResult<void> FLight2DApp::BuildTextures(IRhiDevice& device) noexcept {
    u8 buf[64 * 64 * 4] = {};

    // 床: 64x64 石ノイズ (不透明)。
    for (u32 y = 0; y < 64; ++y) {
        for (u32 x = 0; x < 64; ++x) {
            const u32 n = HashU32(x * 73856093u ^ y * 19349663u) & 0xFFu;
            const u32 v = 56u + (n % 46u);            // 56..101
            const u32 i = (y * 64 + x) * 4u;
            buf[i + 0] = static_cast<u8>(v);
            buf[i + 1] = static_cast<u8>(v * 92u / 100u);
            buf[i + 2] = static_cast<u8>(v * 82u / 100u);
            buf[i + 3] = 255;
        }
    }
    {
        FTextureDesc td{};
        td.width = 64; td.height = 64; td.format = EFormat::R8G8B8A8_UNorm;
        td.initial_data = buf; td.initial_data_size = 64u * 64u * 4u;
        auto r = CreateRhiTexture(device, td);
        if (r.IsErr()) return Err<void>(r.Error());
        m_FloorTex = Move(r.Value());
    }

    // 柱: 64x64 灰色の円 (石)。
    FillCircle(buf, 64, 150, 144, 136);
    {
        FTextureDesc td{};
        td.width = 64; td.height = 64; td.format = EFormat::R8G8B8A8_UNorm;
        td.initial_data = buf; td.initial_data_size = 64u * 64u * 4u;
        auto r = CreateRhiTexture(device, td);
        if (r.IsErr()) return Err<void>(r.Error());
        m_PillarTex = Move(r.Value());
    }

    // キャラ: 32x32 暖色の円。
    FillCircle(buf, 32, 232, 188, 130);
    {
        FTextureDesc td{};
        td.width = 32; td.height = 32; td.format = EFormat::R8G8B8A8_UNorm;
        td.initial_data = buf; td.initial_data_size = 32u * 32u * 4u;
        auto r = CreateRhiTexture(device, td);
        if (r.IsErr()) return Err<void>(r.Error());
        m_CharTex = Move(r.Value());
    }
    return Ok();
}

void FLight2DApp::DrawWorld(u32 w, u32 h) noexcept {
    // 床タイル
    for (u32 ty = 0; ty < h; ty += 64) {
        for (u32 tx = 0; tx < w; tx += 64) {
            m_SceneBatch.Draw(*m_FloorTex, static_cast<f32>(tx), static_cast<f32>(ty),
                              64.0f, 64.0f);
        }
    }
    // 柱
    for (u32 i = 0; i < kPillarCount; ++i) {
        const f32 s  = kPillars[i].size;
        const f32 px = kPillars[i].fx * static_cast<f32>(w) - s * 0.5f;
        const f32 py = kPillars[i].fy * static_cast<f32>(h) - s * 0.5f;
        m_SceneBatch.Draw(*m_PillarTex, px, py, s, s);
    }
    // キャラ足元のブロブ影 (簡易) → その上にキャラ
    m_Blob.Draw(m_SceneBatch, m_CharPos.x, m_CharPos.y + 16.0f, 40.0f, 20.0f, 0.55f);
    m_SceneBatch.Draw(*m_CharTex, m_CharPos.x - 16.0f, m_CharPos.y - 16.0f, 32.0f, 32.0f);
}

void FLight2DApp::DrawOccluders() noexcept {
    const FVec4 white{1, 1, 1, 1};
    const u32 w = m_Width, h = m_Height;
    for (u32 i = 0; i < kPillarCount; ++i) {
        const f32 s  = kPillars[i].size;
        const f32 px = kPillars[i].fx * static_cast<f32>(w) - s * 0.5f;
        const f32 py = kPillars[i].fy * static_cast<f32>(h) - s * 0.5f;
        m_OccBatch.Draw(*m_PillarTex, px, py, s, s, white);
    }
    // キャラも影を落とす
    m_OccBatch.Draw(*m_CharTex, m_CharPos.x - 16.0f, m_CharPos.y - 16.0f, 32.0f, 32.0f, white);
}

void FLight2DApp::UpdateLights(u32 w, u32 h) noexcept {
    const f32 fw = static_cast<f32>(w);
    const f32 fh = static_cast<f32>(h);

    m_Lighting.ClearLights();
    if (m_bBlobOnly) {
        // 軽量モード: フラットに明るく照らし、影は足元ブロブのみ。
        m_Lighting.SetAmbient(FVec3{0.9f, 0.88f, 0.85f});
        return;
    }
    m_Lighting.SetAmbient(FVec3{0.05f, 0.05f, 0.08f});

    // 1) マウス追従の松明 (暖色)
    FLight2D torch;
    torch.pos = m_TorchPos;
    torch.radius = 420.0f;
    torch.color = FVec3{1.0f, 0.72f, 0.42f};
    torch.intensity = 1.5f;
    torch.softness = 0.6f;
    m_Lighting.AddLight(torch);

    // 2) 静的な青ランプ
    FLight2D blue;
    blue.pos = FVec2{fw * 0.22f, fh * 0.30f};
    blue.radius = 300.0f;
    blue.color = FVec3{0.35f, 0.55f, 1.0f};
    blue.intensity = 1.1f;
    blue.softness = 0.5f;
    m_Lighting.AddLight(blue);

    // 3) 周回するオレンジ光
    FLight2D orbit;
    orbit.pos = FVec2{fw * 0.62f + std::cos(m_Time * 0.7f) * 160.0f,
                      fh * 0.58f + std::sin(m_Time * 0.7f) * 120.0f};
    orbit.radius = 320.0f;
    orbit.color = FVec3{1.0f, 0.45f, 0.2f};
    orbit.intensity = 1.2f;
    orbit.softness = 0.7f;
    m_Lighting.AddLight(orbit);
}

bool FLight2DApp::OnCustomFrame() noexcept {
    IRhiDevice*    dev = GetRenderer().Device();
    IRhiCommandList* cl = GetRenderer().CommandList();
    IRhiSwapchain* sc  = GetRenderer().Swapchain();
    if (!dev || !cl || !sc) return false;

    const u32 w = sc->Width();
    const u32 h = sc->Height();
    if (w == 0 || h == 0) return true;     // サイズ未確定のフレームはスキップ
    if (w != m_Width || h != m_Height) {
        m_Width = w; m_Height = h;
        (void)m_Lighting.Resize(w, h);
    }

    m_Time += 1.0f / 60.0f;

    // 入力: [B] でブロブ専用モード切替、Esc で終了、マウスが松明。
    if (FInput::IsKeyPressed(EKey::Escape)) { Quit(); return true; }
    if (FInput::IsKeyPressed(EKey::B))      { m_bBlobOnly = !m_bBlobOnly; }
    FVec2 m = FInput::MousePos();
    m.x = m.x < 0 ? 0 : (m.x > static_cast<f32>(w) ? static_cast<f32>(w) : m.x);
    m.y = m.y < 0 ? 0 : (m.y > static_cast<f32>(h) ? static_cast<f32>(h) : m.y);
    m_TorchPos = m;

    // キャラはゆっくり 8 の字に歩く。
    m_CharPos = FVec2{static_cast<f32>(w) * 0.5f + std::cos(m_Time * 0.5f) * 180.0f,
                      static_cast<f32>(h) * 0.5f + std::sin(m_Time * 1.0f) * 110.0f};

    UpdateLights(w, h);

    const u32 buf_idx = sc->AcquireNextImage();
    cl->Begin();

    // 1) world を scene RT へ
    m_Lighting.BeginScene(*cl, FVec4{0.02f, 0.02f, 0.03f, 1.0f});
    m_SceneBatch.Begin(*cl, w, h);
    DrawWorld(w, h);
    m_SceneBatch.End();
    m_Lighting.EndScene(*cl);

    // 2) occluder シルエットを occluder RT へ
    m_Lighting.BeginOccluders(*cl);
    m_OccBatch.Begin(*cl, w, h);
    DrawOccluders();
    m_OccBatch.End();
    m_Lighting.EndOccluders(*cl);

    // 3) backbuffer に scene × light を合成 + HUD
    cl->BeginRenderToSwapchain(*sc, buf_idx, FClearColor{0, 0, 0, 1});
    m_Lighting.Composite(*cl, w, h);

    if (m_bFontReady) {
        m_HudBatch.Begin(*cl, w, h);
        char line[128];
        std::snprintf(line, sizeof(line),
                      "HelloLight2D  -  mouse: torch   [B] blob-only: %s   lights: %u",
                      m_bBlobOnly ? "ON" : "OFF", m_Lighting.LightCount());
        m_HudBatch.DrawString(m_Font, line, 24.0f, 22.0f, FVec4{0.95f, 0.96f, 1.0f, 1.0f});
        m_HudBatch.DrawString(m_Font, "Esc: quit", 24.0f,
                              static_cast<f32>(h) - 34.0f, FVec4{0.6f, 0.62f, 0.7f, 1.0f});
        m_HudBatch.End();
    }

    cl->EndRenderToSwapchain(*sc, buf_idx);
    cl->End();
    if (!cl->Submit() || !sc->Present()) Quit();
    return true;
}

void FLight2DApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    if (m_bFontReady) m_Font.Shutdown();
    m_CharTex.Reset();
    m_PillarTex.Reset();
    m_FloorTex.Reset();
    m_Blob.Shutdown();
    m_Lighting.Shutdown();
    m_HudBatch.Shutdown();
    m_OccBatch.Shutdown();
    m_SceneBatch.Shutdown();
}

} // namespace hellolight2d
