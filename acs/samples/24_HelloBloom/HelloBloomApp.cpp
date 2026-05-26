// SPDX-License-Identifier: Apache-2.0
// HelloBloom — HelloBloomApp 実装。HDR シーンを Bloom + ACES Tonemap で出す。
#include "HelloBloomApp.h"

#include "app/Sample.h"
#include "platform/Input.h"
#include "asset/MeshPrimitive.h"
#include "asset/MeshAsset.h"
#include "math/Mat.h"
#include "math/Math.h"
#include "foundation/Log.h"

#include <cstdio>

using namespace acs;

namespace hellobloom {

void HelloBloomApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }
    IRhiSwapchain* sc = GetRenderer().Swapchain();
    if (!sc) { Quit(); return; }

    const u32 sw = sc->Width();
    const u32 sh = sc->Height();

    ACS_SAMPLE_INIT(_post.Init(*dev, sw, sh, GetRenderer().ColorFormat()));

    // シーン描画は HDR RT へ流すので shader / sky の RT format も HDR に合わせる。
    ACS_SAMPLE_INIT(_shader.Init(*dev, _post.HdrFormat(), GetRenderer().DepthFormat()));
    ACS_SAMPLE_INIT(_sky.Init(*dev, _post.HdrFormat(), GetRenderer().DepthFormat()));
    _sky.PresetNight();

    auto sphere = Primitive::MakeSphere(0.5f, 32, 16);
    auto plane  = Primitive::MakePlane(20.0f, 20.0f);
    ACS_SAMPLE_INIT(UploadMesh(*dev, *sphere, _gm_sphere));
    ACS_SAMPLE_INIT(UploadMesh(*dev, *plane,  _gm_plane));

    // HUD は Tonemap 後の LDR backbuffer に書くので backbuffer format で初期化する。
    ACS_SAMPLE_INIT(_batch.Init(*dev, GetRenderer().ColorFormat()));
    (void)Sample::TryLoadDefaultUIFont(_font, *dev, 18.0f);

    const f32 aspect = static_cast<f32>(sw) / static_cast<f32>(sh);
    _camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 100.0f);

    ACS_LOG_INFO("HelloBloom: backend=%s, HDR=%dx%d",
                 dev->BackendName(), sw, sh);
}

void HelloBloomApp::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) Quit();
    if (Input::IsKeyPressed(EKey::Num1)) _params.bloom_intensity = 0.0f;
    if (Input::IsKeyPressed(EKey::Num2)) _params.bloom_intensity = 0.6f;
    if (Input::IsKeyPressed(EKey::Num3)) _params.bloom_intensity = 1.5f;

    _angle += dt * 0.5f;
    const f32 cam_dist = 7.0f;
    _cam_yaw += (Input::IsKeyDown(EKey::Right) ? 1.0f : 0.0f) * dt;
    _cam_yaw -= (Input::IsKeyDown(EKey::Left)  ? 1.0f : 0.0f) * dt;
    Vec3 cam{ Sin(_cam_yaw) * cam_dist, 2.0f, -Cos(_cam_yaw) * cam_dist };
    _camera.SetLookAt(cam, Vec3{0, 1, 0});
}

