// SPDX-License-Identifier: Apache-2.0
// HelloPbr — HelloPbrApp 実装。
#include "HelloPbrApp.h"

#include "app/Sample.h"
#include "platform/Input.h"

#include "asset/MeshPrimitive.h"
#include "asset/MeshAsset.h"

#include "math/Mat.h"
#include "math/Math.h"

#include "memory/UniquePtr.h"
#include "foundation/Log.h"

#include <cstdio>

using namespace acs;

namespace hellopbr {

void HelloPbrApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }

    ACS_SAMPLE_INIT(_shader.Init(*dev, GetRenderer().ColorFormat(), GetRenderer().DepthFormat()));

    auto sphere = Primitive::MakeSphere(0.55f, 48, 24);
    auto plane  = Primitive::MakePlane(20.0f, 20.0f);
    ACS_SAMPLE_INIT(UploadMesh(*dev, *sphere, _gm_sphere));
    ACS_SAMPLE_INIT(UploadMesh(*dev, *plane,  _gm_plane));

    ACS_SAMPLE_INIT(_batch.Init(*dev, GetRenderer().ColorFormat()));
    (void)FSample::TryLoadDefaultUIFont(_font, *dev, 18.0f, 1024, true);

    const f32 aspect = static_cast<f32>(GetRenderer().Swapchain()->Width()) /
                       static_cast<f32>(GetRenderer().Swapchain()->Height());
    _camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 100.0f);
    _cam_pos = FVec3{0, 2.0f, -7.5f};
}

void HelloPbrApp::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) Quit();
    _time += dt;

    const f32 mv = 5.0f * dt, tr = 1.5f * dt;
    if (Input::IsKeyDown(EKey::Left))  _cam_yaw -= tr;
    if (Input::IsKeyDown(EKey::Right)) _cam_yaw += tr;
    if (Input::IsKeyDown(EKey::Up))    _cam_pitch -= tr * 0.8f;
    if (Input::IsKeyDown(EKey::Down))  _cam_pitch += tr * 0.8f;
    const f32 limit = 0.45f * kPi;
    if (_cam_pitch >  limit) _cam_pitch =  limit;
    if (_cam_pitch < -limit) _cam_pitch = -limit;
    FVec3 forward{ Sin(_cam_yaw) * Cos(_cam_pitch),
                 -Sin(_cam_pitch),
                  Cos(_cam_yaw) * Cos(_cam_pitch) };
    FVec3 right{ Cos(_cam_yaw), 0, -Sin(_cam_yaw) };
    if (Input::IsKeyDown(EKey::W)) _cam_pos += forward * mv;
    if (Input::IsKeyDown(EKey::S)) _cam_pos -= forward * mv;
    if (Input::IsKeyDown(EKey::D)) _cam_pos += right   * mv;
    if (Input::IsKeyDown(EKey::A)) _cam_pos -= right   * mv;
    _camera.SetLookAt(_cam_pos, _cam_pos + forward);
}

void HelloPbrApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    if (!cl) return;

    FDirLight lights[1];
    // direction = surface から light source へ向くベクトル (y+ = 上空の太陽)。
    // HelloLights / HelloRaycast3D の慣習に合わせて y を正にする。
    lights[0].direction = FVec3{0.4f, 0.8f, -0.5f};
    // LDR backbuffer (B8G8R8A8_UNorm) なので強度 1.x 程度に抑えて clip 回避。
    lights[0].color     = FVec3{1.4f, 1.33f, 1.26f};

    // 旋回する点光源で highlight をハッキリ見せる
    PointLight pts[1];
    pts[0].position = FVec3{Sin(_time * 0.6f) * 4.0f, 1.5f,
                          Cos(_time * 0.6f) * 4.0f - 2.0f};
    pts[0].color    = FVec3{0.7f, 0.6f, 1.0f};
    pts[0].range    = 8.0f;

    FVec3 ambient{0.05f, 0.06f, 0.08f};
    _shader.SetLights(_camera.ViewProjection(), _camera.Eye(),
                     lights, 1, ambient);
    _shader.SetPointLights(pts, 1);

    cl->SetPipeline(*_shader.Pipeline());
    cl->SetConstantBuffer(0, *_shader.PerFrameCB());
    cl->SetConstantBuffer(1, *_shader.PerObjectCB());
    cl->SetTexture(0, *_shader.DefaultWhiteTexture());
    // PBR pipeline は IBL 用 texture slot 1-3 も持つので fallback を bind
    // しておく (HelloPbr では IBL を使わないので shader 側で uniform-branch
    // して flat ambient に落ちる)
    _shader.BindIblTextures(*cl);

    // 地面 (PBR で metallic=0, roughness=0.8 の灰色)
    _shader.SetObject(FMat4::Translation(FVec3{0, -0.6f, 0}),
                      FVec3{0.5f, 0.5f, 0.5f}, 0.0f, 0.8f, 1.0f);
    cl->SetVertexBuffer(*_gm_plane.vertex_buffer, _gm_plane.vertex_stride);
    cl->SetIndexBuffer(*_gm_plane.index_buffer);
    cl->DrawIndexed(_gm_plane.index_count);

    // material grid: X = metallic 0..1, Y = roughness 0..1
    cl->SetVertexBuffer(*_gm_sphere.vertex_buffer, _gm_sphere.vertex_stride);
    cl->SetIndexBuffer(*_gm_sphere.index_buffer);
    const FVec3 base{0.85f, 0.4f, 0.3f};      // 銅っぽい色
    for (u32 y = 0; y < kGridSize; ++y) {
        for (u32 x = 0; x < kGridSize; ++x) {
            const f32 metallic  = static_cast<f32>(x) / (kGridSize - 1);
            const f32 roughness = 0.05f + static_cast<f32>(y) / (kGridSize - 1) * 0.95f;
            const f32 px = (static_cast<f32>(x) - (kGridSize - 1) * 0.5f) * kSpacing;
            // +3.0 でグリッド全体を床より上に。床 y=-0.6 + sphere radius 0.55 を考慮し
            // row 0 (y=0) の sphere bottom (py - 0.55) が床より上になるように +3.0 を選択。
            const f32 py = (static_cast<f32>(y) - (kGridSize - 1) * 0.5f) * kSpacing + 3.0f;
            _shader.SetObject(FMat4::Translation(FVec3{px, py, 2.0f}),
                              base, metallic, roughness, 1.0f);
            cl->DrawIndexed(_gm_sphere.index_count);
        }
    }

    if (_font.AtlasTexture()) {
        const u32 sw = GetRenderer().Swapchain()->Width();
        const u32 sh = GetRenderer().Swapchain()->Height();
        _batch.Begin(*cl, sw, sh);
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "PBR: %ux%u sphere (X→metallic, Y→roughness)  FPS: %.1f",
                      kGridSize, kGridSize, static_cast<double>(FPS()));
        _batch.DrawString(_font, buf, 20, 20, FVec4{1, 1, 1, 1});
        _batch.DrawString(_font,
                          "WASD: 移動  矢印: 視点  Esc: 終了",
                          20, 44, FVec4{0.7f, 0.85f, 1.0f, 1});
        _batch.End();
    }
}

void HelloPbrApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    _font.Shutdown();
    _batch.Shutdown();
    _gm_plane  = GpuMesh{};
    _gm_sphere = GpuMesh{};
    _shader.Shutdown();
}

} // namespace hellopbr
