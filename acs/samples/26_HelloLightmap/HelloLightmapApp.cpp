// SPDX-License-Identifier: Apache-2.0
// HelloLightmap — HelloLightmapApp の実装。
//
// シーン: Cornell box (床 / 天井 / 奥壁 / 左壁(赤) / 右壁(緑))。
// 焼き: OnStart で BakeLightmaps を呼んで各面の lightmap texture を生成。
// 描画: HDR RT → Bloom + ACES tonemap → LDR backbuffer + HUD。
#include "HelloLightmapApp.h"
#include "LightmapBaker.h"

#include "app/Sample.h"
#include "platform/Input.h"
#include "asset/MeshPrimitive.h"
#include "asset/MeshAsset.h"
#include "render/RenderAssets.h"
#include "math/Mat.h"
#include "math/Math.h"
#include "foundation/Log.h"

#include <cstdio>

using namespace acs;

namespace hellolightmap {

void HelloLightmapApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }
    IRhiSwapchain* sc = GetRenderer().Swapchain();
    if (!sc) { Quit(); return; }

    // HDR PostProcess (Bloom + ACES tonemap)。シーンは HDR RT に描く。
    ACS_SAMPLE_INIT(_post.Init(*dev, sc->Width(), sc->Height(),
                               GetRenderer().ColorFormat()));
    // PbrShader は HDR RT フォーマットに合わせて init する。
    ACS_SAMPLE_INIT(_pbr.Init(*dev, _post.HdrFormat(),
                              GetRenderer().DepthFormat()));

    BuildCornellBox(*dev);
    BakeLightmaps(*dev, _quads);

    // SpriteBatch は tonemap 後の LDR backbuffer に描く。
    ACS_SAMPLE_INIT(_batch.Init(*dev, GetRenderer().ColorFormat()));
    (void)Sample::TryLoadDefaultUIFont(_font, *dev, 18.0f, 1024, true);

    const f32 aspect = static_cast<f32>(sc->Width()) /
                       static_cast<f32>(sc->Height());
    _camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.05f, 100.0f);
    _cam_pos = Vec3{0.0f, 1.0f, -0.9f};

    // PostProcess パラメータ (絵作りで調整可)。
    _post_params.bloom_threshold    = 2.5f;   // 天井 (光源) だけが bloom する閾値
    _post_params.bloom_intensity    = 0.5f;
    _post_params.grain_intensity    = 0.0f;   // GI デモなので film grain は切る
    _post_params.vignette_intensity = 0.15f;
    _post_params.ssr_intensity      = 0.0f;   // SSR 未使用 (fallback mip の誤加算防止)
}

void HelloLightmapApp::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) Quit();
    if (Input::IsKeyPressed(EKey::L)) _show_lightmap = !_show_lightmap;

    const f32 mv = 2.0f * dt, tr = 1.4f * dt;
    if (Input::IsKeyDown(EKey::Left))  _cam_yaw -= tr;
    if (Input::IsKeyDown(EKey::Right)) _cam_yaw += tr;
    if (Input::IsKeyDown(EKey::Up))    _cam_pitch -= tr * 0.8f;
    if (Input::IsKeyDown(EKey::Down))  _cam_pitch += tr * 0.8f;
    const f32 limit = 0.45f * kPi;
    if (_cam_pitch >  limit) _cam_pitch =  limit;
    if (_cam_pitch < -limit) _cam_pitch = -limit;
    Vec3 forward{ Sin(_cam_yaw) * Cos(_cam_pitch),
                 -Sin(_cam_pitch),
                  Cos(_cam_yaw) * Cos(_cam_pitch) };
    Vec3 right{ Cos(_cam_yaw), 0, -Sin(_cam_yaw) };
    if (Input::IsKeyDown(EKey::W)) _cam_pos += forward * mv;
    if (Input::IsKeyDown(EKey::S)) _cam_pos -= forward * mv;
    if (Input::IsKeyDown(EKey::D)) _cam_pos += right   * mv;
    if (Input::IsKeyDown(EKey::A)) _cam_pos -= right   * mv;
    _camera.SetLookAt(_cam_pos, _cam_pos + forward);
}

