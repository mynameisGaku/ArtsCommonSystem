// SPDX-License-Identifier: Apache-2.0
// ACS サンプル 16 — Hello Triangle（低レベル RHI トラックの 1 本目）
//
// ※ このサンプルは HLSL シェーダを直接書く発展編です。グラフィックスが
//    初めてなら、まず 01〜15 の高レベル API サンプル（StandardShader /
//    SpriteBatch でシェーダを書かずに描く）に慣れてから読んでください。
//
// 動作:
//   ・ウィンドウを開く
//   ・カラフルな三角形を毎フレーム描画
//   ・Esc で終了

#include "app/Application.h"
#include "app/EntryPoint.h"
#include "platform/Input.h"
#include "render/Render.h"   // RHI インターフェイス + 高レベルヘルパをまとめて include
#include "memory/UniquePtr.h"
#include "foundation/Log.h"

using namespace acs;

namespace {

// 1 頂点 = 位置 (Vec3) + 色 (Vec3)
struct Vertex {
    f32 pos[3];
    f32 col[3];
};

// HLSL シェーダソース（VS + PS を 1 文字列に同居）
const char* kHLSL = R"(
struct VSIn {
    float3 pos : POSITION;
    float3 col : COLOR;
};
struct VSOut {
    float4 pos : SV_POSITION;
    float3 col : COLOR;
};

VSOut VSMain(VSIn v) {
    VSOut o;
    o.pos = float4(v.pos, 1.0);
    o.col = v.col;
    return o;
}

float4 PSMain(VSOut v) : SV_TARGET {
    return float4(v.col, 1.0);
}
)";

} // namespace

class HelloTriangle : public Application {
public:
    void OnStart() noexcept override {
        // 1. シェーダコンパイル（VS + PS）
        ShaderDesc vs_desc{};
        vs_desc.stage = ShaderStage::Vertex;
        vs_desc.hlsl_source = kHLSL;
        vs_desc.entry_point = "VSMain";
        vs_desc.debug_name  = "Triangle.VS";
        auto vs_r = CreateRhiShader(*GetRenderer().Device(), vs_desc);
        if (vs_r.IsErr()) {
            ACS_LOG_ERROR("VS compile failed: %s", vs_r.Error().message);
            Quit();
            return;
        }
        _vs = Move(vs_r.Value());

        ShaderDesc ps_desc{};
        ps_desc.stage = ShaderStage::Pixel;
        ps_desc.hlsl_source = kHLSL;
        ps_desc.entry_point = "PSMain";
        ps_desc.debug_name  = "Triangle.PS";
        auto ps_r = CreateRhiShader(*GetRenderer().Device(), ps_desc);
        if (ps_r.IsErr()) {
            ACS_LOG_ERROR("PS compile failed: %s", ps_r.Error().message);
            Quit();
            return;
        }
        _ps = Move(ps_r.Value());

        // 2. 頂点バッファ作成（3 頂点ぶん）
        Vertex verts[3] = {
            {{ 0.0f,  0.7f, 0.0f}, {1.0f, 0.0f, 0.0f}},  // 上 (赤)
            {{ 0.7f, -0.7f, 0.0f}, {0.0f, 1.0f, 0.0f}},  // 右下 (緑)
            {{-0.7f, -0.7f, 0.0f}, {0.0f, 0.0f, 1.0f}},  // 左下 (青)
        };
        BufferDesc vb_desc{};
        vb_desc.size = sizeof(verts);
        vb_desc.usage = BufferUsage::Vertex;
        vb_desc.cpu_writable = true;
        vb_desc.initial_data = verts;
        auto vb_r = CreateRhiBuffer(*GetRenderer().Device(), vb_desc);
        if (vb_r.IsErr()) {
            ACS_LOG_ERROR("Vertex buffer create failed");
            Quit();
            return;
        }
        _vb = Move(vb_r.Value());

        // 3. パイプライン作成（VS + PS + 入力レイアウト）
        PipelineDesc pd{};
        pd.vs = _vs.Get();
        pd.ps = _ps.Get();
        pd.topology = PrimitiveTopology::TriangleList;
        pd.rt_format = Format::B8G8R8A8_UNorm;
        pd.depth_format = Format::Unknown;
        pd.vertex_stride = sizeof(Vertex);
        pd.layout[0] = { "POSITION", 0, Format::R32G32B32_Float, 0 };
        pd.layout[1] = { "COLOR",    0, Format::R32G32B32_Float, sizeof(f32) * 3 };
        pd.layout_count = 2;
        auto pl_r = CreateRhiPipeline(*GetRenderer().Device(), pd);
        if (pl_r.IsErr()) {
            ACS_LOG_ERROR("Pipeline create failed");
            Quit();
            return;
        }
        _pipeline = Move(pl_r.Value());

        ACS_LOG_INFO("HelloTriangle initialized");
    }

    void OnUpdate(f32 /*dt*/) noexcept override {
        if (Input::IsKeyPressed(Key::Escape)) Quit();
    }

    void OnRender() noexcept override {
        // BeginFrame は基底クラスが先に呼んでくれる（クリア済み）
        IRhiCommandList* cl = GetRenderer().CommandList();
        if (!cl || !_pipeline || !_vb) return;

        cl->SetPipeline(*_pipeline);
        cl->SetVertexBuffer(*_vb, sizeof(Vertex));
        cl->Draw(3);
    }

    void OnShutdown() noexcept override {
        // GPU が描画完了するまで待ってからリソースを解放
        if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
        _pipeline.Reset();
        _vb.Reset();
        _ps.Reset();
        _vs.Reset();
    }

private:
    UniquePtr<IRhiShader>    _vs;
    UniquePtr<IRhiShader>    _ps;
    UniquePtr<IRhiBuffer>    _vb;
    UniquePtr<IRhiPipeline>  _pipeline;
};

ACS_DEFINE_MAIN(HelloTriangle)
