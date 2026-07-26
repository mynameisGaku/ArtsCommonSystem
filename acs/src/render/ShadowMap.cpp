// SPDX-License-Identifier: Apache-2.0
// FShadowMap 実装 (single + CSM atlas)
#include "render/ShadowMap.h"
#include "asset/MeshAsset.h"          // MeshVertex の input layout 用
#include "math/Math.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

#include <cmath>

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
    const f64 length = std::hypot(
        static_cast<f64>(v.x),
        static_cast<f64>(v.y),
        static_cast<f64>(v.z));
    if (!std::isfinite(length) || length < 1e-6)
        return FVec3{0, 1, 0};
    return {
        static_cast<f32>(static_cast<f64>(v.x) / length),
        static_cast<f32>(static_cast<f64>(v.y) / length),
        static_cast<f32>(static_cast<f64>(v.z) / length),
    };
}

ACS_FORCEINLINE bool IsFinite(FVec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y)
        && std::isfinite(value.z);
}

bool IsFinite(const FMat4& value) noexcept {
    for (u32 row = 0; row < 4; ++row) {
        for (u32 column = 0; column < 4; ++column) {
            if (!std::isfinite(value.m[row][column])) return false;
        }
    }
    return true;
}

/**
 * 2 点間のユークリッド距離を返す。
 *
 * @param a 始点。
 * @param b 終点。
 * @return a と b の距離。
 */
ACS_FORCEINLINE f32 Distance3(const FVec3& a, const FVec3& b) noexcept {
    return static_cast<f32>(std::hypot(
        static_cast<f64>(a.x) - static_cast<f64>(b.x),
        static_cast<f64>(a.y) - static_cast<f64>(b.y),
        static_cast<f64>(a.z) - static_cast<f64>(b.z)));
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

/**
 * 直交シャドウ投影をシャドウマップの texel grid にスナップする。
 *
 * @details
 * カメラや cascade 中心が texel 未満だけ動いたとき light VP が連続的に揺れると、
 * 固定したワールド面上でも影の境界がちらつく。world 原点の light clip 座標を
 * 最寄り texel に丸め、その差だけ投影の平行移動成分を補正することで、投影を
 * 1 texel 単位で安定させる。CSM は正方形の cascade viewport、single fallback
 * は確保済み atlas 全幅を使うため、実際の viewport 幅と高さを別々に渡す。
 */
ACS_FORCEINLINE void StabilizeOrthoProjection(const FMat4& light_view,
                                              FMat4& light_proj,
                                              u32 map_width,
                                              u32 map_height) noexcept {
    if (map_width == 0 || map_height == 0) return;

    const FMat4 light_vp = light_view * light_proj;
    const FVec4 origin   = MulRowVec4(FVec4{0, 0, 0, 1}, light_vp);
    const f32 half_width = static_cast<f32>(map_width) * 0.5f;
    const f32 half_height = static_cast<f32>(map_height) * 0.5f;
    const f32 texel_x    = origin.x * half_width;
    const f32 texel_y    = origin.y * half_height;

    // row-vector 規約では clip-space translation は projection の第 4 行。
    light_proj.m[3][0] += (Round(texel_x) - texel_x) / half_width;
    light_proj.m[3][1] += (Round(texel_y) - texel_y) / half_height;
}

} // namespace