// OnCustomFrame: HDR RT にシーンを描き、PostProcess (Bloom + ACES tonemap)
// で LDR backbuffer へ。HDR lightmap の高輝度が tonemap で自然にロールオフする。
bool HelloLightmapApp::OnCustomFrame() noexcept {
    IRhiDevice*      dev   = GetRenderer().Device();
    IRhiCommandList* cl    = GetRenderer().CommandList();
    IRhiSwapchain*   sc    = GetRenderer().Swapchain();
    IRhiTexture*     hdr   = _post.HdrRenderTarget();
    IRhiTexture*     depth = GetRenderer().DepthBuffer();
    if (!dev || !cl || !sc || !hdr) return false;

    const u32 buf_idx = sc->AcquireNextImage();
    cl->Begin();

    // ===== HDR RT に Cornell box を描画 =====
    cl->BeginRenderToTexture(*hdr, ClearColor{0, 0, 0, 1}, depth, 1.0f);
    Viewport vp{}; vp.width  = static_cast<f32>(hdr->Width());
                   vp.height = static_cast<f32>(hdr->Height());
    cl->SetViewport(vp);
    ScissorRect svr{}; svr.right  = static_cast<i32>(hdr->Width());
                       svr.bottom = static_cast<i32>(hdr->Height());
    cl->SetScissor(svr);

    // 動的ライトは使わず ごく弱い ambient のみ。間接光は lightmap が担う。
    _pbr.SetLights(_camera.ViewProjection(), _camera.Eye(),
                   nullptr, 0, Vec3{0.02f, 0.02f, 0.02f});
    _pbr.SetPointLights(nullptr, 0);

    cl->SetPipeline(*_pbr.Pipeline());
    cl->SetConstantBuffer(0, *_pbr.PerFrameCB());
    cl->SetConstantBuffer(1, *_pbr.PerObjectCB());
    cl->SetTexture(0, *_pbr.DefaultWhiteTexture());

    for (u32 i = 0; i < kQuadCount; ++i) {
        Quad& q = _quads[i];
        // lightmap を slot 8 に bind (L キーで OFF にすると flat ambient のみ)
        if (_show_lightmap && q.lightmap) {
            _pbr.SetLightmap(q.lightmap.Get(), 1.0f);
        } else {
            _pbr.SetLightmap(nullptr, 0.0f);
        }
        _pbr.SetObject(q.model, q.albedo, /*metallic=*/0.0f,
                       /*roughness=*/0.9f, /*ao=*/1.0f);
        _pbr.BindIblTextures(*cl);
        cl->SetVertexBuffer(*q.mesh.vertex_buffer, q.mesh.vertex_stride);
        cl->SetIndexBuffer(*q.mesh.index_buffer);
        cl->DrawIndexed(q.mesh.index_count);
    }
    cl->EndRenderToTexture(*hdr);

    // ===== Bloom + ACES tonemap → LDR backbuffer =====
    _post.Render(*cl, *sc, buf_idx, _post_params);

    // ===== HUD (LDR backbuffer) =====
    if (_font.AtlasTexture()) {
        const u32 sw = sc->Width();
        const u32 sh = sc->Height();
        _batch.Begin(*cl, sw, sh);
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Cornell box - path-traced HDR lightmap (%u rays x %u bounces)  FPS: %.1f",
                      kBakeRays, kBounceDepth, static_cast<double>(FPS()));
        _batch.DrawString(_font, buf, 20, 20, Vec4{1, 1, 1, 1});
        std::snprintf(buf, sizeof(buf), "Lightmap: %s   (L で切替)",
                      _show_lightmap ? "ON" : "OFF");
        _batch.DrawString(_font, buf, 20, 44, Vec4{1.0f, 0.95f, 0.7f, 1});
        _batch.DrawString(_font, "WASD: 移動   矢印: 視点   Esc: 終了",
                          20, 68, Vec4{0.7f, 0.85f, 1.0f, 1});
        _batch.End();
    }

    cl->EndRenderToSwapchain(*sc, buf_idx);
    cl->End();
    cl->Submit();
    sc->Present();
    return true;
}

void HelloLightmapApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    _font.Shutdown();
    _batch.Shutdown();
    for (u32 i = 0; i < kQuadCount; ++i) {
        _quads[i].lightmap.Reset();
        _quads[i].mesh = GpuMesh{};
    }
    _pbr.Shutdown();
    _post.Shutdown();
}

