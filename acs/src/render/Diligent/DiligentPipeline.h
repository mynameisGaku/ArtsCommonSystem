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
    Diligent::IPipelineState*           Native() const noexcept { return m_Pso; }
    Diligent::IShaderResourceBinding*   Srb()    const noexcept { return m_Srb; }

    u32 CbufferSlots() const noexcept { return m_CbSlots; }
    u32 TextureSlots() const noexcept { return m_TexSlots; }

    // 名前ルックアップ用（CommandList::SetConstantBuffer/SetTexture で使う）
    // 戻り値は「cb0/t0」形式の固定バッファ or FPipelineDesc で渡された名前
    const char* CbufferName(u32 slot) const noexcept;
    const char* TextureName(u32 slot) const noexcept;

private:
    DiligentDevice*                  m_Device = nullptr;
    Diligent::IPipelineState*        m_Pso    = nullptr;
    Diligent::IShaderResourceBinding* m_Srb   = nullptr;
    u32                              m_CbSlots  = 0;
    u32                              m_TexSlots = 0;

    // FPipelineDesc から複製した HLSL リソース名。null なら fallback 名を返す。
    // Phase 33f-prep: 8→16 へ拡張、FPbrShader が 8/8 で上限到達したため。
    static constexpr u32 kMaxResourceSlots = 16;
    const char* m_CbNames[kMaxResourceSlots]  = {};
    const char* m_TexNames[kMaxResourceSlots] = {};

    // フォールバック名 ("cb0".."cb15", "t0".."t15") を返す
    static const char* FallbackCbName(u32 slot) noexcept;
    static const char* FallbackTexName(u32 slot) noexcept;
};

} // namespace acs
