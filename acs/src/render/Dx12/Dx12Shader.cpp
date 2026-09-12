// SPDX-License-Identifier: Apache-2.0
// 指定形式に応じて作成器を選び、命令の所有権を一つのblobへ統一する。
#include "foundation/Log.h"            // Win32 ヘッダ前に読む（マクロ衝突回避）
#include "memory/UniquePtr.h"
#include "render/Dx12/Dx12Shader.h"
#include "render/Dx12/Dx12Device.h"
#include "render/Dx12/Dx12DxcCompilerInternal.h"

namespace acs {

/** コンパイル済みバイトコード blob を解放する。 */
FDx12Shader::~FDx12Shader() noexcept {
    Reset();
}

void FDx12Shader::Reset() noexcept
{
    ACS_SAFE_RELEASE(m_Blob);
    m_Stage = EShaderStage::Vertex;
}

/** HLSLを指定した形式で作成する。失敗時は古い命令を残さない。 */
FHrResult FDx12Shader::Init(const FShaderDesc& desc) noexcept {
    FHrResult r{};
    Reset();

    if (!desc.hlsl_source || !desc.entry_point || desc.entry_point[0] == '\0') {
        r.hr = E_INVALIDARG;
        return r;
    }

    // ステージ → コンパイルターゲット文字列
    const char* target = desc.target;
    if (!target) {
        switch (desc.stage) {
            case EShaderStage::Vertex:  target = "vs_5_1"; break;
            case EShaderStage::Pixel:   target = "ps_5_1"; break;
            case EShaderStage::Compute: target = "cs_5_1"; break;
            default:
                break;
            }
    }
    /** 記述上の段階と実際の命令の段階を一致させる先頭文字。 */
    const char stage_prefix = desc.stage == EShaderStage::Vertex ? 'v' : desc.stage == EShaderStage::Pixel ? 'p' : desc.stage == EShaderStage::Compute ? 'c' : '\0';
    if (!stage_prefix || !target || target[0] != stage_prefix || target[1] != 's' || target[2] != '_') {
        r.hr = E_INVALIDARG;
        return r;
    }

    if (target[3] == '6') {
        if (target[4] != '_' || target[5] < '0' || target[5] > '9' || target[6] != '\0') {
            r.hr = E_INVALIDARG;
            return r;
        }
        r.hr = render_internal::CompileDxilShader_Internal(desc, target, m_Blob);
        if (r.IsOk()) m_Stage = desc.stage;
        return r;
    }

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if ACS_BUILD_DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    ID3DBlob* err_blob = nullptr;
    ID3DBlob* compiled_blob = nullptr;
    usize source_len = 0;
    while (desc.hlsl_source[source_len]) ++source_len;

    r.hr = ::D3DCompile(desc.hlsl_source, source_len, desc.debug_name, nullptr, nullptr, desc.entry_point, target,
                        flags, 0, &compiled_blob, &err_blob);

    if (r.IsErr() && err_blob) {
        // コンパイルエラーをログに出力（行番号付きメッセージ）
        ACS_LOG_ERROR("Shader compile failed (%s):\n%s", desc.debug_name ? desc.debug_name : "shader",
                      static_cast<const char*>(err_blob->GetBufferPointer()));
    }
    if (err_blob) err_blob->Release();
    if (r.IsErr() || !compiled_blob) {
        ACS_SAFE_RELEASE(compiled_blob);
        if (r.IsOk()) r.hr = E_FAIL;
        return r;
    }

    m_Blob = compiled_blob;
    m_Stage = desc.stage;
    return r;
}

#if !WITH_RENDER_DILIGENT
/**
 * DX12 用に IRhiShader を生成するファクトリ。
 *
 * @details
 * RTTI 無効のためバックエンド名で DX12 を判定し、Dx12Shader を構築して HLSL を
 * コンパイルする。Diligent バックエンド有効時は別実装が提供される。
 * @param device 生成元のデバイス (DX12 でなければエラー)。
 * @param desc コンパイルするシェーダの記述 (ソース・ステージ・エントリポイント等)。
 * @return 生成したシェーダを保持する TResult、判定・コンパイル失敗ならエラー。
 */
TResult<TUniquePtr<IRhiShader>> CreateRhiShader(IRhiDevice& device, const FShaderDesc& desc) noexcept {
    const char* bn = device.BackendName();
    if (!(bn[0] == 'D' && bn[1] == 'X' && bn[2] == '1' && bn[3] == '2'))
        return ACS_ERR(Render, 40, "CreateRhiShader: device is not DX12");
    auto s = MakeUnique<FDx12Shader>();
    const FHrResult r = s->Init(desc);
    if (r.IsErr())
        return ACS_ERR_OS(Render, 41, "Dx12Shader::Init failed (compile)", static_cast<u32>(r.hr));
    TUniquePtr<IRhiShader> base(s.Release(), s.GetAllocator());
    return TResult<TUniquePtr<IRhiShader>>(OkInit, Move(base));
}
#endif

} // namespace acs
