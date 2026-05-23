// SPDX-License-Identifier: Apache-2.0
// ACS サンプル 18 — HelloTextured（低レベル RHI トラック）
//
// ※ HLSL を直接書く発展編。先に 01〜15 の高レベル API サンプルから進めてください。
//
// 動作:
//   ・3D 空間に「テクスチャ付きキューブ」を描画
//   ・毎フレーム回転
//   ・テクスチャは手続き生成（チェッカーボード + 模様）
//   ・矢印キーでカメラ回転、Esc で終了
//
// 学習ポイント:
//   ・IRhiTexture 作成 → SetTexture でバインド
//   ・PipelineDesc::static_samplers で固定サンプラを指定
//   ・PixelShader からテクスチャ・サンプラを読む HLSL 構文
//   ・複数の constant buffer（MVP + 時間）

#include "app/Application.h"
#include "app/EntryPoint.h"
#include "platform/Input.h"
#include "render/IRhiBuffer.h"
#include "render/IRhiShader.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiTexture.h"
#include "render/IRhiSampler.h"
#include "math/Camera.h"
#include "math/Mat.h"
#include "memory/UniquePtr.h"
#include "foundation/Log.h"

using namespace acs;

namespace {

// 1 頂点 = 位置 + UV
struct Vertex {
    f32 pos[3];
    f32 uv[2];
};

// キューブ 24 頂点 (各面 4 頂点、UV は (0,0)..(1,1))
constexpr Vertex kCubeVertices[24] = {
    // -Z 面
    {{-1, -1, -1}, {0, 1}}, {{ 1, -1, -1}, {1, 1}},
    {{ 1,  1, -1}, {1, 0}}, {{-1,  1, -1}, {0, 0}},
    // +Z 面
    {{ 1, -1,  1}, {0, 1}}, {{-1, -1,  1}, {1, 1}},
    {{-1,  1,  1}, {1, 0}}, {{ 1,  1,  1}, {0, 0}},
    // -X 面
    {{-1, -1,  1}, {0, 1}}, {{-1, -1, -1}, {1, 1}},
    {{-1,  1, -1}, {1, 0}}, {{-1,  1,  1}, {0, 0}},
    // +X 面
    {{ 1, -1, -1}, {0, 1}}, {{ 1, -1,  1}, {1, 1}},
    {{ 1,  1,  1}, {1, 0}}, {{ 1,  1, -1}, {0, 0}},
    // +Y 面
    {{-1,  1, -1}, {0, 1}}, {{ 1,  1, -1}, {1, 1}},
    {{ 1,  1,  1}, {1, 0}}, {{-1,  1,  1}, {0, 0}},
    // -Y 面
    {{-1, -1,  1}, {0, 1}}, {{ 1, -1,  1}, {1, 1}},
    {{ 1, -1, -1}, {1, 0}}, {{-1, -1, -1}, {0, 0}},
};
constexpr u16 kCubeIndices[36] = {
    0,  1,  2,    0,  2,  3,
    4,  5,  6,    4,  6,  7,
    8,  9, 10,    8, 10, 11,
   12, 13, 14,   12, 14, 15,
   16, 17, 18,   16, 18, 19,
   20, 21, 22,   20, 22, 23,
};

// HLSL: テクスチャサンプリング
const char* kHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer Frame : register(b0) { float4x4 mvp; };

Texture2D    tex0 : register(t0);
SamplerState smp0 : register(s0);

struct VSIn  { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(VSIn v) {
    VSOut o;
    o.pos = mul(float4(v.pos, 1.0), mvp);
    o.uv  = v.uv;
    return o;
}

float4 PSMain(VSOut v) : SV_TARGET {
    return tex0.Sample(smp0, v.uv);
}
)";

// 手続き生成: 64x64 のチェッカーボード + 中央に円
constexpr u32 kTexSize = 64;

void GenerateTexture(u8* dst /* 64*64*4 bytes */) noexcept {
    for (u32 y = 0; y < kTexSize; ++y) {
        for (u32 x = 0; x < kTexSize; ++x) {
            const bool checker = ((x / 8) + (y / 8)) & 1;
            // 円判定
            const f32 cx = static_cast<f32>(x) - kTexSize * 0.5f;
            const f32 cy = static_cast<f32>(y) - kTexSize * 0.5f;
            const f32 r2 = cx * cx + cy * cy;
            const bool circle = r2 < (kTexSize * 0.25f) * (kTexSize * 0.25f);

            u8 R, G, B;
            if (circle) {            R = 230; G = 200; B = 20;  }   // 中央: 山吹色の月
            else if (checker)      { R = 60;  G = 100; B = 220; }   // 青
            else                   { R = 230; G = 230; B = 230; }   // 白

            const usize idx = static_cast<usize>(y * kTexSize + x) * 4;
            dst[idx + 0] = R;
            dst[idx + 1] = G;
            dst[idx + 2] = B;
            dst[idx + 3] = 255;
        }
    }
}

} // namespace

