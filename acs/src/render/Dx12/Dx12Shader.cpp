// SPDX-License-Identifier: Apache-2.0
// DX12 シェーダ実装（D3DCompile による HLSL コンパイル）
#include "foundation/Log.h"            // Win32 ヘッダ前に読む（マクロ衝突回避）
#include "memory/UniquePtr.h"
#include "render/Dx12/Dx12Shader.h"
#include "render/Dx12/Dx12Device.h"

namespace acs {

Dx12Shader::~Dx12Shader() noexcept {
    ACS_SAFE_RELEASE(_blob);
}

HrResult Dx12Shader::Init(Dx12Device& /*device*/, const ShaderDesc& desc) noexcept {
    HrResult r{};
    _stage = desc.stage;

    if (!desc.hlsl_source) { r.hr = E_INVALIDARG; return r; }

    // ステージ → コンパイルターゲット文字列
    const char* target = desc.target;
    if (!target) {
        switch (desc.stage) {
            case ShaderStage::Vertex:  target = "vs_5_1"; break;
            case ShaderStage::Pixel:   target = "ps_5_1"; break;
            case ShaderStage::Compute: target = "cs_5_1"; break;
        }
    }

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if ACS_BUILD_DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    ID3DBlob* err_blob = nullptr;
    usize source_len = 0;
    while (desc.hlsl_source[source_len]) ++source_len;

    r.hr = ::D3DCompile(
        desc.hlsl_source, source_len, desc.debug_name,
        nullptr, nullptr,
        desc.entry_point, target, flags, 0,
        &_blob, &err_blob);

    if (r.IsErr() && err_blob) {
        // コンパイルエラーをログに出力（行番号付きメッセージ）
        ACS_LOG_ERROR("Shader compile failed (%s):\n%s",
                      desc.debug_name,
                      static_cast<const char*>(err_blob->GetBufferPointer()));
    }
    if (err_blob) err_blob->Release();
    return r;
}

// ファクトリ
#if !WITH_RENDER_DILIGENT
Result<UniquePtr<IRhiShader>> CreateRhiShader(IRhiDevice& device, const ShaderDesc& desc) noexcept {
    const char* bn = device.BackendName();
    if (!(bn[0] == 'D' && bn[1] == 'X' && bn[2] == '1' && bn[3] == '2'))
        return ACS_ERR(Render, 40, "CreateRhiShader: device is not DX12");
    Dx12Device* dxd = static_cast<Dx12Device*>(&device);
    auto s = MakeUnique<Dx12Shader>();
    HrResult r = s->Init(*dxd, desc);
    if (r.IsErr())
        return ACS_ERR_OS(Render, 41, "Dx12Shader::Init failed (compile)", static_cast<u32>(r.hr));
    UniquePtr<IRhiShader> base(s.Release(), s.GetAllocator());
    return Result<UniquePtr<IRhiShader>>(OkInit, Move(base));
}
#endif

} // namespace acs
