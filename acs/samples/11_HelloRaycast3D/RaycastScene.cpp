// SPDX-License-Identifier: Apache-2.0
#include "RaycastScene.h"

#include "HudRenderer.h"

#include "asset/MeshPrimitive.h"
#include "asset/MeshAsset.h"
#include "math/Mat.h"

using namespace acs;

namespace helloraycast3d {

bool RaycastScene::Init(IRhiDevice& dev, f32 aspect) noexcept {
    // 球は半径 1 で作っておき、描画時に radius_or_half で Scale する。
    // 立方体も同様に full-size 1.0 (= 半サイズ 0.5) で生成 → Scale で実寸を出す。
    auto sphere = Primitive::MakeSphere(1.0f, 32, 16);
    auto cube   = Primitive::MakeCube(1.0f);
    auto plane  = Primitive::MakePlane(40.0f, 40.0f);
    if (!sphere || !cube || !plane) return false;
    if (UploadMesh(dev, *sphere, _gm_sphere).IsErr()) return false;
    if (UploadMesh(dev, *cube,   _gm_cube).IsErr())   return false;
    if (UploadMesh(dev, *plane,  _gm_plane).IsErr())  return false;

    _targets.Init();
    _caster.Init(aspect);
    return true;
}

void RaycastScene::Shutdown() noexcept {
    _gm_plane  = GpuMesh{};
    _gm_cube   = GpuMesh{};
    _gm_sphere = GpuMesh{};
}

void RaycastScene::Update(f32 dt) noexcept {
    _caster.Update(dt, _targets);
}

void RaycastScene::Render(StandardShader& shader,
                          IRhiCommandList& cl,
                          SpriteBatch& batch,
                          Font& font,
                          u32 screen_w,
                          u32 screen_h) noexcept {
    // ---- 2 灯ライティング (暖色キー + 寒色フィル) ----
    // フィルの色を寒色にして、影側でも色が完全に潰れないように演出する。
    FDirLight lights[2];
    lights[0].direction = FVec3{ 0.5f, 0.8f, 0.3f};
    lights[0].color     = FVec3{ 1.0f, 0.9f, 0.7f};
    lights[1].direction = FVec3{-0.4f, 0.5f,-0.7f};
    lights[1].color     = FVec3{ 0.3f, 0.4f, 0.6f};
    const FCamera& cam = _caster.Camera();
    shader.SetLights(cam.ViewProjection(), cam.Eye(), lights, 2, kAmbient);

    cl.SetPipeline(*shader.Pipeline());
    cl.SetConstantBuffer(0, *shader.PerFrameCB());
    cl.SetConstantBuffer(1, *shader.PerObjectCB());
    cl.SetTexture(0, *shader.DefaultWhiteTexture());
    cl.SetTexture(1, *shader.ShadowTextureOrDefault());

    _render_targets(shader, cl);

    HudRenderer::Draw(batch, font, cl, screen_w, screen_h, _targets);
}

void RaycastScene::_render_targets(StandardShader& shader,
                                   IRhiCommandList& cl) noexcept {
    // 地面 (世界原点に置く)
    shader.SetObject(FMat4::Translation(FVec3{0, 0, 0}),
                     kPlaneColor, 0.0f, 1.0f);
    cl.SetVertexBuffer(*_gm_plane.vertex_buffer, _gm_plane.vertex_stride);
    cl.SetIndexBuffer(*_gm_plane.index_buffer);
    cl.DrawIndexed(_gm_plane.index_count);

    // 各オブジェクト: ヒット中は色を白 + spec/shine を強めてハイライト
    const u32 n = _targets.Count();
    for (u32 i = 0; i < n; ++i) {
        const Object& o = _targets.At(i);
        const bool    selected = (static_cast<i32>(i) == _targets.HitIndex());
        const FVec3    col   = selected ? kSelectedColor : o.base_color;
        const f32     spec  = selected ? 0.9f  : 0.3f;
        const f32     shine = selected ? 64.0f : 16.0f;

        // Scale を先に → Translation を後に (= 球/立方体のメッシュ標準サイズに
        // radius_or_half をかけてからワールド位置に運ぶ)
        const FMat4 m = FMat4::Scale(FVec3{o.radius_or_half, o.radius_or_half, o.radius_or_half}) *
                       FMat4::Translation(o.position);
        shader.SetObject(m, col, spec, shine);

        const GpuMesh& mesh = (o.kind == ShapeKind::FSphere) ? _gm_sphere : _gm_cube;
        cl.SetVertexBuffer(*mesh.vertex_buffer, mesh.vertex_stride);
        cl.SetIndexBuffer(*mesh.index_buffer);
        cl.DrawIndexed(mesh.index_count);
    }
}

} // namespace helloraycast3d
