// Screen-Space Ambient Occlusion (Phase 34j)
//
// HBAO-lite: depth-only horizon-based AO。各 pixel から N 方向に screen-space で
// march して horizon angle を求め、ambient occlusion を出す。
//
// 出力は R8G8B8A8_UNorm の RT、.r channel に visibility (1=完全照明、0=完全遮蔽)。
// PostProcess の tonemap 入力に bind し、hdr_col に乗算合成する。
//
// 制限:
//   - depth-only (G-buffer normal なし)、depth-derivative で screen-space normal を再構成
//   - 単純な uniform random direction、Halton 等の low-discrepancy 未使用
//   - blur pass なし (フル解像度で計算、ノイズは少なめ)
//   - 4 direction × 4 step = 16 sample / pixel、UE5 比だと粗いが educational 用途で十分
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "math/Mat.h"
#include "math/Vec.h"
#include "render/IRhiDevice.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiTexture.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiShader.h"
#include "render/IRhiBuffer.h"

namespace acs {

class Ssao {
public:
    Ssao() noexcept = default;
    ~Ssao() noexcept = default;

    Ssao(const Ssao&) = delete;
    Ssao& operator=(const Ssao&) = delete;

    Result<void> Init(IRhiDevice& device, u32 width, u32 height) noexcept;
    void Shutdown() noexcept;
    Result<void> Resize(u32 width, u32 height) noexcept;

    // SSAO を計算して内部 RT に書く。
    //   scene_depth: shader_visible_depth=true な depth buffer
    //   inv_view_proj: 現フレーム VP の逆 (depth+uv → world)
    //   eye: カメラ world pos
    //   intensity: 遮蔽強度 (0=AO 無効、1=neutral、>1=過度に暗く)
    //   radius:    AO の最大半径 (world units、0.5 ~ 2.0 が典型)
    void Render(IRhiDevice& device, IRhiCommandList& cl,
                IRhiTexture& scene_depth,
                const Mat4& view_proj,
                const Mat4& inv_view_proj,
                Vec3 eye,
                f32 intensity = 1.0f,
                f32 radius    = 0.5f) noexcept;

    IRhiTexture* OutputTexture() const noexcept { return _output.Get(); }

private:
    Result<void> CreateOutputRT(IRhiDevice& device, u32 w, u32 h) noexcept;
    Result<void> CreatePipeline(IRhiDevice& device) noexcept;

    IRhiDevice*             _device = nullptr;
    u32                     _width  = 0;
    u32                     _height = 0;

    UniquePtr<IRhiTexture>  _output;
    UniquePtr<IRhiShader>   _vs;
    UniquePtr<IRhiShader>   _ps;
    UniquePtr<IRhiPipeline> _pipeline;
    UniquePtr<IRhiBuffer>   _cb;
};

} // namespace acs
