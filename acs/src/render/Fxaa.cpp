// SPDX-License-Identifier: Apache-2.0
// High-quality FXAA fullscreen resolve.
#include "render/Fxaa.h"
#include "foundation/Move.h"

#if !WITH_RENDER_DILIGENT
#include "render/Dx12/Dx12Shader.h"
#endif

namespace acs {

namespace {

/**
 * FXAA 3.11-style quality shader.
 *
 * A full 3x3 stencil classifies the edge, a bounded bidirectional search
 * locates its endpoints, and a sub-pixel term preserves thin coverage. The
 * fixed twelve-iteration search keeps GPU cost finite at every resolution.
 */
const char* kFxaaHLSL = R"(
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

float Luma(float3 c) {
    return dot(max(c, 0.0), float3(0.2126, 0.7152, 0.0722));
}

float2 ClampUv(float2 uv, float2 px) {
    // The sampler also clamps, but keeping the search inside texel centres
    // prevents the long-edge walk from repeatedly filtering the border texel.
    return clamp(uv, px * 0.5, 1.0 - px * 0.5);
}

float4 SampleColor(float2 uv, float2 px) {
    return src.SampleLevel(src_sampler, ClampUv(uv, px), 0);
}

float SampleLuma(float2 uv, float2 px) {
    return Luma(SampleColor(uv, px).rgb);
}

float4 PSMain(VSOut v) : SV_TARGET {
    uint w, h;
    src.GetDimensions(w, h);
    float2 px = rcp(float2(max(w, 1u), max(h, 1u)));
    float2 uv = ClampUv(v.uv, px);

    float4 center = SampleColor(uv, px);
    float lM  = Luma(center.rgb);
    float lN  = SampleLuma(uv + px * float2( 0, -1), px);
    float lS  = SampleLuma(uv + px * float2( 0,  1), px);
    float lW  = SampleLuma(uv + px * float2(-1,  0), px);
    float lE  = SampleLuma(uv + px * float2( 1,  0), px);
    float lNW = SampleLuma(uv + px * float2(-1, -1), px);
    float lNE = SampleLuma(uv + px * float2( 1, -1), px);
    float lSW = SampleLuma(uv + px * float2(-1,  1), px);
    float lSE = SampleLuma(uv + px * float2( 1,  1), px);

    float lMin = min(lM, min(min(lN, lS), min(lW, lE)));
    float lMax = max(lM, max(max(lN, lS), max(lW, lE)));
    float lRange = lMax - lMin;
    // FXAA 3.11 quality-preset thresholds: ignore sub-visible contrast while
    // retaining dark thin geometry.
    if (lRange < max(0.0312, lMax * 0.125)) return center;

    // Classify the edge with the full cardinal/diagonal stencil. The old
    // diagonal-only gradient mistook long horizontal and vertical silhouettes
    // for texture detail and could not search their endpoints.
    float edgeHorizontal =
        abs(-2.0 * lW + lNW + lSW) +
        2.0 * abs(-2.0 * lM + lN + lS) +
        abs(-2.0 * lE + lNE + lSE);
    float edgeVertical =
        abs(-2.0 * lN + lNW + lNE) +
        2.0 * abs(-2.0 * lM + lW + lE) +
        abs(-2.0 * lS + lSW + lSE);
    bool horizontal = edgeHorizontal >= edgeVertical;

    float lNegative = horizontal ? lN : lW;
    float lPositive = horizontal ? lS : lE;
    float gradientNegative = abs(lNegative - lM);
    float gradientPositive = abs(lPositive - lM);
    bool negativeSteeper = gradientNegative >= gradientPositive;
    float gradient = max(gradientNegative, gradientPositive);
    float lLocalAverage =
        0.5 * (lM + (negativeSteeper ? lNegative : lPositive));

    float signedNormalStep = horizontal ? px.y : px.x;
    if (negativeSteeper) signedNormalStep = -signedNormalStep;
    float2 normalStep = horizontal
        ? float2(0.0, signedNormalStep)
        : float2(signedNormalStep, 0.0);
    float2 edgeStep = horizontal ? float2(px.x, 0.0)
                                 : float2(0.0, px.y);
    float2 edgeUv = uv + normalStep * 0.5;

    float2 uvNegative = edgeUv - edgeStep;
    float2 uvPositive = edgeUv + edgeStep;
    float lEndNegative = 0.0;
    float lEndPositive = 0.0;
    bool reachedNegative = false;
    bool reachedPositive = false;
    const float gradientThreshold = gradient * 0.25;

    // Fixed finite quality budget. The widening tail reaches long shallow
    // edges without unbounded loops or resolution-dependent worst cases.
    static const float kSearchStep[12] = {
        1.0, 1.0, 1.0, 1.5, 1.5, 2.0,
        2.0, 2.0, 3.0, 4.0, 6.0, 8.0
    };
    [unroll]
    for (int i = 0; i < 12; ++i) {
        if (!reachedNegative) {
            lEndNegative = SampleLuma(uvNegative, px) - lLocalAverage;
            reachedNegative = abs(lEndNegative) >= gradientThreshold;
            if (!reachedNegative) uvNegative -= edgeStep * kSearchStep[i];
        }
        if (!reachedPositive) {
            lEndPositive = SampleLuma(uvPositive, px) - lLocalAverage;
            reachedPositive = abs(lEndPositive) >= gradientThreshold;
            if (!reachedPositive) uvPositive += edgeStep * kSearchStep[i];
        }
        if (reachedNegative && reachedPositive) break;
    }

    float distanceNegative = horizontal ? uv.x - uvNegative.x
                                        : uv.y - uvNegative.y;
    float distancePositive = horizontal ? uvPositive.x - uv.x
                                        : uvPositive.y - uv.y;
    distanceNegative = max(distanceNegative, 0.0);
    distancePositive = max(distancePositive, 0.0);
    bool useNegative = distanceNegative < distancePositive;
    float nearestDistance = min(distanceNegative, distancePositive);
    float edgeSpan = max(distanceNegative + distancePositive, 1e-6);
    float endpointDelta = useNegative ? lEndNegative : lEndPositive;
    bool centerBelowAverage = lM < lLocalAverage;
    bool endpointOpposesCenter =
        ((endpointDelta < 0.0) != centerBelowAverage);
    float edgeOffset = endpointOpposesCenter
        ? max(0.0, 0.5 - nearestDistance / edgeSpan)
        : 0.0;

    // Preserve thin sub-pixel coverage even when both endpoint searches land
    // symmetrically. This remains below half a pixel so resolved texture
    // detail is not softened.
    float neighborhoodLuma =
        (2.0 * (lN + lS + lW + lE) + lNW + lNE + lSW + lSE) / 12.0;
    float subpixel = saturate(
        abs(neighborhoodLuma - lM) / max(lRange, 1e-6));
    subpixel = smoothstep(0.0, 1.0, subpixel);
    subpixel = subpixel * subpixel * 0.75;

    float finalOffset = min(max(edgeOffset, subpixel), 0.5);
    return SampleColor(uv + normalStep * finalOffset, px);
}
)";

} // namespace

