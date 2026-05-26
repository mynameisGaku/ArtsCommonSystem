// SPDX-License-Identifier: Apache-2.0
// HelloLights — LightsScene 実装。
#include "LightsScene.h"

#include "math/Math.h"

using namespace acs;

namespace hellolights {

void LightsScene::Build() noexcept {
    _objects.Clear();
    // 6 個の物体を散らす (cube と sphere を交互に)
    for (u32 i = 0; i < kCubeCount; ++i) {
        const f32 a = (kPi * 2.0f) * static_cast<f32>(i) / kCubeCount + 0.4f;
        const f32 r = 2.5f;
        ObjectInst o;
        o.is_sphere     = (i & 1) != 0;
        const f32 h     = 0.5f;
        const Vec3 pos  { Sin(a) * r, h, Cos(a) * r };
        o.model = Mat4::Translation(pos);
        o.color = Vec3{0.85f, 0.85f, 0.85f};   // ライトの色を反映するので白が映える
        _objects.PushBack(o);
    }
}

void LightsScene::Render(StandardShader&  shader,
                         IRhiCommandList& cl,
                         const Camera&    camera,
                         const GpuMesh&   plane,
                         const GpuMesh&   cube,
                         const GpuMesh&   sphere,
                         f32              time) noexcept {
    // === ライト設定 ===
    // 弱い dir ライトで全体を最低限照らす
    DirLight dir;
    dir.direction = Vec3{0.3f, 1.0f, 0.4f};
    dir.color     = Vec3{0.05f, 0.05f, 0.07f};
    shader.SetLights(camera.ViewProjection(), camera.Eye(),
                     &dir, 1, Vec3{0.04f, 0.04f, 0.06f});

    // 4 灯の点光源を円周状にゆっくり回す
    PointLight pts[kPointCount];
    const Vec3 colors[kPointCount] = {
        {1.0f, 0.3f, 0.3f},   // 赤
        {0.3f, 1.0f, 0.4f},   // 緑
        {0.3f, 0.4f, 1.0f},   // 青
        {1.0f, 0.9f, 0.3f},   // 黄
    };
    for (u32 i = 0; i < kPointCount; ++i) {
        const f32 a = (kPi * 2.0f) * static_cast<f32>(i) / kPointCount + time * 0.4f;
        const f32 r = 4.5f;
        pts[i].position = Vec3{ Sin(a) * r,
                                1.5f + Sin(time * 1.5f + static_cast<f32>(i)) * 0.6f,
                                Cos(a) * r };
        pts[i].color    = colors[i] * 4.0f;        // 暗い部屋なので強めに
        pts[i].range    = 7.0f;
    }
    shader.SetPointLights(pts, kPointCount);

    // === 描画 ===
    cl.SetPipeline(*shader.Pipeline());
    cl.SetConstantBuffer(0, *shader.PerFrameCB());
    cl.SetConstantBuffer(1, *shader.PerObjectCB());
    cl.SetTexture(0, *shader.DefaultWhiteTexture());
    cl.SetTexture(1, *shader.ShadowTextureOrDefault());

    // 地面
    shader.SetObject(Mat4::Translation(Vec3{0, 0, 0}),
                     Vec3{0.55f, 0.55f, 0.6f}, 0.2f, 32.0f);
    cl.SetVertexBuffer(*plane.vertex_buffer, plane.vertex_stride);
    cl.SetIndexBuffer(*plane.index_buffer);
    cl.DrawIndexed(plane.index_count);

    // オブジェクト
    for (const ObjectInst& obj : _objects) {
        shader.SetObject(obj.model, obj.color, 0.5f, 32.0f);
        const GpuMesh& m = obj.is_sphere ? sphere : cube;
        cl.SetVertexBuffer(*m.vertex_buffer, m.vertex_stride);
        cl.SetIndexBuffer(*m.index_buffer);
        cl.DrawIndexed(m.index_count);
    }

    // 光源を可視化 (小さな球を点光源位置に置いて自己発光風)
    // ベース色を非常に高く + 環境光に依存する形で「光ってる」風にする
    for (u32 i = 0; i < kPointCount; ++i) {
        const Vec3 col = pts[i].color * 0.3f;
        const Mat4 m = Mat4::Scale(Vec3{0.2f, 0.2f, 0.2f}) *
                        Mat4::Translation(pts[i].position);
        // 自己発光風: ambient を強く感じる色 (材質側で base_color を高く)
        shader.SetObject(m, col, 0.0f, 1.0f);
        cl.SetVertexBuffer(*sphere.vertex_buffer, sphere.vertex_stride);
        cl.SetIndexBuffer(*sphere.index_buffer);
        cl.DrawIndexed(sphere.index_count);
    }
}

} // namespace hellolights
