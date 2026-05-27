// SPDX-License-Identifier: Apache-2.0
// Diligent Engine 経由のパイプライン
#pragma once

#include "render/IRhiPipeline.h"
#include "memory/UniquePtr.h"

namespace Diligent {
    struct IPipelineState;
    struct IShaderResourceBinding;
}

namespace acs {

class DiligentDevice;

class DiligentPipeline final : public IRhiPipeline {
public:
    DiligentPipeline() noexcept = default;
    ~DiligentPipeline() noexcept override;

    DiligentPipeline(const DiligentPipeline&) = delete;
    DiligentPipeline& operator=(const DiligentPipeline&) = delete;

    TResult<void> Init(DiligentDevice& device, const FPipelineDesc& desc) noexcept;

    // 内部公開
    Diligent::IPipelineState*           Native() const noexcept { return _pso; }
    Diligent::IShaderResourceBinding*   Srb()    const noexcept { return _srb; }

    u32 CbufferSlots() const noexcept { return _cb_slots; }
    u32 TextureSlots() const noexcept { return _tex_slots; }

    // 名前ルックアップ用（CommandList::SetConstantBuffer/SetTexture で使う）
    // 戻り値は「cb0/t0」形式の固定バッファ or FPipelineDesc で渡された名前
    const char* CbufferName(u32 slot) const noexcept;
    const char* TextureName(u32 slot) const noexcept;

private:
    DiligentDevice*                  _device = nullptr;
    Diligent::IPipelineState*        _pso    = nullptr;
    Diligent::IShaderResourceBinding* _srb   = nullptr;
    u32                              _cb_slots  = 0;
    u32                              _tex_slots = 0;

    // FPipelineDesc から複製した HLSL リソース名。null なら fallback 名を返す。
    // Phase 33f-prep: 8→16 へ拡張、FPbrShader が 8/8 で上限到達したため。
    static constexpr u32 kMaxResourceSlots = 16;
    const char* _cb_names[kMaxResourceSlots]  = {};
    const char* _tex_names[kMaxResourceSlots] = {};

    // フォールバック名 ("cb0".."cb15", "t0".."t15") を返す
    static const char* FallbackCbName(u32 slot) noexcept;
    static const char* FallbackTexName(u32 slot) noexcept;
};

} // namespace acs