/** 深度テクスチャ・キャスター VS・定数バッファ・depth-only パイプラインを生成する。 */
TResult<void> FShadowMap::Init(IRhiDevice& device, u32 size, u32 cascade_count) noexcept {
    Shutdown();

    if (size == 0) size = 2048;
    if (cascade_count == 0) cascade_count = 1;
    if (cascade_count > kMaxCascades) cascade_count = kMaxCascades;
    m_Size          = size;
    m_CascadeCount = cascade_count;
    m_CascadeCapacity = cascade_count;
    m_Device        = &device;
    m_CurrentCascade = 0;
    (void)BeginFrame();

    auto fail_init = [this](FErrorCode error) noexcept -> TResult<void> {
        Shutdown();
        return Err<void>(error);
    };

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
    if (tr.IsErr()) return fail_init(tr.Error());
    m_Depth = Move(tr.Value());

    // Caster VS。
    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kCasterHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "ShadowCaster.VS";
    auto vs_r = CreateRhiShader(device, vs_d);
    if (vs_r.IsErr()) return fail_init(vs_r.Error());
    m_Vs = Move(vs_r.Value());

    // 定数バッファ。
    FBufferDesc cbd{};
    cbd.size = 256;
    cbd.usage = EBufferUsage::Uniform;
    cbd.cpu_writable = true;
    for (u32 cascade = 0; cascade < m_CascadeCount; ++cascade) {
        auto lb_r = CreateRhiBuffer(device, cbd);
        if (lb_r.IsErr()) return fail_init(lb_r.Error());
        m_LightCbs[cascade] = Move(lb_r.Value());
    }

    // Keep the legacy post-Init CasterObjectCB() contract non-null per cascade.
    for (u32 cascade = 0; cascade < m_CascadeCount; ++cascade) {
        if (!EnsureCasterCapacity(cascade, 1u))
            return fail_init(ACS_ERR(
                Render, 115,
                "FShadowMap: failed to create caster constant buffer"));
    }

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
    pd.vertex_stride = sizeof(FMeshVertex);
    pd.layout[0] = { "POSITION", 0, EFormat::R32G32B32_Float, 0  };
    pd.layout[1] = { "NORMAL",   0, EFormat::R32G32B32_Float, 16 };
    pd.layout[2] = { "TEXCOORD", 0, EFormat::R32G32_Float,    32 };
    pd.layout_count = 3;
    auto pl_r = CreateRhiPipeline(device, pd);
    if (pl_r.IsErr()) return fail_init(pl_r.Error());
    m_Pipeline = Move(pl_r.Value());

    // 未使用 cascade スロットは inf split で「常に cascade 0 を選択」に
    for (u32 c = 0; c < kMaxCascades; ++c) m_CascadeSplits[c] = 1e30f;

    return Ok();
}

/** 確保した GPU リソースを解放し、サイズ・cascade 数を初期状態に戻す。 */
void FShadowMap::Shutdown() noexcept {
    m_Pipeline.Reset();
    for (u32 cascade = 0; cascade < kMaxCascades; ++cascade) {
        m_ObjectCbs[cascade].ReleaseStorage();
    }
    for (u32 i = 0; i < kMaxCascades; ++i) m_LightCbs[i].Reset();
    m_Vs.Reset();
    m_Depth.Reset();
    m_Device        = nullptr;
    m_Size          = 0;
    m_CascadeCount = 1;
    m_CascadeCapacity = 1;
    m_CurrentCascade = 0;
    m_TotalCasterDrawCount = 0;
    m_FrameCapacityReady = false;
    for (u32 cascade = 0; cascade < kMaxCascades; ++cascade) {
        m_CurrentCasters[cascade] = 0;
        m_CasterDrawCounts[cascade] = 0;
        m_CasterOverflowed[cascade] = false;
        m_CasterWarningIssued[cascade] = false;
    }
}

/** Reset the CPU cursor; already-recorded GPU addresses remain immutable. */
bool FShadowMap::BeginFrame(
    u32 required_casters_per_cascade) noexcept {
    m_TotalCasterDrawCount = 0;
    for (u32 cascade = 0; cascade < kMaxCascades; ++cascade) {
        m_CurrentCasters[cascade] = 0;
        m_CasterDrawCounts[cascade] = 0;
        m_CasterOverflowed[cascade] = false;
        m_CasterWarningIssued[cascade] = false;
    }
    m_FrameCapacityReady =
        required_casters_per_cascade != kInvalidCasterBuffer;
    for (u32 cascade = 0;
         m_FrameCapacityReady && cascade < m_CascadeCapacity;
         ++cascade) {
        m_FrameCapacityReady = EnsureCasterCapacity(
            cascade, required_casters_per_cascade);
    }
    if (!m_FrameCapacityReady) {
        for (u32 cascade = 0; cascade < kMaxCascades; ++cascade) {
            m_CurrentCasters[cascade] = kInvalidCasterBuffer;
            if (cascade < m_CascadeCapacity)
                m_CasterOverflowed[cascade] = true;
        }
    }
    return m_FrameCapacityReady;
}

