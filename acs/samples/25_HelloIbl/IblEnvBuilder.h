// SPDX-License-Identifier: Apache-2.0
// HelloIbl — CPU 側 equirect 環境マップ生成。
//
// 二系統の equirect builder を提供する:
//   1) BuildEquirectFromSky: 現在の Sky procedural を CPU で評価して equirect float bitmap
//      に焼く。ProcSky (Ibl.cpp の HLSL) と同じ式を C++ で再現したもの。SH9 計算用。
//   2) BuildStudioHdrEquirect: 4 色の HDR パネル光源を equirect に合成 (Studio HDR preset)。
//      暗い背景 + 異なる方位の 4 灯で、metallic sphere に 4 色の反射が現れる。
//
// 出力は RGBA float (4 ch / pixel) の Array<f32>。サイズは IblTypes.h の
// kEquirectWidth * kEquirectHeight * 4。引数 buf は append ではなく resize される。
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"

namespace acs { class Sky; }

namespace helloibl {

// 現 Sky preset (Day / Sunset / Night) の procedural を CPU で評価して
// equirect HDR float buffer に焼く。SH9 計算で使う。
void BuildEquirectFromSky(const acs::Sky& sky, acs::Array<acs::f32>& buf) noexcept;

// 4 灯 Studio HDR equirect を合成 (Studio preset、Sky 評価は不要)。
void BuildStudioHdrEquirect(acs::Array<acs::f32>& buf) noexcept;

} // namespace helloibl