namespace {

TResult<CFxaa::FCompiledShaders> CompileFxaaShadersWithDevice(
    IRhiDevice& device, bool compile_async) noexcept {
    CFxaa::FCompiledShaders compiled{};
    auto compile = [&](EShaderStage stage, const char* debug_name,
                       TUniquePtr<IRhiShader>& output) noexcept
        -> TResult<void> {
        FShaderDesc description{};
        description.stage = stage;
        description.hlsl_source = kFxaaHLSL;
        description.entry_point = stage == EShaderStage::Vertex
            ? "VSMain" : "PSMain";
        description.debug_name = debug_name;
        description.compile_async = compile_async;
        auto result = CreateRhiShader(device, description);
        if (result.IsErr()) return Err<void>(result.Error());
        output = Move(result.Value());
        return Ok();
    };

    if (auto r = compile(EShaderStage::Vertex, "CFxaa.VS",
                         compiled.vertex); r.IsErr()) {
        return Err<CFxaa::FCompiledShaders>(r.Error());
    }
    if (auto r = compile(EShaderStage::Pixel, "CFxaa.PS",
                         compiled.pixel); r.IsErr()) {
        return Err<CFxaa::FCompiledShaders>(r.Error());
    }
    return TResult<CFxaa::FCompiledShaders>(OkInit, Move(compiled));
}

} // namespace

EShaderStatus CFxaa::FCompiledShaders::Status() const noexcept {
    if (!vertex || !pixel) return EShaderStatus::Failed;
    bool compiling = false;
    const EShaderStatus vertex_status = vertex->Status();
    const EShaderStatus pixel_status = pixel->Status();
    if (vertex_status == EShaderStatus::Failed
        || pixel_status == EShaderStatus::Failed) {
        return EShaderStatus::Failed;
    }
    compiling = vertex_status == EShaderStatus::Compiling
        || pixel_status == EShaderStatus::Compiling;
    return compiling ? EShaderStatus::Compiling : EShaderStatus::Ready;
}

