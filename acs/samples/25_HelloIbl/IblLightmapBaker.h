// SPDX-License-Identifier: Apache-2.0
// HelloIbl — 床用 lightmap CPU baker (Phase 33f)。
//
// 球グリッドの直下にある床用に静的 AO を CPU 焼き。
// 球 5x5 を Sphere-Ray analytical hit で覆い、各 plane texel で
// 8 方向の hemisphere sampling して visibility を求める。簡易だが
// 「スフィア真下に影 + 開けた所の明るい indirect」が出る。
//
// 戻り値: 焼いた lightmap を R8G8B8A8_UNorm の IRhiTexture として生成。
//         失敗時は Result<void>::Err を返す。
#pragma once

#include "memory/UniquePtr.h"
#include "foundation/Result.h"

namespace acs {
class IRhiDevice;
class IRhiTexture;
}

namespace helloibl {

acs::Result<acs::UniquePtr<acs::IRhiTexture>>
BakeFloorLightmap(acs::IRhiDevice& dev) noexcept;

} // namespace helloibl