class HelloTextured : public Application {
public:
    void OnStart() noexcept override {
        IRhiDevice* dev = GetRenderer().Device();
        if (!dev) { Quit(); return; }

        // === シェーダ ===
        ShaderDesc vs_desc{};
        vs_desc.stage = EShaderStage::Vertex;
        vs_desc.hlsl_source = kHLSL;
        vs_desc.entry_point = "VSMain";
        vs_desc.debug_name  = "Tex.VS";
        if (auto r = CreateRhiShader(*dev, vs_desc); r.IsErr()) {
            ACS_LOG_ERROR("VS: %s", r.Error().message); Quit(); return;
        } else _vs = Move(r.Value());

        ShaderDesc ps_desc{};
        ps_desc.stage = EShaderStage::Pixel;
        ps_desc.hlsl_source = kHLSL;
        ps_desc.entry_point = "PSMain";
        ps_desc.debug_name  = "Tex.PS";
        if (auto r = CreateRhiShader(*dev, ps_desc); r.IsErr()) {
            ACS_LOG_ERROR("PS: %s", r.Error().message); Quit(); return;
        } else _ps = Move(r.Value());

        // === バッファ ===
        BufferDesc vb{};
        vb.size = sizeof(kCubeVertices);
        vb.usage = EBufferUsage::Vertex; vb.cpu_writable = true;
        vb.initial_data = kCubeVertices;
        if (auto r = CreateRhiBuffer(*dev, vb); r.IsErr()) {
            ACS_LOG_ERROR("頂点バッファ作成に失敗: %s", r.Error().message); Quit(); return;
        }
        else _vb = Move(r.Value());

        BufferDesc ib{};
        ib.size = sizeof(kCubeIndices);
        ib.usage = EBufferUsage::Index16; ib.cpu_writable = true;
        ib.initial_data = kCubeIndices;
        if (auto r = CreateRhiBuffer(*dev, ib); r.IsErr()) {
            ACS_LOG_ERROR("インデックスバッファ作成に失敗: %s", r.Error().message); Quit(); return;
        }
        else _ib = Move(r.Value());

        BufferDesc cb_d{};
        cb_d.size = 256; cb_d.usage = EBufferUsage::Uniform; cb_d.cpu_writable = true;
        if (auto r = CreateRhiBuffer(*dev, cb_d); r.IsErr()) {
            ACS_LOG_ERROR("定数バッファ作成に失敗: %s", r.Error().message); Quit(); return;
        }
        else _cb = Move(r.Value());

        // === テクスチャ（手続き生成）===
        u8 pixels[kTexSize * kTexSize * 4];
        GenerateTexture(pixels);

        TextureDesc td{};
        td.width  = kTexSize;
        td.height = kTexSize;
        td.format = EFormat::R8G8B8A8_UNorm;
        td.initial_data = pixels;
        td.initial_data_size = sizeof(pixels);
        if (auto r = CreateRhiTexture(*dev, td); r.IsErr()) {
            ACS_LOG_ERROR("Texture create: %s", r.Error().message); Quit(); return;
        } else _tex = Move(r.Value());

        // === パイプライン（CB 1 + Texture 1 + Sampler 1）===
        PipelineDesc pd{};
        pd.vs = _vs.Get();
        pd.ps = _ps.Get();
        pd.topology      = EPrimitiveTopology::TriangleList;
        pd.rt_format     = GetRenderer().ColorFormat();
        pd.depth_format  = GetRenderer().DepthFormat();
        pd.depth_test    = true;
        pd.depth_write   = true;
        pd.cull_mode     = ECullMode::Back;
        pd.cbuffer_slots = 1;
        pd.texture_slots = 1;
        pd.static_sampler_count = 1;
        pd.static_samplers[0].filter      = ESamplerFilter::Point;     // ピクセルアートっぽい外観
        pd.static_samplers[0].address_u   = ESamplerAddress::Wrap;
        pd.static_samplers[0].address_v   = ESamplerAddress::Wrap;
        pd.vertex_stride = sizeof(Vertex);
        pd.layout[0] = { "POSITION",  0, EFormat::R32G32B32_Float, 0 };
        pd.layout[1] = { "TEXCOORD",  0, EFormat::R32G32_Float,    sizeof(f32) * 3 };
        pd.layout_count = 2;
        if (auto r = CreateRhiPipeline(*dev, pd); r.IsErr()) {
            ACS_LOG_ERROR("Pipeline create: %s", r.Error().message); Quit(); return;
        } else _pipeline = Move(r.Value());

        // === カメラ ===
        const f32 aspect = static_cast<f32>(GetRenderer().Swapchain()->Width()) /
                           static_cast<f32>(GetRenderer().Swapchain()->Height());
        _camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 100.0f);

