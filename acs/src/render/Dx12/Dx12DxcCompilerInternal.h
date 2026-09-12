// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "render/Dx12/Dx12Shader.h"

namespace acs::render_internal {

/** モジュール横のDXCと検証器で明示SM6を作成する。失敗時は空の出力を保持する。 */
HRESULT CompileDxilShader_Internal(const FShaderDesc& desc, const char* target, ID3DBlob*& output) noexcept;

} // namespace acs::render_internal
