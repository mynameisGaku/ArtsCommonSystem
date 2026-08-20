// SPDX-License-Identifier: Apache-2.0
#include "render/Sprite3DRenderer.h"

#include "foundation/Limits.h"
#include "foundation/Move.h"

#include <cmath>

#if !WITH_RENDER_DILIGENT
#include "render/Dx12/Dx12Shader.h"
#endif

namespace acs {

namespace {

/** editorのSPR3Dと同じUV、alpha cutoff、固定向きXY板を描くシェーダ。 */
constexpr char kSprite3DHlsl[] =
    "#pragma pack_matrix(row_major)\n"
    "cbuffer Frame : register(b0) { float4x4 view_projection; };\n"
    "Texture2D SpriteTexture : register(t0);\n"
    "SamplerState SpriteTexture_sampler : register(s0);\n"
    "struct VSInput { float3 position : POSITION; float2 uv : TEXCOORD0; };\n"
    "struct VSOutput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };\n"
    "VSOutput VSMain(VSInput input) {\n"
    "    VSOutput output;\n"
    "    output.position = mul(float4(input.position, 1.0), view_projection);\n"
    "    output.uv = input.uv;\n"
    "    return output;\n"
    "}\n"
    "float4 PSMain(VSOutput input) : SV_TARGET {\n"
    "    float4 sampled = SpriteTexture.Sample(SpriteTexture_sampler, input.uv);\n"
    "    clip(sampled.a - 0.02);\n"
    "    return sampled;\n"
    "}\n";

/** view×projectionだけをGPUへ渡す定数buffer layout。 */
struct FFrameBufferLayout {
    FMat4 ViewProjection;
};

/** DX12定数bufferの256byte境界へ型サイズを切り上げる。 */
template<typename T>
constexpr usize ConstantBufferSize() noexcept
{
    return (sizeof(T) + 255u) & ~static_cast<usize>(255u);
}

/** 行列とベクトルが有限かを確認する。 */
bool IsFinite(const FMat4& value) noexcept
{
    for (u32 row = 0u; row < 4u; ++row) {
        for (u32 column = 0u; column < 4u; ++column) {
            if (!std::isfinite(value.m[row][column])) return false;
        }
    }
    return true;
}

/** device経由で同期または非同期のシェーダ組を生成する。 */
TResult<CSprite3DRenderer::FCompiledShaders> CompileSpriteShadersWithDevice(IRhiDevice& device, bool compile_async) noexcept
{
    CSprite3DRenderer::FCompiledShaders compiled{};

    FShaderDesc vertex_description{};
    vertex_description.stage = EShaderStage::Vertex;
    vertex_description.hlsl_source = kSprite3DHlsl;
    vertex_description.entry_point = "VSMain";
    vertex_description.debug_name = "CSprite3DRenderer.VS";
    vertex_description.compile_async = compile_async;
    auto vertex = CreateRhiShader(device, vertex_description);
    if (vertex.IsErr())
        return Err<CSprite3DRenderer::FCompiledShaders>(vertex.Error());
    compiled.Vertex = Move(vertex.Value());

    FShaderDesc pixel_description{};
    pixel_description.stage = EShaderStage::Pixel;
    pixel_description.hlsl_source = kSprite3DHlsl;
    pixel_description.entry_point = "PSMain";
    pixel_description.debug_name = "CSprite3DRenderer.PS";
    pixel_description.compile_async = compile_async;
    auto pixel = CreateRhiShader(device, pixel_description);
    if (pixel.IsErr())
        return Err<CSprite3DRenderer::FCompiledShaders>(pixel.Error());
    compiled.Pixel = Move(pixel.Value());

    return TResult<CSprite3DRenderer::FCompiledShaders>(OkInit, Move(compiled));
}

} // namespace

EShaderStatus CSprite3DRenderer::FCompiledShaders::Status() const noexcept
{
    if (!Vertex || !Pixel) return EShaderStatus::Failed;
    const EShaderStatus vertex_status = Vertex->Status();
    const EShaderStatus pixel_status = Pixel->Status();
    if (vertex_status == EShaderStatus::Failed || pixel_status == EShaderStatus::Failed) {
        return EShaderStatus::Failed;
    }
    if (vertex_status == EShaderStatus::Compiling || pixel_status == EShaderStatus::Compiling) {
        return EShaderStatus::Compiling;
    }
    return EShaderStatus::Ready;
}

TResult<void> CSprite3DRenderer::Init(IRhiDevice& device, EFormat render_target_format, EFormat depth_format, u32 max_sprite_count) noexcept
{
    auto compiled = CompileSpriteShadersWithDevice(device, false);
    if (compiled.IsErr()) return Err<void>(compiled.Error());
    return InitWithCompiledShaders(device, Move(compiled.Value()), render_target_format, depth_format, max_sprite_count);
}

TResult<CSprite3DRenderer::FCompiledShaders>
CSprite3DRenderer::CompileShadersCpu() noexcept
{
#if !WITH_RENDER_DILIGENT
    auto compile = [](EShaderStage stage, const char* entry_point, const char* debug_name) noexcept
        -> TResult<TUniquePtr<IRhiShader>> {
        FShaderDesc description{};
        description.stage = stage;
        description.hlsl_source = kSprite3DHlsl;
        description.entry_point = entry_point;
        description.debug_name = debug_name;
        auto shader = MakeUnique<FDx12Shader>();
        if (!shader)
            return ACS_ERR(Memory, 780, "3D sprite shader allocation failed");
        const FHrResult result = shader->Init(description);
        if (result.IsErr()) {
            return ACS_ERR_OS(Render, 781, "3D sprite shader CPU compile failed", static_cast<u32>(result.hr));
        }
        auto* allocator = shader.GetAllocator();
        TUniquePtr<IRhiShader> output(shader.Release(), allocator);
        return TResult<TUniquePtr<IRhiShader>>(OkInit, Move(output));
    };

    FCompiledShaders compiled{};
    auto vertex = compile(EShaderStage::Vertex, "VSMain", "CSprite3DRenderer.VS");
    if (vertex.IsErr()) return Err<FCompiledShaders>(vertex.Error());
    compiled.Vertex = Move(vertex.Value());
    auto pixel = compile(EShaderStage::Pixel, "PSMain", "CSprite3DRenderer.PS");
    if (pixel.IsErr()) return Err<FCompiledShaders>(pixel.Error());
    compiled.Pixel = Move(pixel.Value());
    return TResult<FCompiledShaders>(OkInit, Move(compiled));
#else
    return ACS_ERR(Render, 782, "3D sprite CPU compilation is available only on raw DX12");
#endif
}

TResult<CSprite3DRenderer::FCompiledShaders>
CSprite3DRenderer::BeginCompileShadersAsync(IRhiDevice& device) noexcept
{
    if (!device.SupportsAsyncShaderCompilation()) {
        return ACS_ERR(Render, 783, "3D sprite backend asynchronous compilation is unsupported");
    }
    return CompileSpriteShadersWithDevice(device, true);
}

TResult<void> CSprite3DRenderer::InitWithCompiledShaders(IRhiDevice& device, FCompiledShaders&& shaders, EFormat render_target_format, EFormat depth_format, u32 max_sprite_count) noexcept
{
    constexpr u32 kMaximumSpriteCount = 65536u;
    if (shaders.Status() != EShaderStatus::Ready) {
        return ACS_ERR(Render, 784, "3D sprite shader set is not ready");
    }
    if (render_target_format == EFormat::Unknown || depth_format == EFormat::Unknown || max_sprite_count == 0u || max_sprite_count > kMaximumSpriteCount) {
        return ACS_ERR(Render, 785, "3D sprite initialization input is invalid");
    }
    if (static_cast<usize>(max_sprite_count) > TNumLimits<usize>::Max() / (6u * sizeof(FVertex))) {
        return ACS_ERR(Render, 786, "3D sprite vertex capacity overflows");
    }

    TArray<FVertex> vertices(*m_Vertices.GetAllocator());
    if (!vertices.TryReserve(static_cast<usize>(max_sprite_count) * 6u)) {
        return ACS_ERR(Memory, 787, "3D sprite CPU vertex allocation failed");
    }

    FPipelineDesc pipeline_description{};
    pipeline_description.vs = shaders.Vertex.Get();
    pipeline_description.ps = shaders.Pixel.Get();
    pipeline_description.topology = EPrimitiveTopology::TriangleList;
    pipeline_description.rt_format = render_target_format;
    pipeline_description.depth_format = depth_format;
    pipeline_description.depth_test = true;
    pipeline_description.depth_write = false;
    pipeline_description.cull_mode = ECullMode::None;
    pipeline_description.blend_mode = EBlendMode::AlphaBlend;
    pipeline_description.cbuffer_slots = 1u;
    pipeline_description.cbuffer_names[0] = "Frame";
    pipeline_description.texture_slots = 1u;
    pipeline_description.texture_names[0] = "SpriteTexture";
    pipeline_description.static_sampler_count = 1u;
    pipeline_description.static_samplers[0].filter = ESamplerFilter::Linear;
    pipeline_description.static_samplers[0].address_u = ESamplerAddress::Clamp;
    pipeline_description.static_samplers[0].address_v = ESamplerAddress::Clamp;
    pipeline_description.vertex_stride = sizeof(FVertex);
    pipeline_description.layout[0] = {"POSITION", 0u, EFormat::R32G32B32_Float, 0u};
    pipeline_description.layout[1] = {"TEXCOORD", 0u, EFormat::R32G32_Float, sizeof(FVec3)};
    pipeline_description.layout_count = 2u;
    auto pipeline_result = CreateRhiPipeline(device, pipeline_description);
    if (pipeline_result.IsErr()) return Err<void>(pipeline_result.Error());
    TUniquePtr<IRhiPipeline> pipeline = Move(pipeline_result.Value());

    FBufferDesc vertex_description{};
    vertex_description.size =
        static_cast<usize>(max_sprite_count) * 6u * sizeof(FVertex);
    vertex_description.usage = EBufferUsage::Vertex;
    vertex_description.cpu_writable = true;
    auto vertex_result = CreateRhiBuffer(device, vertex_description);
    if (vertex_result.IsErr()) return Err<void>(vertex_result.Error());
    TUniquePtr<IRhiBuffer> vertex_buffer = Move(vertex_result.Value());

    FBufferDesc frame_description{};
    frame_description.size = ConstantBufferSize<FFrameBufferLayout>();
    frame_description.usage = EBufferUsage::Uniform;
    frame_description.cpu_writable = true;
    auto frame_result = CreateRhiBuffer(device, frame_description);
    if (frame_result.IsErr()) return Err<void>(frame_result.Error());
    TUniquePtr<IRhiBuffer> frame_buffer = Move(frame_result.Value());

    m_Pipeline = Move(pipeline);
    m_VertexBuffer = Move(vertex_buffer);
    m_FrameBuffer = Move(frame_buffer);
    m_VertexShader = Move(shaders.Vertex);
    m_PixelShader = Move(shaders.Pixel);
    m_Vertices = Move(vertices);
    m_MaxSpriteCount = max_sprite_count;
    return Ok();
}

void CSprite3DRenderer::Shutdown() noexcept
{
    m_FrameBuffer.Reset();
    m_VertexBuffer.Reset();
    m_Pipeline.Reset();
    m_PixelShader.Reset();
    m_VertexShader.Reset();
    m_Vertices.Empty();
    m_MaxSpriteCount = 0u;
}

bool CSprite3DRenderer::TryBuildVertices(const FMat4& world, FVertex (&output)[6]) noexcept
{
    if (!IsFinite(world)) return false;
    constexpr FVec3 kLocalPositions[4] = {{-0.5f, 0.5f, 0.0f}, {0.5f, 0.5f, 0.0f}, {-0.5f, -0.5f, 0.0f}, {0.5f, -0.5f, 0.0f}};
    constexpr FVec2 kUvs[4] = {{1.0f, 0.0f}, {0.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    constexpr u32 kIndices[6] = {0u, 2u, 3u, 0u, 3u, 1u};
    FVertex candidate[6]{};
    for (u32 output_index = 0u; output_index < 6u; ++output_index) {
        const u32 source_index = kIndices[output_index];
        const FVec3 position = TransformPoint(kLocalPositions[source_index], world);
        if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
            return false;
        }
        candidate[output_index] = FVertex{position, kUvs[source_index]};
    }
    for (u32 output_index = 0u; output_index < 6u; ++output_index) {
        output[output_index] = candidate[output_index];
    }
    return true;
}

bool CSprite3DRenderer::DrawBatch(IRhiCommandList& command_list, const FMat4& view_projection, const FDraw* draws, u32 count) noexcept
{
    if (!m_Pipeline || !m_VertexBuffer || !m_FrameBuffer || !IsFinite(view_projection) || count > m_MaxSpriteCount || (count > 0u && draws == nullptr)) {
        return false;
    }
    if (count == 0u) return true;

    const usize vertex_count = static_cast<usize>(count) * 6u;
    if (!m_Vertices.TrySetNum(vertex_count)) return false;
    for (u32 draw_index = 0u; draw_index < count; ++draw_index) {
        if (draws[draw_index].Texture == nullptr) return false;
        FVertex vertices[6]{};
        if (!TryBuildVertices(draws[draw_index].World, vertices)) return false;
        for (u32 vertex_index = 0u; vertex_index < 6u; ++vertex_index) {
            m_Vertices[static_cast<usize>(draw_index) * 6u + vertex_index] =
                vertices[vertex_index];
        }
    }

    FFrameBufferLayout frame{};
    frame.ViewProjection = view_projection;
    m_FrameBuffer->Update(&frame, sizeof(frame));
    m_VertexBuffer->Update(m_Vertices.GetData(), vertex_count * sizeof(FVertex), 0u);
    command_list.SetPipeline(*m_Pipeline);
    command_list.SetConstantBuffer(0u, *m_FrameBuffer);
    command_list.SetVertexBuffer(*m_VertexBuffer, sizeof(FVertex));
    for (u32 draw_index = 0u; draw_index < count; ++draw_index) {
        command_list.SetTexture(0u, *draws[draw_index].Texture);
        command_list.Draw(6u, draw_index * 6u);
    }
    return true;
}

IRhiPipeline* CSprite3DRenderer::Pipeline() const noexcept
{
    return m_Pipeline.Get();
}

} // namespace acs
