// SPDX-License-Identifier: Apache-2.0
// Diligent Engine 経由のコマンドリスト
// Diligent は IDeviceContext 自体がコマンドキューを持つ即時 API なので、
// ACS の CommandList 概念は IDeviceContext の薄いラッパとして実装する。
#pragma once

#include "render/IRhiCommandList.h"
#include "memory/UniquePtr.h"

namespace acs {

class DiligentDevice;
class DiligentPipeline;
class DiligentSwapchain;
class DiligentTexture;

class DiligentCommandList final : public IRhiCommandList {
public:
    DiligentCommandList() noexcept = default;
    ~DiligentCommandList() noexcept override;

    DiligentCommandList(const DiligentCommandList&) = delete;
    DiligentCommandList& operator=(const DiligentCommandList&) = delete;

    TResult<void> Init(DiligentDevice& device) noexcept;

    // ---- IRhiCommandList ----
    void Begin() noexcept override;
    void End() noexcept override;
    void Submit() noexcept override;

    void BeginRenderToSwapchain(IRhiSwapchain& sc, u32 buffer_index,
                                const ClearColor& clear,
                                IRhiTexture* depth = nullptr,
                                f32 depth_clear = 1.0f) noexcept override;
    void EndRenderToSwapchain(IRhiSwapchain& sc, u32 buffer_index) noexcept override;

    void BeginShadowPass(IRhiTexture& depth, f32 depth_clear = 1.0f) noexcept override;
    void EndShadowPass(IRhiTexture& depth) noexcept override;

    void BeginRenderToTexture(IRhiTexture& rt, const ClearColor& clear,
                              IRhiTexture* depth = nullptr,
                              f32 depth_clear = 1.0f) noexcept override;
    void EndRenderToTexture(IRhiTexture& rt) noexcept override;
    void BeginRenderToTextureLoad(IRhiTexture& rt,
                                   IRhiTexture* depth = nullptr) noexcept override;
    void BeginRenderToTextureSlice(IRhiTexture& rt, u32 slice, u32 mip,
                                    const ClearColor& clear) noexcept override;
    void BeginRenderToTextureMrt(IRhiTexture* const* rts, u32 rt_count,
                                  const ClearColor& clear,
                                  IRhiTexture* depth = nullptr,
                                  f32 depth_clear = 1.0f) noexcept override;

    void SetViewport(const FViewport& vp) noexcept override;
    void SetScissor(const FScissorRect& sr) noexcept override;

    void SetPipeline(IRhiPipeline& pipeline) noexcept override;
    void SetVertexBuffer(IRhiBuffer& vb, u32 stride) noexcept override;
    void SetIndexBuffer(IRhiBuffer& ib) noexcept override;
    void SetConstantBuffer(u32 slot, IRhiBuffer& cb) noexcept override;
    void SetTexture(u32 slot, IRhiTexture& tex) noexcept override;

    void Draw(u32 vertex_count, u32 first_vertex = 0) noexcept override;
    void DrawIndexed(u32 index_count, u32 first_index = 0, i32 base_vertex = 0) noexcept override;

    void* NativeHandle() noexcept override;

private:
    DiligentDevice*    _device   = nullptr;
    DiligentPipeline*  _pipeline = nullptr;
    bool               _is_index32 = false;
    // フレーム内 main pass の RT を記憶する: shadow / off-screen pass を
    // 途中で挟んでも EndShadowPass / EndRenderToTexture で復帰させるため。
    DiligentSwapchain* _main_swapchain     = nullptr;
    DiligentTexture*   _main_depth         = nullptr;
};

} // namespace acs