/** Grow one cascade's persistent immutable per-draw CB pool. */
bool FShadowMap::EnsureCasterCapacity(
    u32 cascade, u32 required_casters) noexcept {
    if (cascade >= m_CascadeCapacity || !m_Device ||
        required_casters == kInvalidCasterBuffer) return false;
    TArray<TUniquePtr<IRhiBuffer>>& buffers = m_ObjectCbs[cascade];
    if (required_casters <= buffers.Size()) return true;

    u32 target = static_cast<u32>(buffers.Size());
    if (target == 0u) target = 1u;
    while (target < required_casters) {
        const u32 growth = target > 1u ? target / 2u : 1u;
        if (target > kInvalidCasterBuffer - growth) {
            target = required_casters;
        } else {
            target += growth;
        }
    }

    if (!buffers.TryReserve(target)) return false;
    while (buffers.Size() < target) {
        FBufferDesc desc{};
        desc.size = 256;
        desc.usage = EBufferUsage::Uniform;
        desc.cpu_writable = true;
        auto result = CreateRhiBuffer(*m_Device, desc);
        if (result.IsErr())
            return buffers.Size() >= required_casters;
        if (!buffers.TryPushBack(Move(result.Value())))
            return buffers.Size() >= required_casters;
    }
    return true;
}

/** single cascade 用に光源の LookAt + ortho を組み、cascade 0 の light VP を更新する。 */
void FShadowMap::SetDirectionalLight(FVec3 light_dir, FVec3 center, f32 radius) noexcept {
    // This API publishes a single shadow volume even when Init reserved a CSM
    // atlas. Keep the active API state aligned with the only CB updated below;
    // SetDirectionalLightCascades can restore the reserved capacity later.
    m_CascadeCount = 1;
    if (!IsFinite(center)) center = FVec3{0, 0, 0};
    if (!std::isfinite(radius) || radius < 1e-3f) radius = 1.0f;
    // Keep every intermediate comfortably inside float range. Values beyond
    // this are not representable with useful shadow-map precision anyway.
    if (radius > 1.0e12f) radius = 1.0e12f;
    const FVec3 dir = NormalizeSafe(light_dir);
    FVec3 light_pos = FVec3{
        center.x + dir.x * radius * 2.5f,
        center.y + dir.y * radius * 2.5f,
        center.z + dir.z * radius * 2.5f,
    };
    // At very large world coordinates a small offset can round back to the
    // center, making LookAtLH degenerate. Use the equivalent origin-centered
    // fallback instead of publishing a NaN matrix.
    if (!IsFinite(light_pos) || Distance3(light_pos, center) < radius) {
        center = FVec3{0, 0, 0};
        light_pos = FVec3{
            dir.x * radius * 2.5f,
            dir.y * radius * 2.5f,
            dir.z * radius * 2.5f,
        };
    }
    FVec3 up = FVec3{0, 1, 0};
    if (Abs(dir.y) > 0.95f) up = FVec3{0, 0, 1};

    const FMat4 view = FMat4::LookAtLH(light_pos, center, up);
    FMat4 proj = FMat4::OrthoLH(radius * 2.5f, radius * 2.5f, 0.0f, radius * 5.0f);
    // A CSM atlas remains physically wide after switching to one active
    // projection. Cover the complete SRV so receivers using cascade_count=1
    // and UV scale=1 do not sample three quarters of unwritten depth.
    StabilizeOrthoProjection(
        view, proj, m_Size * m_CascadeCapacity, m_Size);
    m_LightVp[0] = view * proj;
    // 残りの cascade は同じ VP を入れて split を inf に (常に cascade 0 が当たる)
    for (u32 c = 1; c < kMaxCascades; ++c) {
        m_LightVp[c]       = m_LightVp[0];
        m_CascadeSplits[c] = 1e30f;
    }
    m_CascadeSplits[0] = 1e30f;   // single mode は cascade 0 が全範囲

    m_CurrentCascade = 0;
    if (m_LightCbs[0]) m_LightCbs[0]->Update(&m_LightVp[0], sizeof(FMat4));
}

