// SPDX-License-Identifier: Apache-2.0
// HelloShowcase — HUD overlay (タイトル + 状態 + キーガイド)。
// FPostProcess::Render が bind した swapchain RT の上にそのまま描く。
#pragma once

#include "ShowcaseAssets.h"

namespace acs { class IRhiCommandList; class IRhiSwapchain; }

namespace helloshowcase {

void ExecuteHudPass(Assets& a,
                     acs::IRhiCommandList& cl,
                     acs::IRhiSwapchain& sc,
                     bool paused,
                     bool show_ssr,
                     bool show_refraction) noexcept;

} // namespace helloshowcase
