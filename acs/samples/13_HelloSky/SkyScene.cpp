// SPDX-License-Identifier: Apache-2.0
// HelloSky — SkyScene 実装。
#include "SkyScene.h"

#include "math/Math.h"

using namespace acs;

namespace hellosky {

void SkyScene::SetPreset(FSky& sky, SkyPreset p) noexcept {
    _preset = p;
    switch (p) {
        case SkyPreset::Day:    sky.PresetDay();    break;
        case SkyPreset::Sunset: sky.PresetSunset(); break;
        case SkyPreset::Night:  sky.PresetNight();  break;
    }
}

void SkyScene::Render(FSky&             sky,
                      FStandardShader&  shader,
                      IRhiCommandList& cl,
                      const FCamera&    camera,
                      const FGpuMesh&   plane,
                      const FGpuMesh&   sphere,
                      f32              angle) noexcept {
    // FSky を先に描く。深度書込みも深度テストも無効なので、後続のメッシュは
    // 自動で空を覆い隠す形になる (背景塗り)。
    sky.Render(cl, camera);

    // メイン光源は FSky の太陽方向 / 色と合わせる。ここがずれると
    // 「空は朝なのに地面は昼」のような不整合な絵になる。
    FDirLight lights[2];
    lights[0].direction = sky.SunDirection();
    lights[0].color     = sky.SunColor();
    // 2 灯目は環境光フィル。太陽の真逆から弱い青を当てて影側のクラッシュ
    // (真っ黒つぶれ) を防ぐ。FSky の zenith に近い寒色を採用。
    lights[1].direction = FVec3{-sky.SunDirection().x,
                                sky.SunDirection().y * 0.5f,
                               -sky.SunDirection().z};
    lights[1].color     = FVec3{0.15f, 0.18f, 0.25f};

    FVec3 ambient;
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

    // ---- 地面 (拡散主体、わずかな specular) ----
    shader.SetObject(FMat4::Translation(FVec3{0, 0, 0}),
                     FVec3{0.4f, 0.45f, 0.5f}, 0.1f, 8.0f);
    cl.SetVertexBuffer(*plane.vertex_buffer, plane.vertex_stride);
    cl.SetIndexBuffer(*plane.index_buffer);
    cl.DrawIndexed(plane.index_count);

    // ---- 球 (回転 + 強い specular で太陽が反射する見せ場) ----
    FMat4 ms = FMat4::RotationY(angle) *
              FMat4::Translation(FVec3{0, 1.5f, 0});
    shader.SetObject(ms, FVec3{1.0f, 0.85f, 0.4f}, 0.7f, 64.0f);
    cl.SetVertexBuffer(*sphere.vertex_buffer, sphere.vertex_stride);
    cl.SetIndexBuffer(*sphere.index_buffer);
    cl.DrawIndexed(sphere.index_count);
}

} // namespace hellosky
