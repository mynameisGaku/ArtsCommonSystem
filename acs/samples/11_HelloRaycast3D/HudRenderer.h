// SPDX-License-Identifier: Apache-2.0
// HelloRaycast3D — 2D HUD (クロスヘア + 当たり情報) 専用の描画ヘルパ。
//
// 分離した理由: 3D 描画と HUD は完全に独立した処理 (Pipeline / 入力データ / 座標系)
// なので、混ぜると 1 関数が大きくなって読みにくくなる。
#pragma once

#include "foundation/Types.h"

namespace acs {
    class IRhiCommandList;
    class SpriteBatch;
    class Font;
}

namespace helloraycast3d {

class RaycastTargets;

class HudRenderer {
public:
    // font が未ロード (AtlasTexture が無い) の場合は何もしない。
    static void Draw(acs::SpriteBatch& batch,
                     acs::Font& font,
                     acs::IRhiCommandList& cl,
                     acs::u32 screen_w,
                     acs::u32 screen_h,
                     const RaycastTargets& targets) noexcept;
};

} // namespace helloraycast3d