        ACS_LOG_INFO("HelloTextured initialized");
    }

    void OnUpdate(f32 dt) noexcept override {
        if (Input::IsKeyPressed(EKey::Escape)) Quit();

        _angle += dt * 0.6f;
        if (Input::IsKeyDown(EKey::Left))  _cam_yaw -= dt * 1.5f;
        if (Input::IsKeyDown(EKey::Right)) _cam_yaw += dt * 1.5f;

        Vec3 eye{ Sin(_cam_yaw) * 5.0f, 1.5f, -Cos(_cam_yaw) * 5.0f };
        _camera.SetLookAt(eye, {0, 0, 0});

        Mat4 model = Mat4::RotationY(_angle) * Mat4::RotationX(_angle * 0.4f);
        Mat4 mvp   = model * _camera.View() * _camera.Projection();
        _cb->Update(&mvp, sizeof(Mat4));
    }

    void OnRender() noexcept override {
        IRhiCommandList* cl = GetRenderer().CommandList();
        if (!cl || !_pipeline) return;
        cl->SetPipeline(*_pipeline);
        cl->SetConstantBuffer(0, *_cb);
        cl->SetTexture(0, *_tex);
        cl->SetVertexBuffer(*_vb, sizeof(Vertex));
        cl->SetIndexBuffer(*_ib);
        cl->DrawIndexed(36);
    }

    void OnShutdown() noexcept override {
        if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
        _pipeline.Reset();
        _tex.Reset();
        _cb.Reset();
        _ib.Reset();
        _vb.Reset();
        _ps.Reset();
        _vs.Reset();
    }

private:
    UniquePtr<IRhiShader>   _vs;
    UniquePtr<IRhiShader>   _ps;
    UniquePtr<IRhiBuffer>   _vb;
    UniquePtr<IRhiBuffer>   _ib;
    UniquePtr<IRhiBuffer>   _cb;
    UniquePtr<IRhiTexture>  _tex;
    UniquePtr<IRhiPipeline> _pipeline;

    Camera _camera;
    f32    _angle   = 0.0f;
    f32    _cam_yaw = 0.0f;
};

ACS_DEFINE_MAIN(HelloTextured)
