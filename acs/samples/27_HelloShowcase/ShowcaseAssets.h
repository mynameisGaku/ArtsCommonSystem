// SPDX-License-Identifier: Apache-2.0
// HelloShowcase — GPU resource 一式 (Assets) と初期化 / 解放ヘルパ。
//
// PostProcess / Sky / IBL / PBR / SSR / SSAO / HiZ / Motion / Refraction / Blit /
// SpriteBatch / Font / 球 + 床メッシュ / 屈折 background RT を 1 つの struct に
// まとめる。pass 系 helper (PbrPass / RefractionPass / SsrPass / ...) はこの
// Assets を const & または & で受け取って動く free function 群。
//
// ShowcaseApp は Assets を 1 メンバとして保持し、OnStart で Initialize() を
// 1 行で呼ぶ。Initialize() は失敗時に TResult<void> を返すので、呼び出し側で
// ACS_SAMPLE_INIT(...) を被せれば失敗時に自動 Quit() できる。
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"

#include "render/Blit.h"
#include "render/Font.h"
#include "render/HiZ.h"
#include "render/Ibl.h"
#include "render/IRhiTexture.h"
#include "render/MotionVector.h"
#include "render/PbrShader.h"
#include "render/PostProcess.h"
#include "render/RefractionShader.h"
#include "render/RenderAssets.h"
#include "render/RhiTypes.h"
#include "render/Sky.h"
#include "render/SpriteBatch.h"
#include "render/Ssao.h"
#include "render/Ssr.h"

namespace acs { class IRhiDevice; }

namespace helloshowcase {

// 描画に必要な GPU resource をひとまとめにしたもの。
// pass helper はこの参照を受け取って働き、内部状態は持たない。
struct Assets {
    acs::PostProcess        post;
    acs::ImageBasedLighting ibl;
    acs::Sky                sky;
    acs::PbrShader          pbr;
    acs::Ssr                ssr;
    acs::Ssao               ssao;
    acs::HiZ                hiz;            // SSR の skip-ahead 用 coarse min-depth
    acs::MotionVector       motion;
    acs::RefractionShader   refr;
    acs::Blit               blit;
    acs::TUniquePtr<acs::IRhiTexture> bg_rt;     // 屈折 pass が読む opaque シーンの複製先
    acs::GpuMesh            gm_sphere;
    acs::GpuMesh            gm_floor;
    acs::SpriteBatch        batch;
    acs::Font               font;
};

// すべての resource を一括で初期化する。HDR RT + Bloom + ACES tonemap を有効化し、
// Sky を Day preset に、sphere/plane メッシュを生成し、SSR/SSAO/HiZ/Motion/
// Refraction/Blit/SpriteBatch を順次セットアップする。
//
//   color_fmt / depth_fmt : swapchain の format (PostProcess / Sky / Refraction に渡す)
//
// 1 つでも失敗すると IsErr が返り、後段の resource は未初期化のまま残る
// (Shutdown は安全に呼べる)。
acs::TResult<void> InitializeAssets(Assets& assets,
                                    acs::IRhiDevice& device,
                                    acs::u32 width, acs::u32 height,
                                    acs::EFormat color_fmt,
                                    acs::EFormat depth_fmt) noexcept;

// 全 resource を解放する (順序は依存関係に従う)。
// device.WaitIdle() は呼び出し側で先に行う想定。
void ShutdownAssets(Assets& assets) noexcept;

} // namespace helloshowcase
