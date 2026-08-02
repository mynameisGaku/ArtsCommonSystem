// SPDX-License-Identifier: Apache-2.0
// HelloShadows — ShadowsScene 実装。
#include "ShadowsScene.h"

#include "math/Math.h"

using namespace acs;

namespace helloshadows {

void CShadowsScene::Build() noexcept {
    m_Casters.Clear();
    // 中央に背の高い柱
    for (u32 i = 0; i < 4; ++i) {
        const f32 a = (kPi * 2.0f) * static_cast<f32>(i) / 4.0f + 0.4f;
        const f32 r = 3.0f;
        FCasterInst c;
        c.is_sphere  = false;
        c.base_color = FVec3{0.85f, 0.7f, 0.5f};
        c.model = FMat4::Scale(FVec3{0.6f, 2.0f, 0.6f}) *
                  FMat4::Translation(FVec3{Sin(a) * r, 1.0f, Cos(a) * r});
        m_Casters.PushBack(c);
    }
    // 周囲に球 (複数色)
    const FVec3 colors[4] = {
        {0.95f, 0.45f, 0.45f},
        {0.45f, 0.85f, 0.55f},
        {0.45f, 0.55f, 0.95f},
        {0.95f, 0.85f, 0.45f},
    };
    for (u32 i = 0; i < 4; ++i) {
        const f32 a = (kPi * 2.0f) * static_cast<f32>(i) / 4.0f + 0.785f;
        const f32 r = 1.5f;
        FCasterInst c;
        c.is_sphere  = true;
        c.base_color = colors[i];
        c.model = FMat4::Scale(FVec3{1.0f, 1.0f, 1.0f}) *
                  FMat4::Translation(FVec3{Sin(a) * r, 0.5f, Cos(a) * r});
        m_Casters.PushBack(c);
    }
}

void CShadowsScene::Render(CSky&             sky,
                          CStandardShader&  shader,
                          CShadowMap&       shadow,
                          IRhiCommandList& cl,
                          const CCamera&    camera,
                          const FGpuMesh&   plane,
                          const FGpuMesh&   cube,
                          const FGpuMesh&   sphere,
                          FVec3             sun_dir) noexcept {
    if (m_Casters.Size() >= static_cast<usize>(~u32{0})) return;
    const u32 caster_draws = static_cast<u32>(m_Casters.Size());
    if (!shader.BeginFrame(caster_draws + 1u) ||
        !shadow.BeginFrame(caster_draws)) return;

    sky.SetSunDirection(sun_dir);

    // === 1. シャドウパス ===
    shadow.SetDirectionalLight(sun_dir, FVec3{0, 1, 0}, /*radius=*/12.0f);
    cl.BeginShadowPass(*shadow.DepthTexture(), 1.0f);
    cl.SetPipeline(*shadow.CasterPipeline());
    cl.SetConstantBuffer(0, *shadow.LightCB());

    // 地面はシャドウキャスタにしないので、影を落とす物体だけ
    for (const FCasterInst& obj : m_Casters) {
        if (!shadow.TrySetCaster(obj.model)) continue;
        cl.SetConstantBuffer(1, *shadow.CasterObjectCB());
        const FGpuMesh& m = obj.is_sphere ? sphere : cube;
        cl.SetVertexBuffer(*m.vertex_buffer, m.vertex_stride);
        cl.SetIndexBuffer(*m.index_buffer);
        cl.DrawIndexed(m.index_count);
    }
    cl.EndShadowPass(*shadow.DepthTexture());

    // === 2. 主パス: スカイ → シーン ===
    sky.Render(cl, camera);

    FDirLight lights[1];
    lights[0].direction = sun_dir;
    lights[0].color     = sky.SunColor();
    FVec3 ambient{0.20f, 0.22f, 0.28f};
    shader.SetLights(camera.ViewProjection(), camera.Eye(),
                     lights, 1, ambient);
    // 太陽方向が毎フレーム変わるので、ライト VP も主パスへ再送して同期する。
    shader.SetShadowMap(shadow.DepthTexture(), shadow.LightViewProjection(), 0.001f);

    cl.SetPipeline(*shader.Pipeline());
    cl.SetConstantBuffer(0, *shader.PerFrameCB());
    cl.SetTexture(0, *shader.DefaultWhiteTexture());
    cl.SetTexture(1, *shader.ShadowTextureOrDefault());

    // 地面 (受影、キャストしない)
    if (!shader.SetObject(FMat4::Translation(FVec3{0, 0, 0}),
                          FVec3{0.55f, 0.55f, 0.6f}, 0.05f, 4.0f)) return;
    cl.SetConstantBuffer(1, *shader.PerObjectCB());
    cl.SetVertexBuffer(*plane.vertex_buffer, plane.vertex_stride);
    cl.SetIndexBuffer(*plane.index_buffer);
    cl.DrawIndexed(plane.index_count);

    for (const FCasterInst& obj : m_Casters) {
        if (!shader.SetObject(obj.model, obj.base_color, 0.4f, 32.0f)) continue;
        cl.SetConstantBuffer(1, *shader.PerObjectCB());
        const FGpuMesh& m = obj.is_sphere ? sphere : cube;
        cl.SetVertexBuffer(*m.vertex_buffer, m.vertex_stride);
        cl.SetIndexBuffer(*m.index_buffer);
        cl.DrawIndexed(m.index_count);
    }
}

} // namespace helloshadows
