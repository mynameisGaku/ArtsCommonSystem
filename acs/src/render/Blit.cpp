// SPDX-License-Identifier: Apache-2.0
// フルスクリーン texture コピー実装
#include "render/Blit.h"
#include "foundation/Move.h"

#if !WITH_RENDER_DILIGENT
#include "render/Dx12/Dx12Shader.h"
#endif

namespace acs {

namespace {

/**
 * fullscreen blit シェーダの HLSL ソース。
 *
 * @details
 * SV_VertexID で 3 頂点の fullscreen 三角形を生成し、source texture を素 sample して
 * 出力する。頂点バッファ無しで Draw(3) で描画できる (SSR / SSGI / FPostProcess と
 * 同じパターン)。
 */
const char* kBlitHLSL = R"(
Texture2D    src : register(t0);
SamplerState src_sampler : register(s0);

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.uv  = uv;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    o.pos.y = -o.pos.y;
    return o;
}

float4 PSMain(VSOut v) : SV_TARGET {
    return src.SampleLevel(src_sampler, v.uv, 0);
}
)";

TResult<FBlit::FCompiledShaders> CompileBlitShadersWithDevice(
    IRhiDevice& device, bool compile_async) noexcept {
    FBlit::FCompiledShaders compiled{};

    FShaderDesc vertex_description{};
    vertex_description.stage = EShaderStage::Vertex;
    vertex_description.hlsl_source = kBlitHLSL;
    vertex_description.entry_point = "VSMain";
    vertex_description.debug_name = "FBlit.VS";
    vertex_description.compile_async = compile_async;
    auto vertex = CreateRhiShader(device, vertex_description);
    if (vertex.IsErr()) {
        return Err<FBlit::FCompiledShaders>(vertex.Error());
    }
    compiled.vertex = Move(vertex.Value());

    FShaderDesc pixel_description{};
    pixel_description.stage = EShaderStage::Pixel;
    pixel_description.hlsl_source = kBlitHLSL;
    pixel_description.entry_point = "PSMain";
    pixel_description.debug_name = "FBlit.PS";
    pixel_description.compile_async = compile_async;
    auto pixel = CreateRhiShader(device, pixel_description);
    if (pixel.IsErr()) {
        return Err<FBlit::FCompiledShaders>(pixel.Error());
    }
    compiled.pixel = Move(pixel.Value());

    return TResult<FBlit::FCompiledShaders>(OkInit, Move(compiled));
}

} // namespace

EShaderStatus FBlit::FCompiledShaders::Status() const noexcept {
    if (!vertex || !pixel) return EShaderStatus::Failed;
    const EShaderStatus vertex_status = vertex->Status();
    const EShaderStatus pixel_status = pixel->Status();
    if (vertex_status == EShaderStatus::Failed ||
        pixel_status == EShaderStatus::Failed) {
        return EShaderStatus::Failed;
    }
    if (vertex_status == EShaderStatus::Compiling ||
        pixel_status == EShaderStatus::Compiling) {
        return EShaderStatus::Compiling;
    }
    return EShaderStatus::Ready;
}

/** ブリット用 VS/PS をコンパイルし、rt_format に合わせた PSO を生成する。 */
TResult<void> FBlit::Init(IRhiDevice& device, EFormat rt_format) noexcept {
    auto compiled = CompileBlitShadersWithDevice(device, false);
    if (compiled.IsErr()) return Err<void>(compiled.Error());
    return InitWithCompiledShaders(
        device, Move(compiled.Value()), rt_format);
}

