// ACS の HelloMesh サンプル
//
// 動作:
//   ・3D 空間に色付きキューブを描画
//   ・毎フレーム回転する
//   ・カメラはやや上から見下ろす視点
//   ・矢印キーでカメラを左右回転
//   ・Esc で終了
//
// 学習ポイント:
//   ・定数バッファ（CB）で MVP 行列を毎フレーム更新する流れ
//   ・深度バッファによる正しい遮蔽
//   ・パイプラインの cbuffer_slots / cull_mode / depth_test 設定

#include "app/Application.h"
#include "app/EntryPoint.h"
#include "platform/Input.h"
#include "render/IRhiBuffer.h"
#include "render/IRhiShader.h"
#include "render/IRhiPipeline.h"
#include "math/Camera.h"
#include "math/Mat.h"
#include "memory/UniquePtr.h"
#include "foundation/Log.h"

using namespace acs;

namespace {

// 1 頂点 = 位置 + 色
struct Vertex {
    f32 pos[3];
    f32 col[3];
};

// 8 頂点しか作らないと面ごとに色が変えられないので、
// 24 頂点（6 面 × 4 頂点）で定義する。
constexpr Vertex kCubeVertices[24] = {
    // 前面 (-Z) 赤
    {{-1, -1, -1}, {1, 0, 0}}, {{ 1, -1, -1}, {1, 0, 0}},
    {{ 1,  1, -1}, {1, 0, 0}}, {{-1,  1, -1}, {1, 0, 0}},
    // 背面 (+Z) 緑
    {{ 1, -1,  1}, {0, 1, 0}}, {{-1, -1,  1}, {0, 1, 0}},
    {{-1,  1,  1}, {0, 1, 0}}, {{ 1,  1,  1}, {0, 1, 0}},
    // 左面 (-X) 青
    {{-1, -1,  1}, {0, 0, 1}}, {{-1, -1, -1}, {0, 0, 1}},
    {{-1,  1, -1}, {0, 0, 1}}, {{-1,  1,  1}, {0, 0, 1}},
    // 右面 (+X) 黄
    {{ 1, -1, -1}, {1, 1, 0}}, {{ 1, -1,  1}, {1, 1, 0}},
    {{ 1,  1,  1}, {1, 1, 0}}, {{ 1,  1, -1}, {1, 1, 0}},
    // 上面 (+Y) シアン
    {{-1,  1, -1}, {0, 1, 1}}, {{ 1,  1, -1}, {0, 1, 1}},
    {{ 1,  1,  1}, {0, 1, 1}}, {{-1,  1,  1}, {0, 1, 1}},
    // 下面 (-Y) マゼンタ
    {{-1, -1,  1}, {1, 0, 1}}, {{ 1, -1,  1}, {1, 0, 1}},
    {{ 1, -1, -1}, {1, 0, 1}}, {{-1, -1, -1}, {1, 0, 1}},
};

// 各面 2 三角形 = 6 面 × 6 = 36 indices
constexpr u16 kCubeIndices[36] = {
    0,  1,  2,    0,  2,  3,
    4,  5,  6,    4,  6,  7,
    8,  9, 10,    8, 10, 11,
   12, 13, 14,   12, 14, 15,
   16, 17, 18,   16, 18, 19,
   20, 21, 22,   20, 22, 23,
};

// HLSL シェーダ
//   `row_major` プラグマで Mat4 (行優先) をそのまま受け取る
const char* kHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer Frame : register(b0) {
    float4x4 mvp;
};

struct VSIn  { float3 pos : POSITION; float3 col : COLOR; };
struct VSOut { float4 pos : SV_POSITION; float3 col : COLOR; };

VSOut VSMain(VSIn v) {
    VSOut o;
    o.pos = mul(float4(v.pos, 1.0), mvp);
    o.col = v.col;
    return o;
}

float4 PSMain(VSOut v) : SV_TARGET {
    return float4(v.col, 1.0);
}
)";

} // namespace