void HelloLightmapApp::InitQuad(IRhiDevice& dev, Quad& q, f32 w, f32 h,
                                const Mat4& model, Vec3 albedo,
                                i32 axis, f32 axis_value,
                                f32 u_min, f32 u_max,
                                f32 v_min, f32 v_max,
                                bool emissive) noexcept {
    auto plane = Primitive::MakePlane(w, h);
    (void)UploadMesh(dev, *plane, q.mesh);
    q.model      = model;
    q.albedo     = albedo;
    q.plane_w    = w;
    q.plane_h    = h;
    q.normal     = Normalize(TransformVector(Vec3{0, 1, 0}, model));
    q.axis       = axis;
    q.axis_value = axis_value;
    q.u_min = u_min; q.u_max = u_max;
    q.v_min = v_min; q.v_max = v_max;
    q.emissive   = emissive;
}

void HelloLightmapApp::BuildCornellBox(IRhiDevice& dev) noexcept {
    const Vec3 white{0.72f, 0.72f, 0.72f};
    const Vec3 red  {0.65f, 0.10f, 0.10f};
    const Vec3 green{0.10f, 0.55f, 0.12f};

    // model は Rotation * Translation の順 (ACS の row-major では「先に
    // 回転、次に平行移動」= ローカルで回転してから world 位置へ移動)。
    // MakePlane(w,h) は XZ 平面: ローカル u→+X, v→-Z。回転後の world 軸への
    // 写像を考慮して w/h を割り当てる (例: 壁は回転で v→Z, u→Y になる)。

    // 床: y=0、法線 +Y。回転なし。u→X(幅2), v→Z(奥行3)。
    InitQuad(dev, _quads[0], 2.0f, 3.0f,
             Mat4::Translation(Vec3{0.0f, kBoxMinY, 0.5f}),
             white, /*axis(y)=*/1, kBoxMinY,
             kBoxMinX, kBoxMaxX, kBoxMinZ, kBoxMaxZ, false);
    // 天井: y=2、法線 -Y。X 軸 π 回転で裏返す。emissive = 光源。u→X, v→Z。
    InitQuad(dev, _quads[1], 2.0f, 3.0f,
             Mat4::RotationX(kPi) * Mat4::Translation(Vec3{0.0f, kBoxMaxY, 0.5f}),
             white, /*axis(y)=*/1, kBoxMaxY,
             kBoxMinX, kBoxMaxX, kBoxMinZ, kBoxMaxZ, true);
    // 奥壁: z=2、法線 -Z。X 軸 -π/2 回転。u→X(幅2), v→Y(高さ2)。
    InitQuad(dev, _quads[2], 2.0f, 2.0f,
             Mat4::RotationX(-kPi * 0.5f) * Mat4::Translation(Vec3{0.0f, 1.0f, kBoxMaxZ}),
             white, /*axis(z)=*/2, kBoxMaxZ,
             kBoxMinX, kBoxMaxX, kBoxMinY, kBoxMaxY, false);
    // 左壁: x=-1、法線 +X、赤。Z 軸 -π/2 回転。u→Y(高さ2), v→Z(奥行3)。
    InitQuad(dev, _quads[3], 2.0f, 3.0f,
             Mat4::RotationZ(-kPi * 0.5f) * Mat4::Translation(Vec3{kBoxMinX, 1.0f, 0.5f}),
             red, /*axis(x)=*/0, kBoxMinX,
             kBoxMinY, kBoxMaxY, kBoxMinZ, kBoxMaxZ, false);
    // 右壁: x=1、法線 -X、緑。Z 軸 +π/2 回転。u→Y(高さ2), v→Z(奥行3)。
    InitQuad(dev, _quads[4], 2.0f, 3.0f,
             Mat4::RotationZ(kPi * 0.5f) * Mat4::Translation(Vec3{kBoxMaxX, 1.0f, 0.5f}),
             green, /*axis(x)=*/0, kBoxMaxX,
             kBoxMinY, kBoxMaxY, kBoxMinZ, kBoxMaxZ, false);
}

} // namespace hellolightmap
