// SPDX-License-Identifier: Apache-2.0
// FShadowMap 実装 (single + CSM atlas)
#include "render/ShadowMap.h"
#include "asset/MeshAsset.h"          // MeshVertex の input layout 用
#include "math/Math.h"
#include "foundation/Move.h"

namespace acs {

namespace {

/** シャドウキャスターを depth-only で描く頂点シェーダの HLSL ソース。 */
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

/**
 * ゼロ除算を避けて正規化する。
 *
 * @param v 正規化するベクトル。
 * @return 正規化したベクトル (長さがほぼ 0 なら {0,1,0})。
 */
ACS_FORCEINLINE FVec3 NormalizeSafe(FVec3 v) noexcept {
    const f32 len2 = v.x*v.x + v.y*v.y + v.z*v.z;
    if (len2 < 1e-12f) return FVec3{0, 1, 0};
    const f32 inv = 1.0f / Sqrt(len2);
    return { v.x * inv, v.y * inv, v.z * inv };
}

/**
 * 2 点間のユークリッド距離を返す。
 *
 * @param a 始点。
 * @param b 終点。
 * @return a と b の距離。
 */
ACS_FORCEINLINE f32 Distance3(const FVec3& a, const FVec3& b) noexcept {
    const f32 dx = a.x - b.x;
    const f32 dy = a.y - b.y;
    const f32 dz = a.z - b.z;
    return Sqrt(dx * dx + dy * dy + dz * dz);
}

/**
 * 行ベクトル × 行列を計算する (row-major: v_out = v * M)。
 *
 * @param v 行ベクトル。
 * @param m 掛ける行列。
 * @return v * m の結果。
 */
ACS_FORCEINLINE FVec4 MulRowVec4(const FVec4& v, const FMat4& m) noexcept {
    return FVec4{
        v.x * m.m[0][0] + v.y * m.m[1][0] + v.z * m.m[2][0] + v.w * m.m[3][0],
        v.x * m.m[0][1] + v.y * m.m[1][1] + v.z * m.m[2][1] + v.w * m.m[3][1],
        v.x * m.m[0][2] + v.y * m.m[1][2] + v.z * m.m[2][2] + v.w * m.m[3][2],
        v.x * m.m[0][3] + v.y * m.m[1][3] + v.z * m.m[2][3] + v.w * m.m[3][3],
    };
}

} // namespace

/** 深度テクスチャ・キャスター VS・定数バッファ・depth-only パイプラインを生成する。 */
TResult<void> FShadowMap::Init(IRhiDevice& device, u32 size, u32 cascade_count) noexcept {
    if (size == 0) size = 2048;
    if (cascade_count == 0) cascade_count = 1;
    if (cascade_count > kMaxCascades) cascade_count = kMaxCascades;
    m_Size          = size;
    m_CascadeCount = cascade_count;

    // 深度テクスチャ (SRV 可視)。
    // single mode (cascade_count=1): size × size
    // CSM mode    (cascade_count≥2): atlas = (size * cascade_count) × size
    FTextureDesc td{};
    td.width  = size * cascade_count;
    td.height = size;
    td.format = EFormat::D32_Float;
    td.is_depth_target      = true;
    td.shader_visible_depth = true;
    auto tr = CreateRhiTexture(device, td);
    if (tr.IsErr()) return Err<void>(tr.Error());
    m_Depth = Move(tr.Value());

    // Caster VS。
    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kCasterHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "ShadowCaster.VS";
    auto vs_r = CreateRhiShader(device, vs_d);
    if (vs_r.IsErr()) return Err<void>(vs_r.Error());
    m_Vs = Move(vs_r.Value());

    // 定数バッファ。
    FBufferDesc cbd{};
    cbd.size = 256;
    cbd.usage = EBufferUsage::Uniform;
    cbd.cpu_writable = true;
    auto lb_r = CreateRhiBuffer(device, cbd);
    if (lb_r.IsErr()) return Err<void>(lb_r.Error());
    m_LightCb = Move(lb_r.Value());

    auto ob_r = CreateRhiBuffer(device, cbd);
    if (ob_r.IsErr()) return Err<void>(ob_r.Error());
    m_ObjectCb = Move(ob_r.Value());

    // 深度のみパイプライン。
    FPipelineDesc pd{};
    pd.vs = m_Vs.Get();
    pd.ps = nullptr;          // depth-only
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = EFormat::Unknown;       // ps==nullptr なら無視される
    pd.depth_format  = EFormat::D32_Float;
    pd.depth_test    = true;
    pd.depth_write   = true;
    // 「シャドウアクネ」回避のため front-cull で物体の裏側深度を書く
    pd.cull_mode     = ECullMode::Front;
    pd.cbuffer_slots = 2;
    pd.texture_slots = 0;
    pd.cbuffer_names[0] = "LightFrame";
    pd.cbuffer_names[1] = "CasterObject";
    pd.vertex_stride = sizeof(MeshVertex);
    pd.layout[0] = { "POSITION", 0, EFormat::R32G32B32_Float, 0  };
    pd.layout[1] = { "NORMAL",   0, EFormat::R32G32B32_Float, 16 };
    pd.layout[2] = { "TEXCOORD", 0, EFormat::R32G32_Float,    32 };
    pd.layout_count = 3;
    auto pl_r = CreateRhiPipeline(device, pd);
    if (pl_r.IsErr()) return Err<void>(pl_r.Error());
    m_Pipeline = Move(pl_r.Value());

    // 未使用 cascade スロットは inf split で「常に cascade 0 を選択」に
    for (u32 c = 0; c < kMaxCascades; ++c) m_CascadeSplits[c] = 1e30f;

    return Ok();
}

/** 確保した GPU リソースを解放し、サイズ・cascade 数を初期状態に戻す。 */
void FShadowMap::Shutdown() noexcept {
    m_Pipeline.Reset();
    m_ObjectCb.Reset();
    m_LightCb.Reset();
    m_Vs.Reset();
    m_Depth.Reset();
    m_Size          = 0;
    m_CascadeCount = 1;
}

/** single cascade 用に光源の LookAt + ortho を組み、cascade 0 の light VP を更新する。 */
void FShadowMap::SetDirectionalLight(FVec3 light_dir, FVec3 center, f32 radius) noexcept {
    const FVec3 dir = NormalizeSafe(light_dir);
    const FVec3 light_pos = FVec3{
        center.x + dir.x * radius * 2.5f,
        center.y + dir.y * radius * 2.5f,
        center.z + dir.z * radius * 2.5f,
    };
    FVec3 up = FVec3{0, 1, 0};
    if (Abs(dir.y) > 0.95f) up = FVec3{0, 0, 1};

    const FMat4 view = FMat4::LookAtLH(light_pos, center, up);
    const FMat4 proj = FMat4::OrthoLH(radius * 2.5f, radius * 2.5f, 0.0f, radius * 5.0f);
    m_LightVp[0] = view * proj;
    // 残りの cascade は同じ VP を入れて split を inf に (常に cascade 0 が当たる)
    for (u32 c = 1; c < kMaxCascades; ++c) {
        m_LightVp[c]       = m_LightVp[0];
        m_CascadeSplits[c] = 1e30f;
    }
    m_CascadeSplits[0] = 1e30f;   // single mode は cascade 0 が全範囲

    if (m_LightCb) m_LightCb->Update(&m_LightVp[0], sizeof(FMat4));
}

/** CSM 用に frustum を分割し、各 cascade の bounding sphere から light VP を計算する。 */
void FShadowMap::SetDirectionalLightCascades(FVec3 light_dir,
                                             const FMat4& view, const FMat4& proj,
                                             f32 near_z, f32 far_z,
                                             f32 lambda) noexcept {
    if (lambda < 0.0f) lambda = 0.0f;
    if (lambda > 1.0f) lambda = 1.0f;
    if (near_z < 1e-3f) near_z = 1e-3f;
    if (far_z <= near_z) far_z = near_z + 1.0f;

    // Zhang practical split: splits[i] = (1-λ)*uniform + λ*log
    // 配列要素は cascade 境界の view-space z (splits[0]=near、splits[N]=far)
    f32 splits[kMaxCascades + 1] = {};
    splits[0] = near_z;
    splits[m_CascadeCount] = far_z;
    const f32 inv_n = 1.0f / static_cast<f32>(m_CascadeCount);
    for (u32 c = 1; c < m_CascadeCount; ++c) {
        const f32 f         = static_cast<f32>(c) * inv_n;
        const f32 uniform_v = near_z + (far_z - near_z) * f;
        const f32 log_v     = near_z * Pow(far_z / near_z, f);
        splits[c] = lambda * log_v + (1.0f - lambda) * uniform_v;
    }

    // 全 frustum の world-space コーナー 8 個を取得。
    // ndc corner → view-projection 逆変換で world に戻す
    const FMat4 vp     = view * proj;
    const FMat4 inv_vp = Inverse(vp);
    const FVec4 ndc[8] = {
        FVec4{-1, -1, 0, 1}, FVec4{ 1, -1, 0, 1}, FVec4{ 1,  1, 0, 1}, FVec4{-1,  1, 0, 1},  // near
        FVec4{-1, -1, 1, 1}, FVec4{ 1, -1, 1, 1}, FVec4{ 1,  1, 1, 1}, FVec4{-1,  1, 1, 1},  // far
    };
    FVec3 fcorners[8];
    for (u32 i = 0; i < 8; ++i) {
        const FVec4 w = MulRowVec4(ndc[i], inv_vp);  // row-major: ndc * inv_vp
        const f32  iw = 1.0f / w.w;
        fcorners[i] = FVec3{ w.x * iw, w.y * iw, w.z * iw };
    }

    // 各 cascade の sub-frustum bounding sphere → light VP。
    const FVec3 dir = NormalizeSafe(light_dir);
    const FVec3 up  = (Abs(dir.y) > 0.95f) ? FVec3{0, 0, 1} : FVec3{0, 1, 0};

    const f32 inv_full = 1.0f / (far_z - near_z);
    for (u32 c = 0; c < m_CascadeCount; ++c) {
        const f32 z_n = splits[c];
        const f32 z_f = splits[c + 1];
        const f32 t_n = (z_n - near_z) * inv_full;
        const f32 t_f = (z_f - near_z) * inv_full;

        FVec3 sub[8];
        for (u32 i = 0; i < 4; ++i) {
            const FVec3 ray = fcorners[i + 4] - fcorners[i];
            sub[i]     = fcorners[i] + ray * t_n;
            sub[i + 4] = fcorners[i] + ray * t_f;
        }

        // 中心 = 8 corner の平均、半径 = max distance
        FVec3 center{0, 0, 0};
        for (u32 i = 0; i < 8; ++i) center += sub[i];
        center *= 0.125f;
        f32 radius = 0.0f;
        for (u32 i = 0; i < 8; ++i) {
            const f32 d = Distance3(sub[i], center);
            if (d > radius) radius = d;
        }
        if (radius < 1e-3f) radius = 1e-3f;

        // Light VP: center を見るカメラ、ortho 幅は半径×2.5 (10% margin、large caster の
        // edge clip 防止。single-cascade SetDirectionalLight と同じ margin)。
        const FVec3 light_pos{
            center.x + dir.x * radius * 2.5f,
            center.y + dir.y * radius * 2.5f,
            center.z + dir.z * radius * 2.5f,
        };
        const FMat4 view_l = FMat4::LookAtLH(light_pos, center, up);
        const FMat4 proj_l = FMat4::OrthoLH(radius * 2.5f, radius * 2.5f, 0.0f, radius * 5.0f);
        m_LightVp[c]       = view_l * proj_l;
        m_CascadeSplits[c] = z_f;            // view-space z far (cascade 選択の閾値)
    }
    // 未使用 cascade は inf で「常にスキップ」
    for (u32 c = m_CascadeCount; c < kMaxCascades; ++c) {
        m_LightVp[c]       = m_LightVp[m_CascadeCount - 1];
        m_CascadeSplits[c] = 1e30f;
    }

    // default: LightCB に cascade 0 を書いておく
    if (m_LightCb) m_LightCb->Update(&m_LightVp[0], sizeof(FMat4));
}

/** LightCB を選択 cascade の VP で更新する (範囲外は cascade 0)。 */
void FShadowMap::SetCurrentCascade(u32 cascade) noexcept {
    if (cascade >= m_CascadeCount) cascade = 0;
    if (m_LightCb) m_LightCb->Update(&m_LightVp[cascade], sizeof(FMat4));
}

/** キャスターの model 行列を per-object CB へ書き込む。 */
void FShadowMap::SetCaster(const FMat4& model) noexcept {
    if (m_ObjectCb) m_ObjectCb->Update(&model, sizeof(FMat4));
}

/** atlas 内の cascade 領域に対応する viewport を組み立てて返す。 */
FViewport FShadowMap::CascadeViewport(u32 cascade) const noexcept {
    if (cascade >= m_CascadeCount) cascade = 0;
    FViewport vp{};
    vp.x         = static_cast<f32>(cascade * m_Size);
    vp.y         = 0.0f;
    vp.width     = static_cast<f32>(m_Size);
    vp.height    = static_cast<f32>(m_Size);
    vp.min_depth = 0.0f;
    vp.max_depth = 1.0f;
    return vp;
}

/** atlas 内の cascade 領域に対応する scissor を組み立てて返す。 */
FScissorRect FShadowMap::CascadeScissor(u32 cascade) const noexcept {
    if (cascade >= m_CascadeCount) cascade = 0;
    FScissorRect r{};
    r.left   = static_cast<i32>(cascade * m_Size);
    r.top    = 0;
    r.right  = static_cast<i32>((cascade + 1) * m_Size);
    r.bottom = static_cast<i32>(m_Size);
    return r;
}

} // namespace acs