/** CSM 用に frustum を分割し、各 cascade の bounding sphere から light VP を計算する。 */
void FShadowMap::SetDirectionalLightCascades(FVec3 light_dir,
                                             const FMat4& view, const FMat4& proj,
                                             f32 near_z, f32 far_z,
                                             f32 lambda) noexcept {
    // A preceding single-volume fallback must not permanently discard the CSM
    // resources allocated by Init. Re-enable the full reserved set before
    // computing splits; another invalid input below safely returns to one.
    m_CascadeCount = m_CascadeCapacity;
    if (!std::isfinite(lambda)) lambda = 0.5f;
    if (lambda < 0.0f) lambda = 0.0f;
    if (lambda > 1.0f) lambda = 1.0f;
    if (!std::isfinite(near_z) || near_z < 1e-3f) near_z = 0.1f;
    if (!std::isfinite(far_z) || far_z <= near_z) far_z = near_z + 100.0f;

    // A singular/non-finite camera transform cannot define a frustum. Keep a
    // finite single-volume fallback instead of publishing NaN cascade matrices
    // that would make every receiver fail its shadow projection.
    if (!IsFinite(view) || !IsFinite(proj)) {
        SetDirectionalLight(light_dir, FVec3{0, 0, 0}, far_z);
        return;
    }

    // 呼び出し側が渡した near/far は「CSM を割り当てたい範囲」であり、projection が
    // 実際に持つ clip range と一致するとは限らない。NDC z=0/1 を view space へ逆投影して
    // 実 near/far を復元し、後段の corner 補間は必ずこの実範囲を基準にする。
    // これにより、例えば projection=0.05..500 / requested=0.5..300 でも far=300 が
    // frustum の 60% 地点へ正しく写り、500 までの巨大な範囲を誤って各 cascade に含めない。
    const FMat4 inv_proj = Inverse(proj);
    if (!IsFinite(inv_proj)) {
        SetDirectionalLight(light_dir, FVec3{0, 0, 0}, far_z);
        return;
    }
    const FVec4 near_h   = MulRowVec4(FVec4{0, 0, 0, 1}, inv_proj);
    const FVec4 far_h    = MulRowVec4(FVec4{0, 0, 1, 1}, inv_proj);
    f32 actual_near = near_z;
    f32 actual_far  = far_z;
    if (Abs(near_h.w) > 1e-6f && Abs(far_h.w) > 1e-6f) {
        const f32 derived_near = near_h.z / near_h.w;
        const f32 derived_far  = far_h.z / far_h.w;
        // NaN は比較が false になるため、この条件で非可逆/不正 projection も除外できる。
        if (derived_near >= 0.0f && derived_far > derived_near + 1e-4f) {
            actual_near = derived_near;
            actual_far  = derived_far;
        }
    }

    if (actual_near < 1e-3f) actual_near = 1e-3f;
    if (actual_far <= actual_near) actual_far = actual_near + 1.0f;
    if (near_z < actual_near) near_z = actual_near;
    if (far_z > actual_far) far_z = actual_far;
    if (near_z >= actual_far) near_z = actual_near;
    if (far_z <= near_z) far_z = actual_far;

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
    if (!IsFinite(inv_vp)) {
        SetDirectionalLight(light_dir, FVec3{0, 0, 0}, far_z);
        return;
    }
    const FVec4 ndc[8] = {
        FVec4{-1, -1, 0, 1}, FVec4{ 1, -1, 0, 1}, FVec4{ 1,  1, 0, 1}, FVec4{-1,  1, 0, 1},  // near
        FVec4{-1, -1, 1, 1}, FVec4{ 1, -1, 1, 1}, FVec4{ 1,  1, 1, 1}, FVec4{-1,  1, 1, 1},  // far
    };
    FVec3 fcorners[8];
    for (u32 i = 0; i < 8; ++i) {
        const FVec4 w = MulRowVec4(ndc[i], inv_vp);  // row-major: ndc * inv_vp
        if (!std::isfinite(w.w) || Abs(w.w) <= 1e-6f) {
            SetDirectionalLight(light_dir, FVec3{0, 0, 0}, far_z);
            return;
        }
        const f32  iw = 1.0f / w.w;
        fcorners[i] = FVec3{ w.x * iw, w.y * iw, w.z * iw };
        if (!IsFinite(fcorners[i])) {
            SetDirectionalLight(light_dir, FVec3{0, 0, 0}, far_z);
            return;
        }
    }

    // 各 cascade の sub-frustum bounding sphere → light VP。
    const FVec3 dir = NormalizeSafe(light_dir);
    const FVec3 up  = (Abs(dir.y) > 0.95f) ? FVec3{0, 0, 1} : FVec3{0, 1, 0};

    // fcorners は projection の実 near/far に対応するため、requested range ではなく
    // actual range で補間する。requested far が projection far より短い場合にも有効。
    const f32 inv_full = 1.0f / (actual_far - actual_near);
    for (u32 c = 0; c < m_CascadeCount; ++c) {
        const f32 z_n = splits[c];
        const f32 z_f = splits[c + 1];
        const f32 t_n = (z_n - actual_near) * inv_full;
        const f32 t_f = (z_f - actual_near) * inv_full;

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
            if (!std::isfinite(d)) {
                SetDirectionalLight(light_dir, FVec3{0, 0, 0}, far_z);
                return;
            }
            if (d > radius) radius = d;
        }
        if (radius < 1e-3f) radius = 1e-3f;
        // 浮動小数誤差による投影幅の微細な伸縮を抑え、常に外側へ量子化して clipping を防ぐ。
        radius = Ceil(radius * 16.0f) * (1.0f / 16.0f);

        // Light VP: center を見るカメラ、ortho 幅は半径×2.5 (10% margin、large caster の
        // edge clip 防止。single-cascade SetDirectionalLight と同じ margin)。
        const FVec3 light_pos{
            center.x + dir.x * radius * 2.5f,
            center.y + dir.y * radius * 2.5f,
            center.z + dir.z * radius * 2.5f,
        };
        const FMat4 view_l = FMat4::LookAtLH(light_pos, center, up);
        FMat4 proj_l = FMat4::OrthoLH(radius * 2.5f, radius * 2.5f, 0.0f, radius * 5.0f);
        StabilizeOrthoProjection(view_l, proj_l, m_Size, m_Size);
        m_LightVp[c]       = view_l * proj_l;
        m_CascadeSplits[c] = z_f;            // view-space z far (cascade 選択の閾値)
    }
    // 未使用 cascade は inf で「常にスキップ」
    for (u32 c = m_CascadeCount; c < kMaxCascades; ++c) {
        m_LightVp[c]       = m_LightVp[m_CascadeCount - 1];
        m_CascadeSplits[c] = 1e30f;
    }

    // Each cascade owns an immutable CB for all draws recorded this frame.
    for (u32 cascade = 0; cascade < m_CascadeCount; ++cascade) {
        if (m_LightCbs[cascade])
            m_LightCbs[cascade]->Update(&m_LightVp[cascade], sizeof(FMat4));
    }
    m_CurrentCascade = 0;
}