// true を返して Application の既定フレームフローを置き換える (HDR RT を挟むため)。
bool HelloBloomApp::OnCustomFrame() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    IRhiSwapchain*   sc = GetRenderer().Swapchain();
    IRhiTexture*     hdr = _post.HdrRenderTarget();
    IRhiTexture*     depth = GetRenderer().DepthBuffer();
    // false を返すと Application が既定フローでフレームを完走してくれる (失敗時の保険)。
    if (!cl || !sc || !hdr) return false;

    const u32 buf_idx = sc->AcquireNextImage();
    cl->Begin();

    // 1) HDR RT にシーンを描く
    cl->BeginRenderToTexture(*hdr, ClearColor{0,0,0,1}, depth, 1.0f);

    Viewport vp{}; vp.width  = static_cast<f32>(hdr->Width());
                   vp.height = static_cast<f32>(hdr->Height());
    cl->SetViewport(vp);
    ScissorRect svr{}; svr.right  = static_cast<i32>(hdr->Width());
                      svr.bottom = static_cast<i32>(hdr->Height());
    cl->SetScissor(svr);

    _sky.Render(*cl, _camera);

    DirLight dl;
    dl.direction = _sky.SunDirection();
    dl.color     = _sky.SunColor();
    Vec3 ambient{0.05f, 0.06f, 0.10f};
    _shader.SetLights(_camera.ViewProjection(), _camera.Eye(), &dl, 1, ambient);

    cl->SetPipeline(*_shader.Pipeline());
    cl->SetConstantBuffer(0, *_shader.PerFrameCB());
    cl->SetConstantBuffer(1, *_shader.PerObjectCB());
    cl->SetTexture(0, *_shader.DefaultWhiteTexture());
    cl->SetTexture(1, *_shader.ShadowTextureOrDefault());

    // 地面: 暗めの色にして Bloom 対象 (球) のコントラストを稼ぐ。
    _shader.SetObject(Mat4::Translation(Vec3{0, 0, 0}),
                      Vec3{0.10f, 0.12f, 0.15f}, 0.05f, 8.0f);
    cl->SetVertexBuffer(*_gm_plane.vertex_buffer, _gm_plane.vertex_stride);
    cl->SetIndexBuffer(*_gm_plane.index_buffer);
    cl->DrawIndexed(_gm_plane.index_count);

    // HDR > 1.0 の色を 4 個並べる: bloom_threshold (既定 1.0) を超える成分にだけ
    // Bloom が乗る、ということを目視で示すためのデモオブジェクト。
    const Vec3 colors[4] = {
        {6.0f, 1.0f, 0.5f},
        {0.5f, 6.0f, 1.5f},
        {1.0f, 1.5f, 8.0f},
        {5.0f, 5.0f, 1.0f},
    };
    for (u32 i = 0; i < 4; ++i) {
        const f32 a = _angle + i * (kPi * 0.5f);
        const Vec3 pos{ Cos(a) * 3.0f, 1.5f, Sin(a) * 3.0f };
        Mat4 m = Mat4::Translation(pos);
        _shader.SetObject(m, colors[i], 0.0f, 1.0f);
        cl->SetVertexBuffer(*_gm_sphere.vertex_buffer, _gm_sphere.vertex_stride);
        cl->SetIndexBuffer(*_gm_sphere.index_buffer);
        cl->DrawIndexed(_gm_sphere.index_count);
    }

    cl->EndRenderToTexture(*hdr);

    // 2) Bloom + Tonemap を一気にかけて swapchain に合成。
    _post.Render(*cl, *sc, buf_idx, _params);

    // 3) HUD は Tonemap 後の LDR backbuffer に書く (HDR 値が HUD 色を吹き飛ばさないため)。
    //    PostProcess::Render が既に swapchain を RT としてバインドしてくれている。
    if (_font.AtlasTexture()) {
        const u32 sw = sc->Width();
        const u32 sh = sc->Height();
        _batch.Begin(*cl, sw, sh);
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "Bloom intensity = %.2f   FPS = %.1f",
                      _params.bloom_intensity, FPS());
        _batch.DrawString(_font, buf, 20, 20, Vec4{1,1,1,1});
        _batch.DrawString(_font,
                        "1: off  2: 0.6  3: 1.5  ←→: camera  Esc: 終了",
                        20, 44, Vec4{0.8f,0.85f,0.95f,1});
        _batch.End();
    }

    cl->EndRenderToSwapchain(*sc, buf_idx);
    cl->End();
    cl->Submit();
    sc->Present();
    return true;
}

void HelloBloomApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    _font.Shutdown();
    _batch.Shutdown();
    _gm_plane  = GpuMesh{};
    _gm_sphere = GpuMesh{};
    _shader.Shutdown();
    _sky.Shutdown();
    _post.Shutdown();
}

} // namespace hellobloom
