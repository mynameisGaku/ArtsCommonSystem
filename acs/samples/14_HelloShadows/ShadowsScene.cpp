// SPDX-License-Identifier: Apache-2.0
// HelloShadows — ShadowsScene 実装。
#include "ShadowsScene.h"

#include "math/Math.h"

using namespace acs;

namespace helloshadows {

void ShadowsScene::Build() noexcept {
    _casters.Clear();
    // 中央に背の高い柱
    for (u32 i = 0; i < 4; ++i) {
        const f32 a = (kPi * 2.0f) * static_cast<f32>(i) / 4.0f + 0.4f;
        const f32 r = 3.0f;
        CasterInst c;
        c.is_sphere  = false;
        c.base_color = Vec3{0.85f, 0.7f, 0.5f};
        c.model = Mat4::Scale(Vec3{0.6f, 2.0f, 0.6f}) *
                  Mat4::Translation(Vec3{Sin(a) * r, 1.0f, Cos(a) * r});
        _casters.PushBack(c);
    }
    // 周囲に球 (複数色)
    const Vec3 colors[4] = {
        {0.95f, 0.45f, 0.45f},
        {0.45f, 0.85f, 0.55f},
        {0.45f, 0.55f, 0.95f},
        {0.95f, 0.85f, 0.45f},
    };
    for (u32 i = 0; i < 4; ++i) {
        const f32 a = (kPi * 2.0f) * static_cast<f32>(i) / 4.0f + 0.785f;
        const f32 r = 1.5f;
        CasterInst c;
        c.is_sphere  = true;
        c.base_color = colors[i];
        c.model = Mat4::Scale(Vec3{1.0f, 1.0f, 1.0f}) *
                  Mat4::Translation(Vec3{Sin(a) * r, 0.5f, Cos(a) * r});
        _casters.PushBack(c);
    }
}

void ShadowsScene::Render(Sky&             sky,
                          StandardShader&  shader,
                          ShadowMap&       shadow,
                          IRhiCommandList& cl,
                          const Camera&    camera,
                          const GpuMesh&   plane,
                          const GpuMesh&   cube,
                          const GpuMesh&   sphere,
                          Vec3             sun_dir) noexcept {
    sky.SetSunDirection(sun_dir);

    // === 1. シャドウパス ===
    shadow.SetDirectionalLight(sun_dir, Vec3{0, 1, 0}, /*radius=*/12.0f);
    cl.BeginShadowPass(*shadow.DepthTexture(), 1.0f);
    cl.SetPipeline(*shadow.CasterPipeline());
    cl.SetConstantBuffer(0, *shadow.LightCB());
    cl.SetConstantBuffer(1, *shadow.CasterObjectCB());

    // 地面はシャドウキャスタにしないので、影を落とす物体だけ
    for (const CasterInst& obj : _casters) {
        shadow.SetCaster(obj.model);
        const GpuMesh& m = obj.is_sphere ? sphere : cube;
        cl.SetVertexBuffer(*m.vertex_buffer, m.vertex_stride);
        cl.SetIndexBuffer(*m.index_buffer);
        cl.DrawIndexed(m.index_count);
    }
    cl.EndShadowPass(*shadow.DepthTexture());

    // === 2. 主パス: スカイ → シーン ===
    sky.Render(cl, camera);

    DirLight lights[1];
    lights[0].direction = sun_dir;
    lights[0].color     = sky.SunColor();
    Vec3 ambient{0.20f, 0.22f, 0.28f};
    shader.SetLights(camera.ViewProjection(), camera.Eye(),
                     lights, 1, ambient);
    // 太陽方向が毎フレーム変わるので、ライト VP も主パスへ再送して同期する。
    shader.SetShadowMap(shadow.DepthTexture(), shadow.LightViewProjection(), 0.001f);

    cl.SetPipeline(*shader.Pipeline());
    cl.SetConstantBuffer(0, *shader.PerFrameCB());
    cl.SetConstantBuffer(1, *shader.PerObjectCB());
    cl.SetTexture(0, *shader.DefaultWhiteTexture());
    cl.SetTexture(1, *shader.ShadowTextureOrDefault());

    // 地面 (受影、キャストしない)
    shader.SetObject(Mat4::Translation(Vec3{0, 0, 0}),
                     Vec3{0.55f, 0.55f, 0.6f}, 0.05f, 4.0f);
    cl.SetVertexBuffer(*plane.vertex_buffer, plane.vertex_stride);
    cl.SetIndexBuffer(*plane.index_buffer);
    cl.DrawIndexed(plane.index_count);

    for (const CasterInst& obj : _casters) {
        shader.SetObject(obj.model, obj.base_color, 0.4f, 32.0f);
        const GpuMesh& m = obj.is_sphere ? sphere : cube;
        cl.SetVertexBuffer(*m.vertex_buffer, m.vertex_stride);
        cl.SetIndexBuffer(*m.index_buffer);
        cl.DrawIndexed(m.index_count);
    }
}

} // namespace helloshadows
