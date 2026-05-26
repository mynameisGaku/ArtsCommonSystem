// SPDX-License-Identifier: Apache-2.0
// HelloTextured — HLSL シェーダソース。
//
// MVP 行列を b0 から取得し、t0 のテクスチャを s0 サンプラで読む。
// PipelineDesc::static_samplers でサンプラを固定し、Pixel Shader 側は
// SamplerState を 1 つだけ宣言する。
#pragma once

namespace hellotextured {

inline constexpr const char* kHLSL = R"(
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

} // namespace hellotextured
