// SPDX-License-Identifier: Apache-2.0
// シェーダ抽象（HLSL ソースをコンパイルして保持）
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "container/Array.h"

namespace acs {

class IRhiDevice;

// シェーダステージ
enum class EShaderStage : u8 {
    Vertex,
    Pixel,
    Compute,
};

struct ShaderDesc {
    EShaderStage stage     = EShaderStage::Vertex;
    const char* hlsl_source = nullptr;     // HLSL ソース文字列
    const char* entry_point = "main";
    const char* target      = nullptr;     // 例: "vs_5_1" / "ps_5_1"。null なら stage から自動
    const char* debug_name  = "shader";
};

class IRhiShader {
public:
    virtual ~IRhiShader() noexcept = default;

    virtual EShaderStage Stage() const noexcept = 0;

    // コンパイル済みバイトコード（パイプライン作成時にバックエンドが内部参照）
    virtual const byte* Bytecode() const noexcept = 0;
    virtual usize       BytecodeSize() const noexcept = 0;
};

TResult<TUniquePtr<IRhiShader>> CreateRhiShader(IRhiDevice& device, const ShaderDesc& desc) noexcept;

} // namespace acs