class HelloMesh : public Application {
public:
    void OnStart() noexcept override {
        IRhiDevice* dev = GetRenderer().Device();
        if (!dev) { Quit(); return; }

        // === シェーダ ===
        ShaderDesc vs_desc{};
        vs_desc.stage = ShaderStage::Vertex;
        vs_desc.hlsl_source = kHLSL;
        vs_desc.entry_point = "VSMain";
        vs_desc.debug_name  = "Mesh.VS";
        if (auto r = CreateRhiShader(*dev, vs_desc); r.IsErr()) {
            ACS_LOG_ERROR("VS compile: %s", r.Error().message); Quit(); return;
        } else _vs = Move(r.Value());

        ShaderDesc ps_desc{};
        ps_desc.stage = ShaderStage::Pixel;
        ps_desc.hlsl_source = kHLSL;
        ps_desc.entry_point = "PSMain";
        ps_desc.debug_name  = "Mesh.PS";
        if (auto r = CreateRhiShader(*dev, ps_desc); r.IsErr()) {
            ACS_LOG_ERROR("PS compile: %s", r.Error().message); Quit(); return;
        } else _ps = Move(r.Value());

        // === 頂点 / インデックスバッファ ===
        BufferDesc vb_desc{};
        vb_desc.size = sizeof(kCubeVertices);
        vb_desc.usage = BufferUsage::Vertex;
        vb_desc.cpu_writable = true;
        vb_desc.initial_data = kCubeVertices;
        if (auto r = CreateRhiBuffer(*dev, vb_desc); r.IsErr()) {
            ACS_LOG_ERROR("VB create"); Quit(); return;
        } else _vb = Move(r.Value());

        BufferDesc ib_desc{};
        ib_desc.size = sizeof(kCubeIndices);
        ib_desc.usage = BufferUsage::Index16;
        ib_desc.cpu_writable = true;
        ib_desc.initial_data = kCubeIndices;
        if (auto r = CreateRhiBuffer(*dev, ib_desc); r.IsErr()) {
            ACS_LOG_ERROR("IB create"); Quit(); return;
        } else _ib = Move(r.Value());

        // === 定数バッファ（MVP）  256B にアライン ===
        BufferDesc cb_desc{};
        cb_desc.size = 256;
        cb_desc.usage = BufferUsage::Uniform;
        cb_desc.cpu_writable = true;
        if (auto r = CreateRhiBuffer(*dev, cb_desc); r.IsErr()) {
            ACS_LOG_ERROR("CB create"); Quit(); return;
        } else _cb = Move(r.Value());

        // === パイプライン ===
        PipelineDesc pd{};
        pd.vs = _vs.Get();
        pd.ps = _ps.Get();
        pd.topology      = PrimitiveTopology::TriangleList;
        pd.rt_format     = GetRenderer().ColorFormat();
        pd.depth_format  = GetRenderer().DepthFormat();
        pd.depth_test    = true;
        pd.depth_write   = true;
        pd.cull_mode     = CullMode::Back;
        pd.cbuffer_slots = 1;       // b0 = MVP
        pd.vertex_stride = sizeof(Vertex);
        pd.layout[0] = { "POSITION", 0, Format::R32G32B32_Float, 0 };
        pd.layout[1] = { "COLOR",    0, Format::R32G32B32_Float, sizeof(f32) * 3 };
        pd.layout_count = 2;
        if (auto r = CreateRhiPipeline(*dev, pd); r.IsErr()) {
            ACS_LOG_ERROR("Pipeline create"); Quit(); return;
        } else _pipeline = Move(r.Value());

        // === カメラ ===
        const f32 aspect = static_cast<f32>(GetRenderer().Swapchain()->Width()) /
                           static_cast<f32>(GetRenderer().Swapchain()->Height());
        _camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 100.0f);

        ACS_LOG_INFO("HelloMesh initialized");
    }

    void OnUpdate(f32 dt) noexcept override {
        if (Input::IsKeyPressed(Key::Escape)) Quit();

        // 自転
        _angle += dt * 0.8f;

        // カメラを矢印キーで左右回転
        if (Input::IsKeyDown(Key::Left))  _cam_yaw -= dt * 1.5f;
        if (Input::IsKeyDown(Key::Right)) _cam_yaw += dt * 1.5f;

        const f32 cam_dist = 5.0f;
        Vec3 eye{ Sin(_cam_yaw) * cam_dist, 2.0f, -Cos(_cam_yaw) * cam_dist };
        _camera.SetLookAt(eye, {0, 0, 0});

        // モデル行列 = 回転（Y 軸）
        Mat4 model = Mat4::RotationY(_angle);
        Mat4 mvp   = model * _camera.View() * _camera.Projection();
        _cb->Update(&mvp, sizeof(Mat4));
    }

    void OnRender() noexcept override {
        IRhiCommandList* cl = GetRenderer().CommandList();
        if (!cl || !_pipeline) return;

        cl->SetPipeline(*_pipeline);
        cl->SetConstantBuffer(0, *_cb);
        cl->SetVertexBuffer(*_vb, sizeof(Vertex));
        cl->SetIndexBuffer(*_ib);
        cl->DrawIndexed(36);
    }

    void OnShutdown() noexcept override {
        if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
        _pipeline.Reset();
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
    UniquePtr<IRhiPipeline> _pipeline;

    Camera _camera;
    f32    _angle   = 0.0f;
    f32    _cam_yaw = 0.0f;
};

ACS_DEFINE_MAIN(HelloMesh)
