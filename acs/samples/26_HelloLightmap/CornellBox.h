// SPDX-License-Identifier: Apache-2.0
// HelloLightmap — Cornell box シーンの構築。
//
// 5 面 (床 / 天井 / 奥壁 / 左壁(赤) / 右壁(緑)) を InitQuad で組み立てる。
// 法線・軸並行平面パラメータはローカル平面 (+Y 法線) を model 行列で変換して
// 導出するので、回転と軸の取り違えが発生しない。
#pragma once

#include "LightmapTypes.h"

namespace acs { class IRhiDevice; }

namespace hellolightmap {

// 1 面を初期化 (mesh upload + 平面パラメータ設定)。
// world 法線は手書きせず、ローカル +Y を model で変換して導出する。
void InitQuad(acs::IRhiDevice& dev, FQuad& q, acs::f32 w, acs::f32 h,
              const acs::FMat4& model, acs::FVec3 albedo,
              acs::i32 axis, acs::f32 axis_value,
              acs::f32 u_min, acs::f32 u_max,
              acs::f32 v_min, acs::f32 v_max,
              bool emissive) noexcept;

// Cornell box 5 面 (床/天井/奥/左/右) を quads に組み立てる。
// 天井 (quads[1]) のみ emissive=true で光源として扱う。
void BuildCornellBox(acs::IRhiDevice& dev, FQuad (&quads)[kQuadCount]) noexcept;

} // namespace hellolightmap
