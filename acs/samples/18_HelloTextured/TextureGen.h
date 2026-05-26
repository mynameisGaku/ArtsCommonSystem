// SPDX-License-Identifier: Apache-2.0
// HelloTextured — 手続き生成テクスチャの宣言。
//
// kTexSize x kTexSize の RGBA8 ピクセルを dst に書き込む。
// dst は kTexSize*kTexSize*4 bytes の領域を指している必要がある。
#pragma once

#include "foundation/Types.h"

namespace hellotextured {

// 64x64 のチェッカーボード + 中央に山吹色の円 (月) を生成。
void GenerateTexture(acs::u8* dst) noexcept;

} // namespace hellotextured