TResult<CFxaa::FCompiledShaders> CFxaa::CompileShadersCpu() noexcept {
#if !WITH_RENDER_DILIGENT
    CFxaa::FCompiledShaders compiled{};
    auto compile = [](EShaderStage stage, const char* debug_name,
                      TUniquePtr<IRhiShader>& output) noexcept
        -> TResult<void> {
        FShaderDesc description{};
        description.stage = stage;
        description.hlsl_source = kFxaaHLSL;
        description.entry_point = stage == EShaderStage::Vertex
            ? "VSMain" : "PSMain";
        description.debug_name = debug_name;

        auto shader = MakeUnique<FDx12Shader>();
        if (!shader) {
            return ACS_ERR(
                Memory, 730, "FXAA shader allocation failed");
        }
        const FHrResult result = shader->Init(description);
        if (result.IsErr()) {
            return ACS_ERR_OS(
                Render, 731, "FXAA shader CPU compile failed",
                static_cast<u32>(result.hr));
        }
        auto* allocator = shader.GetAllocator();
        output = TUniquePtr<IRhiShader>(shader.Release(), allocator);
        return Ok();
    };

    if (auto r = compile(EShaderStage::Vertex, "CFxaa.VS", compiled.vertex);
        r.IsErr()) return Err<CFxaa::FCompiledShaders>(r.Error());
    if (auto r = compile(EShaderStage::Pixel, "CFxaa.PS", compiled.pixel);
        r.IsErr()) return Err<CFxaa::FCompiledShaders>(r.Error());
    return TResult<CFxaa::FCompiledShaders>(OkInit, Move(compiled));
#else
    return ACS_ERR(
        Render, 732,
        "FXAA CPU compilation is available only on raw DX12");
#endif
}

TResult<CFxaa::FCompiledShaders> CFxaa::BeginCompileShadersAsync(
    IRhiDevice& device) noexcept {
    if (!device.SupportsAsyncShaderCompilation()) {
        return ACS_ERR(
            Render, 733,
            "FXAA backend-managed asynchronous compilation is unsupported");
    }
    return CompileShaders(device, true);
}

TResult<CFxaa::FCompiledShaders> CFxaa::CompileShaders(
    IRhiDevice& device, bool compile_async) noexcept {
    return CompileFxaaShadersWithDevice(device, compile_async);
}

TResult<void> CFxaa::Init(IRhiDevice& device, EFormat rt_format) noexcept {
    auto compiled = CompileShaders(device, false);
    if (compiled.IsErr()) return Err<void>(compiled.Error());
    return InitWithCompiledShaders(
        device, Move(compiled.Value()), rt_format);
}

TResult<void> CFxaa::InitWithCompiledShaders(
    IRhiDevice& device, FCompiledShaders&& shaders,
    EFormat rt_format) noexcept {
    if (shaders.Status() != EShaderStatus::Ready) {
        return ACS_ERR(Render, 734, "FXAA compiled shader set is not ready");
    }

    TUniquePtr<IRhiShader> vertex = Move(shaders.vertex);
    TUniquePtr<IRhiShader> pixel = Move(shaders.pixel);

    FPipelineDesc pd{};
    pd.vs            = vertex.Get();
    pd.ps            = pixel.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = rt_format;
    pd.depth_format  = EFormat::Unknown;
    pd.depth_test    = false;
    pd.depth_write   = false;
    pd.cull_mode     = ECullMode::None;
    pd.blend_mode    = EBlendMode::Opaque;
    pd.cbuffer_slots = 0;
    pd.texture_slots = 1;
    pd.texture_names[0] = "src";
    pd.static_sampler_count = 1;
    pd.static_samplers[0].filter    = ESamplerFilter::Linear;
    pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
    auto pl_r = CreateRhiPipeline(device, pd);
    if (pl_r.IsErr()) return Err<void>(pl_r.Error());

    m_Vs = Move(vertex);
    m_Ps = Move(pixel);
    m_Pipeline = Move(pl_r.Value());

    return Ok();
}

void CFxaa::Shutdown() noexcept {
    m_Pipeline.Reset();
    m_Ps.Reset();
    m_Vs.Reset();
}

void CFxaa::Apply(IRhiCommandList& cmd, IRhiTexture& src) noexcept {
    if (!m_Pipeline) return;
    cmd.SetPipeline(*m_Pipeline);
    cmd.SetTexture(0, src);
    cmd.Draw(3, 0);
}

} // namespace acs
