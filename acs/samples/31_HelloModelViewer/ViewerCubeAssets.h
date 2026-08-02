// SPDX-License-Identifier: Apache-2.0
// HelloModelViewer — 3D viewport が描画する "default cube" の静的データ。
//
// サンプル 17_HelloMesh の cube を最小流用したもの。本 sample の主眼は editor
// UI 統合 (workspace / theme / asset browser / panel orchestration) のため、
// 頂点+色 cube で十分 (CStandardShader / CPbrShader への差替は将来の宿題)。
//
// 24 頂点 (6 面 × 4 頂点) + 36 indices (6 面 × 2 三角形 × 3 頂点) を constexpr
// で持つ。HLSL は b0 に MVP を行優先で渡す最小シェーダ (VS+PS 兼用)。
#pragma once

#include "foundation/Types.h"

namespace hellomv {

// 1 頂点 = 位置 + 色 (FColor は面ごとに使い分け)。
struct FVertex {
    acs::f32 pos[3];
    acs::f32 col[3];
};

// 24 頂点 (6 面 × 4 頂点) で面ごとに色を変える。
inline constexpr FVertex kCubeVertices[24] = {
    // 前面 (-Z) 赤
    {{-1, -1, -1}, {1, 0, 0}}, {{ 1, -1, -1}, {1, 0, 0}},
    {{ 1,  1, -1}, {1, 0, 0}}, {{-1,  1, -1}, {1, 0, 0}},
    // 背面 (+Z) 緑
    {{ 1, -1,  1}, {0, 1, 0}}, {{-1, -1,  1}, {0, 1, 0}},
    {{-1,  1,  1}, {0, 1, 0}}, {{ 1,  1,  1}, {0, 1, 0}},
    // 左面 (-X) 青
    {{-1, -1,  1}, {0, 0, 1}}, {{-1, -1, -1}, {0, 0, 1}},
    {{-1,  1, -1}, {0, 0, 1}}, {{-1,  1,  1}, {0, 0, 1}},
    // 右面 (+X) 黄
    {{ 1, -1, -1}, {1, 1, 0}}, {{ 1, -1,  1}, {1, 1, 0}},
    {{ 1,  1,  1}, {1, 1, 0}}, {{ 1,  1, -1}, {1, 1, 0}},
    // 上面 (+Y) シアン
    {{-1,  1, -1}, {0, 1, 1}}, {{ 1,  1, -1}, {0, 1, 1}},
    {{ 1,  1,  1}, {0, 1, 1}}, {{-1,  1,  1}, {0, 1, 1}},
    // 下面 (-Y) マゼンタ
    {{-1, -1,  1}, {1, 0, 1}}, {{ 1, -1,  1}, {1, 0, 1}},
    {{ 1, -1, -1}, {1, 0, 1}}, {{-1, -1, -1}, {1, 0, 1}},
};

// 36 indices = 6 面 × 2 三角形 × 3 頂点。
inline constexpr acs::u16 kCubeIndices[36] = {
    0,  1,  2,    0,  2,  3,
    4,  5,  6,    4,  6,  7,
    8,  9, 10,    8, 10, 11,
   12, 13, 14,   12, 14, 15,
   16, 17, 18,   16, 18, 19,
   20, 21, 22,   20, 22, 23,
};

// HLSL: MVP は b0、行優先で受け取り。
inline constexpr const char* kHLSL = R"(
#pragma pack_matrix(row_major)
cbuffer Frame : register(b0) { float4x4 mvp; };
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

} // namespace hellomv
