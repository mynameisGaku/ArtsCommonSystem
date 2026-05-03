// ACS の HelloBloom サンプル
//
// 動作:
//   ・暗いシーンに非常に明るい (HDR) 球をいくつか配置
//   ・PostProcess (Bloom + ACES Tonemap) を通して LDR バックバッファに出す
//   ・1 / 2 / 3 で Bloom 強度切替、Esc 終了
//
// 学習ポイント:
//   ・OnCustomFrame() を override して HDR RT 経由のレンダリングを構築
//   ・PostProcess::Render が swapchain への合成まで担当
//
// 注: -DACS_RENDER_DILIGENT=ON 必須（Dx12 raw backend は HDR/Bloom 未対応）

#include "app/Application.h"
#include "app/EntryPoint.h"
#include "platform/Input.h"

#include "asset/MeshPrimitive.h"
#include "asset/MeshAsset.h"

#include "render/StandardShader.h"
#include "render/RenderAssets.h"
#include "render/Sky.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "render/PostProcess.h"

#include "math/Camera.h"
#include "math/Mat.h"
#include "math/Math.h"

#include "memory/UniquePtr.h"
#include "foundation/Log.h"

#include <cstdio>

using namespace acs;

class HelloBloom : public Application {
public:
    void OnStart() noexcept override {
        IRhiDevice* dev = GetRenderer().Device();
        if (!dev) { Quit(); return; }
        IRhiSwapchain* sc = GetRenderer().Swapchain();
        if (!sc) { Quit(); return; }

        const u32 sw = sc->Width();
        const u32 sh = sc->Height();

        // PostProcess (HDR RT + Bloom mip chain + Tonemap pipeline)
        if (auto r = _post.Init(*dev, sw, sh, GetRenderer().ColorFormat());
            r.IsErr()) {
            ACS_LOG_ERROR("PostProcess init failed: %s", r.Error().message);
            Quit();
            return;
        }

        // StandardShader はシーン描画用。RT format は HDR (R16G16B16A16_Float)
        if (auto r = _shader.Init(*dev, _post.HdrFormat(), GetRenderer().DepthFormat());
            r.IsErr()) { Quit(); return; }
        if (auto r = _sky.Init(*dev, _post.HdrFormat(), GetRenderer().DepthFormat());
            r.IsErr()) { Quit(); return; }
        _sky.PresetNight();

        auto sphere = Primitive::MakeSphere(0.5f, 32, 16);
        auto plane  = Primitive::MakePlane(20.0f, 20.0f);
        if (auto r = UploadMesh(*dev, *sphere, _gm_sphere); r.IsErr()) { Quit(); return; }
        if (auto r = UploadMesh(*dev, *plane,  _gm_plane);  r.IsErr()) { Quit(); return; }

        // HUD 用 (LDR バックバッファに直接描く)
        if (auto r = _batch.Init(*dev, GetRenderer().ColorFormat()); r.IsErr()) { Quit(); return; }
        const wchar_t* fp[] = {
            L"C:\\Windows\\Fonts\\meiryo.ttc",
            L"C:\\Windows\\Fonts\\msgothic.ttc",
            L"C:\\Windows\\Fonts\\arial.ttf",
        };
        for (auto p : fp) { if (_font.LoadFromFile(*dev, p, 18.0f).IsOk()) break; }

        const f32 aspect = static_cast<f32>(sw) / static_cast<f32>(sh);
        _camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 100.0f);

        ACS_LOG_INFO("HelloBloom: backend=%s, HDR=%dx%d",
                     dev->BackendName(), sw, sh);
    }

    void OnUpdate(f32 dt) noexcept override {
        if (Input::IsKeyPressed(Key::Escape)) Quit();
        if (Input::IsKeyPressed(Key::Num1)) _params.bloom_intensity = 0.0f;
        if (Input::IsKeyPressed(Key::Num2)) _params.bloom_intensity = 0.6f;
        if (Input::IsKeyPressed(Key::Num3)) _params.bloom_intensity = 1.5f;

        _angle += dt * 0.5f;
        const f32 cam_dist = 7.0f;
        _cam_yaw += (Input::IsKeyDown(Key::Right) ? 1.0f : 0.0f) * dt;
        _cam_yaw -= (Input::IsKeyDown(Key::Left)  ? 1.0f : 0.0f) * dt;
        Vec3 cam{ Sin(_cam_yaw) * cam_dist, 2.0f, -Cos(_cam_yaw) * cam_dist };
        _camera.SetLookAt(cam, Vec3{0, 1, 0});
    }

    // OnCustomFrame() に true を返して、Application のデフォルトフローを置き換える
    bool OnCustomFrame() noexcept override {
        IRhiCommandList* cl = GetRenderer().CommandList();
        IRhiSwapchain*   sc = GetRenderer().Swapchain();
        IRhiTexture*     hdr = _post.HdrRenderTarget();
        IRhiTexture*     depth = GetRenderer().DepthBuffer();
        if (!cl || !sc || !hdr) return false;   // failsafe → 既定フローに任せる

        const u32 buf_idx = sc->AcquireNextImage();
        cl->Begin();

        // 1) HDR RT にシーンを描画
        cl->BeginRenderToTexture(*hdr, ClearColor{0,0,0,1}, depth, 1.0f);

        // 全画面ビューポート
        Viewport vp{}; vp.width  = static_cast<f32>(hdr->Width());
                       vp.height = static_cast<f32>(hdr->Height());
        cl->SetViewport(vp);
        ScissorRect svr{}; svr.right  = static_cast<i32>(hdr->Width());
                          svr.bottom = static_cast<i32>(hdr->Height());
        cl->SetScissor(svr);

        // 空 → シーン本体
        _sky.Render(*cl, _camera);

        // ライト
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

        // 地面 (暗め)
        _shader.SetObject(Mat4::Translation(Vec3{0, 0, 0}),
                          Vec3{0.10f, 0.12f, 0.15f}, 0.05f, 8.0f);
        cl->SetVertexBuffer(*_gm_plane.vertex_buffer, _gm_plane.vertex_stride);
        cl->SetIndexBuffer(*_gm_plane.index_buffer);
        cl->DrawIndexed(_gm_plane.index_count);

        // 強烈に明るい HDR 球を 4 個 (色も派手に)
        const Vec3 colors[4] = {
            {6.0f, 1.0f, 0.5f},   // 赤橙 (HDR > 1.0 で Bloom が乗る)
            {0.5f, 6.0f, 1.5f},   // 緑
            {1.0f, 1.5f, 8.0f},   // 青
            {5.0f, 5.0f, 1.0f},   // 黄
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

        // 2) Bloom + Tonemap → backbuffer
        _post.Render(*cl, *sc, buf_idx, _params);

        // 3) HUD (LDR backbuffer にスプライトで描き込み)
        // backbuffer は Tonemap パスで既に RT としてバインドされてる状態 → 続けて使える
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

    void OnShutdown() noexcept override {
        if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
        _font.Shutdown();
        _batch.Shutdown();
        _gm_plane = GpuMesh{};
        _gm_sphere = GpuMesh{};
        _shader.Shutdown();
        _sky.Shutdown();
        _post.Shutdown();
    }

private:
    PostProcess     _post;
    Sky             _sky;
    StandardShader  _shader;
    SpriteBatch     _batch;
    Font            _font;
    GpuMesh         _gm_sphere, _gm_plane;
    Camera          _camera;
    PostProcessParams _params;
    f32             _cam_yaw = 0.5f;
    f32             _angle   = 0.0f;
};

ACS_DEFINE_MAIN(HelloBloom)
