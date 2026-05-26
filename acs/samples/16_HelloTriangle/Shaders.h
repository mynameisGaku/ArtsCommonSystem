// SPDX-License-Identifier: Apache-2.0
// HelloTriangle — HLSL シェーダソース。
//
// VS + PS を 1 文字列にまとめて持ち、ShaderDesc::hlsl_source に渡す。
// inline constexpr で複数 TU 安全 (C++17)。
#pragma once

namespace hellotri {

// VS は POSITION/COLOR の入力をそのまま SV_POSITION に渡し、
// PS は補間された色をそのまま出力する最小シェーダ。
inline constexpr const char* kHLSL = R"(
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

} // namespace hellotri
