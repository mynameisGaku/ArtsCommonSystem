// SPDX-License-Identifier: Apache-2.0
// HelloSky — SkyScene 実装。
#include "SkyScene.h"

#include "math/Math.h"

using namespace acs;

namespace hellosky {

void SkyScene::SetPreset(Sky& sky, SkyPreset p) noexcept {
    _preset = p;
    switch (p) {
        case SkyPreset::Day:    sky.PresetDay();    break;
        case SkyPreset::Sunset: sky.PresetSunset(); break;
        case SkyPreset::Night:  sky.PresetNight();  break;
    }
}

void SkyScene::Render(Sky&             sky,
                      StandardShader&  shader,
                      IRhiCommandList& cl,
                      const Camera&    camera,
                      const GpuMesh&   plane,
                      const GpuMesh&   sphere,
                      f32              angle) noexcept {
    // 1) 空を最初に描く
    sky.Render(cl, camera);

    // 2) シーン描画 — メイン光源を Sky の太陽方向 + 色と合わせる
    DirLight lights[2];
    lights[0].direction = sky.SunDirection();
    lights[0].color     = sky.SunColor();
    // 環境光相当のフィル (Sky の zenith を弱めて)
    lights[1].direction = Vec3{-sky.SunDirection().x,
                                sky.SunDirection().y * 0.5f,
                               -sky.SunDirection().z};
    lights[1].color     = Vec3{0.15f, 0.18f, 0.25f};

    Vec3 ambient;
    switch (_preset) {
        case SkyPreset::Day:    ambient = kAmbientDay;    break;
        case SkyPreset::Sunset: ambient = kAmbientSunset; break;
        default:                ambient = kAmbientNight;  break;
    }

    shader.SetLights(camera.ViewProjection(), camera.Eye(),
                     lights, 2, ambient);

    cl.SetPipeline(*shader.Pipeline());
    cl.SetConstantBuffer(0, *shader.PerFrameCB());
    cl.SetConstantBuffer(1, *shader.PerObjectCB());
    cl.SetTexture(0, *shader.DefaultWhiteTexture());
    cl.SetTexture(1, *shader.ShadowTextureOrDefault());

    // 地面
    shader.SetObject(Mat4::Translation(Vec3{0, 0, 0}),
                     Vec3{0.4f, 0.45f, 0.5f}, 0.1f, 8.0f);
    cl.SetVertexBuffer(*plane.vertex_buffer, plane.vertex_stride);
    cl.SetIndexBuffer(*plane.index_buffer);
    cl.DrawIndexed(plane.index_count);

    // 球
    Mat4 ms = Mat4::RotationY(angle) *
              Mat4::Translation(Vec3{0, 1.5f, 0});
    shader.SetObject(ms, Vec3{1.0f, 0.85f, 0.4f}, 0.7f, 64.0f);
    cl.SetVertexBuffer(*sphere.vertex_buffer, sphere.vertex_stride);
    cl.SetIndexBuffer(*sphere.index_buffer);
    cl.DrawIndexed(sphere.index_count);
}

} // namespace hellosky