TResult<FBlit::FCompiledShaders> FBlit::CompileShadersCpu() noexcept {
#if !WITH_RENDER_DILIGENT
    auto compile = [](EShaderStage stage, const char* entry_point,
                      const char* debug_name) noexcept
        -> TResult<TUniquePtr<IRhiShader>> {
        FShaderDesc description{};
        description.stage = stage;
        description.hlsl_source = kBlitHLSL;
        description.entry_point = entry_point;
        description.debug_name = debug_name;
        auto shader = MakeUnique<FDx12Shader>();
        if (!shader) {
            return ACS_ERR(Memory, 725, "FBlit shader allocation failed");
        }
        const FHrResult result = shader->Init(description);
        if (result.IsErr()) {
            return ACS_ERR_OS(
                Render, 726, "FBlit shader CPU compile failed",
                static_cast<u32>(result.hr));
        }
        auto* allocator = shader.GetAllocator();
        TUniquePtr<IRhiShader> output(shader.Release(), allocator);
        return TResult<TUniquePtr<IRhiShader>>(OkInit, Move(output));
    };

    FCompiledShaders compiled{};
    auto vertex = compile(
        EShaderStage::Vertex, "VSMain", "FBlit.VS");
    if (vertex.IsErr()) return Err<FCompiledShaders>(vertex.Error());
    compiled.vertex = Move(vertex.Value());
    auto pixel = compile(
        EShaderStage::Pixel, "PSMain", "FBlit.PS");
    if (pixel.IsErr()) return Err<FCompiledShaders>(pixel.Error());
    compiled.pixel = Move(pixel.Value());
    return TResult<FCompiledShaders>(OkInit, Move(compiled));
#else
    return ACS_ERR(
        Render, 727,
        "FBlit CPU compilation is available only on raw DX12");
#endif
}

TResult<FBlit::FCompiledShaders> FBlit::BeginCompileShadersAsync(
    IRhiDevice& device) noexcept {
    if (!device.SupportsAsyncShaderCompilation()) {
        return ACS_ERR(
            Render, 728,
            "FBlit backend-managed asynchronous compilation is unsupported");
    }
    return CompileBlitShadersWithDevice(device, true);
}

TResult<void> FBlit::InitWithCompiledShaders(
    IRhiDevice& device, FCompiledShaders&& shaders,
    EFormat rt_format) noexcept {
    if (shaders.Status() != EShaderStatus::Ready) {
        return ACS_ERR(Render, 729, "FBlit compiled shader set is not ready");
    }
    FPipelineDesc pd{};
    pd.vs            = shaders.vertex.Get();
    pd.ps            = shaders.pixel.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = rt_format;
    pd.depth_format  = EFormat::Unknown;       // depth 不使用
    pd.depth_test    = false;
    pd.depth_write   = false;
    pd.cull_mode     = ECullMode::None;        // 3 頂点の fullscreen 三角形
    pd.blend_mode    = EBlendMode::Opaque;
    pd.cbuffer_slots = 0;
    pd.texture_slots = 1;
    pd.texture_names[0] = "src";
    pd.static_sampler_count = 1;
    pd.static_samplers[0].filter    = ESamplerFilter::Linear;
    pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
    // 頂点バッファ無し (SV_VertexID 駆動): vertex_stride=0, layout_count=0 (既定値)
    auto pl_r = CreateRhiPipeline(device, pd);
    if (pl_r.IsErr()) return Err<void>(pl_r.Error());
    TUniquePtr<IRhiPipeline> pipeline = Move(pl_r.Value());

    m_Pipeline.Reset();
    m_Vs = Move(shaders.vertex);
    m_Ps = Move(shaders.pixel);
    m_Pipeline = Move(pipeline);
    return Ok();
}

/** パイプラインとシェーダを解放する。 */
void FBlit::Shutdown() noexcept {
    m_Pipeline.Reset();
    m_Ps.Reset();
    m_Vs.Reset();
}

/** load 版の RT 開始でフルスクリーン三角形を描画し src を dst へ上書きコピーする。 */
void FBlit::Copy(IRhiCommandList& cmd, IRhiTexture& src, IRhiTexture& dst) noexcept {
    if (!m_Pipeline) return;
    // 全 pixel が src で上書きされるので clear 不要 → load 版で開始する。
    // (本コミットで追加した BeginRenderToTextureLoad の自然な利用例)。
    cmd.BeginRenderToTextureLoad(dst, nullptr);
    cmd.SetPipeline(*m_Pipeline);
    cmd.SetTexture(0, src);
    cmd.Draw(3, 0);                          // fullscreen 三角形 (頂点バッファ無し)
    cmd.EndRenderToTexture(dst);
}

} // namespace acs
