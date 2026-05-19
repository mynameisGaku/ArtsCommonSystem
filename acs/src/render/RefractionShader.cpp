// スクリーンスペース屈折シェーダ実装 (Phase 3)
#include "render/RefractionShader.h"
#include "asset/MeshAsset.h"
#include "foundation/Move.h"

namespace acs {

namespace {

// HLSL ソース (VS + PS、row-major)。
// 屈折: 視線を refract() で曲げ、その先の点を screen へ投影して background を sample。
// 反射: Schlick Fresnel で環境キューブマップと blend (grazing 角ほど反射が強い)。
const char* kRefractionHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer Frame : register(b0) {
    float4x4 view_proj;
    float4   camera_pos;     // xyz=eye
};

cbuffer Object : register(b1) {
    float4x4 model;
    float4   material;       // x=ior, y=thickness, zw=pad
    float4   tint;           // xyz=glass tint (吸収色), w=pad
};

Texture2D    background : register(t0);   // opaque シーンの複製
TextureCube  env        : register(t1);   // Fresnel 反射用の環境マップ
SamplerState background_sampler : register(s0);
SamplerState env_sampler        : register(s1);

struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_POSITION; float3 world_p : POSITION; float3 world_n : NORMAL; };

VSOut VSMain(VSIn v) {
    VSOut o;
    float4 wp = mul(float4(v.pos, 1.0), model);
    o.world_p = wp.xyz;
    o.pos     = mul(wp, view_proj);
    o.world_n = mul(float4(v.nrm, 0.0), model).xyz;
    return o;
}

float4 PSMain(VSOut v) : SV_TARGET {
    float3 N = normalize(v.world_n);
    float3 V = normalize(camera_pos.xyz - v.world_p);   // surface->eye
    float  ior = max(material.x, 1.0);
    float  eta = 1.0 / ior;                              // 空気->ガラス

    // --- 屈折 ---
    // 視線 (-V) を界面で屈折させ、その先 thickness ぶんの点 (exitPoint) を
    // screen へ投影、その UV で background を sample する (= 屈折で歪んだ背景)。
    float3 refractDir = refract(-V, N, eta);
    if (dot(refractDir, refractDir) < 1e-4) refractDir = -V;   // TIR 保険 (eta<1 では原理上発生しない)
    float3 exitPoint  = v.world_p + refractDir * material.y;
    float4 clip       = mul(float4(exitPoint, 1.0), view_proj);
    // exitPoint がカメラ後方 (clip.w<=0) になったら屈折を諦め world_p を投影する
    // (= 素通し)。透過光線は必ず奥へ進むので実際には稀だが、screen 投影の保険。
    if (clip.w < 1e-4) clip = mul(float4(v.world_p, 1.0), view_proj);
    float2 refractUV  = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
    float3 refracted  = background.SampleLevel(background_sampler,
                                               saturate(refractUV), 0).rgb;
    refracted *= tint.xyz;                               // ガラスの吸収色

    // --- Fresnel 反射 (環境マップ) ---
    float  NoV = saturate(dot(N, V));
    float  F   = 0.04 + 0.96 * pow(saturate(1.0 - NoV), 5.0);   // F0=0.04 (誘電体)
    float3 reflected = env.SampleLevel(env_sampler, reflect(-V, N), 0).rgb;

    // 屈折と反射を Fresnel で混合
    return float4(lerp(refracted, reflected, F), 1.0);
}
)";

// CB レイアウト (HLSL と一致、float4 アライン)
struct FrameCBLayout {
    Mat4 view_proj;
    Vec4 camera_pos;
};

struct ObjectCBLayout {
    Mat4 model;
    Vec4 material;       // x=ior, y=thickness
    Vec4 tint;           // xyz=glass tint
};

// CB は 256B にアライン (DX12 制約)
template<typename T>
constexpr usize CBSize() noexcept {
    return (sizeof(T) + 255u) & ~static_cast<usize>(255u);
}

} // namespace