/** Select a cascade-specific CB without overwriting an already-recorded address. */
void FShadowMap::SetCurrentCascade(u32 cascade) noexcept {
    if (cascade >= m_CascadeCount) cascade = 0;
    m_CurrentCascade = cascade;
}

/** 後方互換 API。失敗は CasterOverflowed() で照会できる。 */
void FShadowMap::SetCaster(const FMat4& model) noexcept {
    (void)TrySetCaster(model);
}

/** Write the model matrix to a buffer used by this draw only. */
bool FShadowMap::TrySetCaster(const FMat4& model) noexcept {
    if (!IsFinite(model) || !m_FrameCapacityReady) return false;
    const u32 cascade = m_CurrentCascade;
    u32& draw_count = m_CasterDrawCounts[cascade];
    if (draw_count == kInvalidCasterBuffer ||
        m_TotalCasterDrawCount == kInvalidCasterBuffer) {
        m_CasterOverflowed[cascade] = true;
        if (!m_CasterWarningIssued[cascade]) {
            ACS_LOG_WARN(
                "FShadowMap cascade %u caster cursor overflow; "
                "remaining caster draws are skipped", cascade);
            m_CasterWarningIssued[cascade] = true;
        }
        m_FrameCapacityReady = false;
        return false;
    }

    const u32 slot = draw_count;
    if (!EnsureCasterCapacity(cascade, slot + 1u)) {
        m_CasterOverflowed[cascade] = true;
        if (!m_CasterWarningIssued[cascade]) {
            ACS_LOG_WARN(
                "FShadowMap failed to grow cascade %u caster-CB pool "
                "(required=%u, retained=%zu); remaining pass is skipped",
                cascade, slot + 1u, m_ObjectCbs[cascade].Size());
            m_CasterWarningIssued[cascade] = true;
        }
        m_FrameCapacityReady = false;
        m_CurrentCasters[cascade] = kInvalidCasterBuffer;
        return false;
    }

    m_CurrentCasters[cascade] = slot;
    m_ObjectCbs[cascade][slot]->Update(&model, sizeof(FMat4));
    ++draw_count;
    ++m_TotalCasterDrawCount;
    return true;
}

/** atlas 内の cascade 領域に対応する viewport を組み立てて返す。 */
FViewport FShadowMap::CascadeViewport(u32 cascade) const noexcept {
    if (cascade >= m_CascadeCount) cascade = 0;
    FViewport vp{};
    vp.x         = static_cast<f32>(cascade * m_Size);
    vp.y         = 0.0f;
    vp.width     = static_cast<f32>(
        m_CascadeCount == 1 ? m_Size * m_CascadeCapacity : m_Size);
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
    r.right  = static_cast<i32>(
        m_CascadeCount == 1
            ? m_Size * m_CascadeCapacity
            : (cascade + 1) * m_Size);
    r.bottom = static_cast<i32>(m_Size);
    return r;
}

} // namespace acs
