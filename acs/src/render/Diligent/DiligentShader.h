// SPDX-License-Identifier: Apache-2.0
// Diligent Engine 経由のシェーダ
//
// 注: Diligent は内部で HLSL → SPIRV/MSL/GLSL に変換するため、
// Bytecode() は便宜的に nullptr を返す（Pipeline 作成時には IShader* を直接使う）。
#pragma once

#include "render/IRhiShader.h"
#include "memory/UniquePtr.h"

namespace Diligent {
    struct IShader;
}

namespace acs {

class DiligentDevice;

class DiligentShader final : public IRhiShader {
public:
    DiligentShader() noexcept = default;
    ~DiligentShader() noexcept override;

    DiligentShader(const DiligentShader&) = delete;
    DiligentShader& operator=(const DiligentShader&) = delete;

    TResult<void> Init(DiligentDevice& device, const FShaderDesc& desc) noexcept;

    // ---- IRhiShader ----
    EShaderStage Stage()         const noexcept override { return m_Stage; }
    const byte* Bytecode()      const noexcept override { return nullptr; }
    usize       BytecodeSize()  const noexcept override { return 0; }

    // 内部公開
    Diligent::IShader* Native() const noexcept { return m_Shader; }

    // HLSL source を parse して取得した register binding。slot は b0/t0/s0
    // 等の数字。array index は ACS の slot 番号と一致する想定 (上限 16)。
    // 取得できなかった slot は空文字列。
    static constexpr u32 kMaxSlots    = 16;     // Phase 33f-prep: 8→16
    static constexpr u32 kMaxNameLen  = 64;
    const char* CbufferNameAt(u32 slot) const noexcept {
        return slot < kMaxSlots && m_CbNames[slot][0] ? m_CbNames[slot] : nullptr;
    }
    const char* TextureNameAt(u32 slot) const noexcept {
        return slot < kMaxSlots && m_TexNames[slot][0] ? m_TexNames[slot] : nullptr;
    }

private:
    DiligentDevice*    m_Device = nullptr;
    Diligent::IShader* m_Shader = nullptr;
    EShaderStage        m_Stage  = EShaderStage::Vertex;
    // HLSL source 内の `cbuffer X : register(bN)` / `Texture2D Y : register(tN)`
    // から取得した N → 名前のマッピング (Diligent::ShaderResourceDesc に
    // BindPoint が無いため source parse で代用)。
    char m_CbNames [kMaxSlots][kMaxNameLen] = {};
    char m_TexNames[kMaxSlots][kMaxNameLen] = {};
};

} // namespace acs