Result<void> RefractionShader::Init(IRhiDevice& device, Format rt_format,
                                    Format depth_format) noexcept {
    // === シェーダコンパイル ===
    ShaderDesc vs_d{};
    vs_d.stage = ShaderStage::Vertex;
    vs_d.hlsl_source = kRefractionHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "Refraction.VS";
    auto vs_r = CreateRhiShader(device, vs_d);
    if (vs_r.IsErr()) return Err<void>(vs_r.Error());
    _vs = Move(vs_r.Value());

    ShaderDesc ps_d{};
    ps_d.stage = ShaderStage::Pixel;
    ps_d.hlsl_source = kRefractionHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "Refraction.PS";
    auto ps_r = CreateRhiShader(device, ps_d);
    if (ps_r.IsErr()) return Err<void>(ps_r.Error());
    _ps = Move(ps_r.Value());

    // === 定数バッファ ===
    BufferDesc fcb{};
    fcb.size = CBSize<FrameCBLayout>();
    fcb.usage = BufferUsage::Uniform;
    fcb.cpu_writable = true;
    auto fcb_r = CreateRhiBuffer(device, fcb);
    if (fcb_r.IsErr()) return Err<void>(fcb_r.Error());
    _frame_cb = Move(fcb_r.Value());

    BufferDesc ocb{};
    ocb.size = CBSize<ObjectCBLayout>();
    ocb.usage = BufferUsage::Uniform;
    ocb.cpu_writable = true;
    auto ocb_r = CreateRhiBuffer(device, ocb);
    if (ocb_r.IsErr()) return Err<void>(ocb_r.Error());
    _object_cb = Move(ocb_r.Value());

    // === パイプライン ===
    PipelineDesc pd{};
    pd.vs = _vs.Get();
    pd.ps = _ps.Get();
    pd.topology      = PrimitiveTopology::TriangleList;
    pd.rt_format     = rt_format;
    pd.depth_format  = depth_format;
    pd.depth_test    = depth_format != Format::Unknown;
    pd.depth_write   = true;
    pd.cull_mode     = CullMode::Back;
    pd.blend_mode    = BlendMode::Opaque;   // SS 屈折は背景を曲げて不透明描画する
    pd.cbuffer_slots = 2;     // b0=Frame, b1=Object
    pd.texture_slots = 2;     // t0=background, t1=env
    pd.cbuffer_names[0] = "Frame";
    pd.cbuffer_names[1] = "Object";
    pd.texture_names[0] = "background";
    pd.texture_names[1] = "env";
    pd.static_sampler_count = 2;
    pd.static_samplers[0].filter    = SamplerFilter::Linear;
    pd.static_samplers[0].address_u = SamplerAddress::Clamp;
    pd.static_samplers[0].address_v = SamplerAddress::Clamp;
    pd.static_samplers[1].filter    = SamplerFilter::Linear;
    pd.static_samplers[1].address_u = SamplerAddress::Clamp;
    pd.static_samplers[1].address_v = SamplerAddress::Clamp;
    pd.vertex_stride = sizeof(MeshVertex);
    pd.layout[0] = { "POSITION", 0, Format::R32G32B32_Float, 0  };
    pd.layout[1] = { "NORMAL",   0, Format::R32G32B32_Float, 16 }; // Vec3 はアライン 16
    pd.layout[2] = { "TEXCOORD", 0, Format::R32G32_Float,    32 };
    pd.layout_count = 3;
    auto pl_r = CreateRhiPipeline(device, pd);
    if (pl_r.IsErr()) return Err<void>(pl_r.Error());
    _pipeline = Move(pl_r.Value());

    return Ok();
}

void RefractionShader::Shutdown() noexcept {
    _pipeline.Reset();
    _object_cb.Reset();
    _frame_cb.Reset();
    _ps.Reset();
    _vs.Reset();
}

void RefractionShader::SetFrame(const Mat4& view_projection, Vec3 camera_pos) noexcept {
    if (!_frame_cb) return;
    FrameCBLayout cb{};
    cb.view_proj  = view_projection;
    cb.camera_pos = Vec4{camera_pos.x, camera_pos.y, camera_pos.z, 1.0f};
    _frame_cb->Update(&cb, sizeof(cb));
}

void RefractionShader::SetObject(const Mat4& model, f32 ior, f32 thickness,
                                 Vec3 tint) noexcept {
    if (!_object_cb) return;
    ObjectCBLayout cb{};
    cb.model    = model;
    cb.material = Vec4{ior < 1.0f ? 1.0f : ior,
                       thickness < 0.0f ? 0.0f : thickness, 0, 0};
    cb.tint     = Vec4{tint.x, tint.y, tint.z, 0};
    _object_cb->Update(&cb, sizeof(cb));
}

void RefractionShader::DrawMesh(IRhiCommandList& cmd, const GpuMesh& mesh,
                                const Mat4& model, IRhiTexture& background,
                                IRhiTexture& env, f32 ior, f32 thickness,
                                Vec3 tint) noexcept {
    if (!_pipeline || !mesh.vertex_buffer || !mesh.index_buffer) return;
    SetObject(model, ior, thickness, tint);

    cmd.SetPipeline(*_pipeline);
    cmd.SetConstantBuffer(0, *_frame_cb);
    cmd.SetConstantBuffer(1, *_object_cb);
    cmd.SetTexture(0, background);
    cmd.SetTexture(1, env);
    cmd.SetVertexBuffer(*mesh.vertex_buffer, mesh.vertex_stride);
    cmd.SetIndexBuffer(*mesh.index_buffer);
    cmd.DrawIndexed(mesh.index_count);
}

} // namespace acs
