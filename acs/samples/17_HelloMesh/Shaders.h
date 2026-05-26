// SPDX-License-Identifier: Apache-2.0
// HelloMesh — HLSL シェーダソース。
//
// MVP 行列を b0 定数バッファから取得して頂点位置を変換する 3D 描画の最小構成。
// inline constexpr で複数 TU 安全 (C++17)。
#pragma once

namespace hellomesh {

// `row_major` プラグマで Mat4 (行優先) をそのまま受け取る。
// b0 = MVP のみ。色は頂点バッファから補間してそのまま出力。
inline constexpr const char* kHLSL = R"(
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

} // namespace hellomesh
