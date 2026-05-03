// ShadowMap 実装
#include "render/ShadowMap.h"
#include "asset/MeshAsset.h"          // MeshVertex の input layout 用
#include "math/Math.h"
#include "foundation/Move.h"

namespace acs {

namespace {

const char* kCasterHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer LightFrame : register(b0) {
    float4x4 light_view_proj;
};
cbuffer CasterObject : register(b1) {
    float4x4 model;
};

struct VSIn {
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float2 uv  : TEXCOORD0;
};
struct VSOut {
    float4 pos : SV_POSITION;
};

VSOut VSMain(VSIn v) {
    VSOut o;
    float4 wp = mul(float4(v.pos, 1.0), model);
    o.pos = mul(wp, light_view_proj);
    return o;
}
)";

ACS_FORCEINLINE Vec3 NormalizeSafe(Vec3 v) noexcept {
    const f32 len2 = v.x*v.x + v.y*v.y + v.z*v.z;
    if (len2 < 1e-12f) return Vec3{0, 1, 0};
    const f32 inv = 1.0f / Sqrt(len2);
    return { v.x * inv, v.y * inv, v.z * inv };
}

} // namespace

Result<void> ShadowMap::Init(IRhiDevice& device, u32 size) noexcept {
    if (size == 0) size = 2048;
    _size = size;

    // ===== 深度テクスチャ（SRV 可視）=====
    TextureDesc td{};
    td.width  = size; td.height = size;
    td.format = Format::D32_Float;
    td.is_depth_target = true;
    td.shader_visible_depth = true;
    auto tr = CreateRhiTexture(device, td);
    if (tr.IsErr()) return Err<void>(tr.Error());
    _depth = Move(tr.Value());

    // ===== Caster VS =====
    ShaderDesc vs_d{};
    vs_d.stage = ShaderStage::Vertex;
    vs_d.hlsl_source = kCasterHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "ShadowCaster.VS";
    auto vs_r = CreateRhiShader(device, vs_d);
    if (vs_r.IsErr()) return Err<void>(vs_r.Error());
    _vs = Move(vs_r.Value());

    // ===== 定数バッファ =====
    BufferDesc cbd{};
    cbd.size = 256;
    cbd.usage = BufferUsage::Uniform;
    cbd.cpu_writable = true;
    auto lb_r = CreateRhiBuffer(device, cbd);
    if (lb_r.IsErr()) return Err<void>(lb_r.Error());
    _light_cb = Move(lb_r.Value());

    auto ob_r = CreateRhiBuffer(device, cbd);
    if (ob_r.IsErr()) return Err<void>(ob_r.Error());
    _object_cb = Move(ob_r.Value());

    // ===== 深度のみパイプライン =====
    PipelineDesc pd{};
    pd.vs = _vs.Get();
    pd.ps = nullptr;          // depth-only
    pd.topology      = PrimitiveTopology::TriangleList;
    pd.rt_format     = Format::Unknown;       // ps==nullptr なら無視される
    pd.depth_format  = Format::D32_Float;
    pd.depth_test    = true;
    pd.depth_write   = true;
    // 「シャドウアクネ」回避のため front-cull で物体の裏側深度を書く
    pd.cull_mode     = CullMode::Front;
    pd.cbuffer_slots = 2;
    pd.texture_slots = 0;
    pd.cbuffer_names[0] = "LightFrame";
    pd.cbuffer_names[1] = "CasterObject";
    pd.vertex_stride = sizeof(MeshVertex);
    pd.layout[0] = { "POSITION", 0, Format::R32G32B32_Float, 0  };
    pd.layout[1] = { "NORMAL",   0, Format::R32G32B32_Float, 16 };
    pd.layout[2] = { "TEXCOORD", 0, Format::R32G32_Float,    32 };
    pd.layout_count = 3;
    auto pl_r = CreateRhiPipeline(device, pd);
    if (pl_r.IsErr()) return Err<void>(pl_r.Error());
    _pipeline = Move(pl_r.Value());

    return Ok();
}

void ShadowMap::Shutdown() noexcept {
    _pipeline.Reset();
    _object_cb.Reset();
    _light_cb.Reset();
    _vs.Reset();
    _depth.Reset();
    _size = 0;
}

void ShadowMap::SetDirectionalLight(Vec3 light_dir, Vec3 center, f32 radius) noexcept {
    Vec3 dir = NormalizeSafe(light_dir);
    Vec3 light_pos = Vec3{
        center.x + dir.x * radius * 2.5f,
        center.y + dir.y * radius * 2.5f,
        center.z + dir.z * radius * 2.5f,
    };
    // up は dir と平行にならないよう場合分け
    Vec3 up = Vec3{0, 1, 0};
    if (Abs(dir.y) > 0.95f) up = Vec3{0, 0, 1};

    Mat4 view = Mat4::LookAtLH(light_pos, center, up);
    Mat4 proj = Mat4::OrthoLH(radius * 2.5f, radius * 2.5f, 0.0f, radius * 5.0f);
    _light_vp = view * proj;

    if (_light_cb) _light_cb->Update(&_light_vp, sizeof(Mat4));
}

void ShadowMap::SetCaster(const Mat4& model) noexcept {
    if (_object_cb) _object_cb->Update(&model, sizeof(Mat4));
}

} // namespace acs
