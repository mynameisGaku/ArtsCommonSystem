// SPDX-License-Identifier: Apache-2.0
// HelloLights — LightsScene 実装。
#include "LightsScene.h"

#include "math/Math.h"

using namespace acs;

namespace hellolights {

void LightsScene::Build() noexcept {
    _objects.Clear();
    for (u32 i = 0; i < kCubeCount; ++i) {
        // 開始位相 0.4f はカメラ初期位置と物体が重ならないようにずらすため。
        const f32 a = (kPi * 2.0f) * static_cast<f32>(i) / kCubeCount + 0.4f;
        const f32 r = 2.5f;
        ObjectInst o;
        o.is_sphere     = (i & 1) != 0;
        const f32 h     = 0.5f;
        const FVec3 pos  { Sin(a) * r, h, Cos(a) * r };
        o.model = FMat4::Translation(pos);
        // 物体側は白に近い灰色に統一: 点光源の色がそのまま乗って見えるので
        // ライトのデモとして読みやすい。
        o.color = FVec3{0.85f, 0.85f, 0.85f};
        _objects.PushBack(o);
    }
}

void LightsScene::Render(StandardShader&  shader,
                         IRhiCommandList& cl,
                         const FCamera&    camera,
                         const GpuMesh&   plane,
                         const GpuMesh&   cube,
                         const GpuMesh&   sphere,
                         f32              time) noexcept {
    // --- ライト設定 ---
    // dir ライトはほぼ無効化 (ambient と同程度) に落とし、点光源の色を
    // 主役にする。完全に 0 にしないのは陰側が真っ黒になって読みにくいため。
    FDirLight dir;
    dir.direction = FVec3{0.3f, 1.0f, 0.4f};
    dir.color     = FVec3{0.05f, 0.05f, 0.07f};
    shader.SetLights(camera.ViewProjection(), camera.Eye(),
                     &dir, 1, FVec3{0.04f, 0.04f, 0.06f});

    PointLight pts[kPointCount];
    const FVec3 colors[kPointCount] = {
        {1.0f, 0.3f, 0.3f},
        {0.3f, 1.0f, 0.4f},
        {0.3f, 0.4f, 1.0f},
        {1.0f, 0.9f, 0.3f},
    };
    for (u32 i = 0; i < kPointCount; ++i) {
        const f32 a = (kPi * 2.0f) * static_cast<f32>(i) / kPointCount + time * 0.4f;
        const f32 r = 4.5f;
        pts[i].position = FVec3{ Sin(a) * r,
                                // 上下に揺らして床面に色を流すと、減衰が
                                // 視覚的にわかりやすくなる。
                                1.5f + Sin(time * 1.5f + static_cast<f32>(i)) * 0.6f,
                                Cos(a) * r };
        // 暗いシーンで色がしっかり出るよう ×4 倍。range と組み合わせて
        // 床上の色の落ち方が綺麗に見える値。
        pts[i].color    = colors[i] * 4.0f;
        pts[i].range    = 7.0f;
    }
    shader.SetPointLights(pts, kPointCount);

    // --- 描画 ---
    cl.SetPipeline(*shader.Pipeline());
    cl.SetConstantBuffer(0, *shader.PerFrameCB());
    cl.SetConstantBuffer(1, *shader.PerObjectCB());
    cl.SetTexture(0, *shader.DefaultWhiteTexture());
    cl.SetTexture(1, *shader.ShadowTextureOrDefault());

    // 床: 暗めの灰色 + roughness 0.2 で点光源のハイライトが乗りやすい。
    shader.SetObject(FMat4::Translation(FVec3{0, 0, 0}),
                     FVec3{0.55f, 0.55f, 0.6f}, 0.2f, 32.0f);
    cl.SetVertexBuffer(*plane.vertex_buffer, plane.vertex_stride);
    cl.SetIndexBuffer(*plane.index_buffer);
    cl.DrawIndexed(plane.index_count);

    for (const ObjectInst& obj : _objects) {
        shader.SetObject(obj.model, obj.color, 0.5f, 32.0f);
        const GpuMesh& m = obj.is_sphere ? sphere : cube;
        cl.SetVertexBuffer(*m.vertex_buffer, m.vertex_stride);
        cl.SetIndexBuffer(*m.index_buffer);
        cl.DrawIndexed(m.index_count);
    }

    // 点光源を「自己発光風」に可視化。実 emissive ではなく、点光源色を
    // base_color に流し込み・roughness=0 にして強反射させる近似。点光源の
    // 居場所を把握するのが目的の演出なので、安価なこの方法で十分。
    for (u32 i = 0; i < kPointCount; ++i) {
        const FVec3 col = pts[i].color * 0.3f;
        const FMat4 m = FMat4::Scale(FVec3{0.2f, 0.2f, 0.2f}) *
                        FMat4::Translation(pts[i].position);
        shader.SetObject(m, col, 0.0f, 1.0f);
        cl.SetVertexBuffer(*sphere.vertex_buffer, sphere.vertex_stride);
        cl.SetIndexBuffer(*sphere.index_buffer);
        cl.DrawIndexed(sphere.index_count);
    }
}

} // namespace hellolights
