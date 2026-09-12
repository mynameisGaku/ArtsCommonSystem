// SPDX-License-Identifier: Apache-2.0
#include "render/Dx12/Dx12ShaderModelInternal.h"
#include "render/IRhiShader.h"
#include <dxgi.h>

namespace acs {

namespace {

// 呼出し元が4バイトの範囲を検査済みの位置から読む。先頭や部品の整列を要求しない。
u32 ReadUint32(const byte* data) noexcept
{
    return static_cast<u32>(data[0]) | (static_cast<u32>(data[1]) << 8u) | (static_cast<u32>(data[2]) << 16u) | (static_cast<u32>(data[3]) << 24u);
}

// 公式DxilProgramHeaderの範囲と段階を検査して要求SMを返す。LLVM本体の検証は行わない。
// https://github.com/microsoft/DirectXShaderCompiler/blob/main/include/dxc/DxilContainer/DxilContainer.h
// https://github.com/microsoft/DirectXShaderCompiler/blob/main/include/dxc/DXIL/DxilConstants.h
HRESULT ReadProgramModel(const byte* data, usize size, EShaderStage stage, u32& required) noexcept
{
    if (size < 24u) return E_INVALIDARG;
    // プログラム全長はヘッダーを含む32ビット語数。乗算より先に上限を調べる。
    const u32 words = ReadUint32(data + 4u);
    if (words < 6u || words > size / 4u) return E_INVALIDARG;
    // ビットコードヘッダー先頭からプログラム末尾までの可読長。
    const usize bitcode_span = static_cast<usize>(words) * 4u - 8u;
    // LLVM本体の位置はプログラム先頭ではなくビットコードヘッダー先頭が基準。
    const u32 bitcode_offset = ReadUint32(data + 16u);
    // LLVM本体のバイト数。零長や末尾越えを許さない。
    const u32 bitcode_size = ReadUint32(data + 20u);
    if (ReadUint32(data + 8u) != 0x4c495844u || bitcode_offset < 16u || bitcode_offset > bitcode_span || bitcode_size == 0u || bitcode_size > bitcode_span - bitcode_offset) return E_INVALIDARG;

    // ProgramVersionの上位16ビットが段階、下位8ビットがSM。DxilVersionとは別の値。
    const u32 version = ReadUint32(data);
    // 既存IRhiShaderが申告する段階に対応する公式ShaderKind値。
    u32 expected_kind = 0u;
    switch (stage) {
        case EShaderStage::Pixel: expected_kind = 0u; break;
        case EShaderStage::Vertex: expected_kind = 1u; break;
        case EShaderStage::Compute: expected_kind = 5u; break;
        default: return E_INVALIDARG;
    }
    if ((version >> 16u) != expected_kind || (version & 0x0000ff00u) != 0u) return E_INVALIDARG;
    // DXILとしてSM5以下を受け入れない。将来の主版はこの限定実装では未対応とする。
    const u32 major = (version >> 4u) & 0xfu;
    if (major < 6u) return E_INVALIDARG;
    if (major != 6u) return DXGI_ERROR_UNSUPPORTED;
    required = version & 0xffu;
    return S_OK;
}

// DXBC外枠の全範囲を検査し、本体DXIL部品があるときだけSMを返す。
// 連続配置と最終位置の根拠: 公式IsValidDxilContainer。差分比較で加算の桁あふれを避ける。
// https://github.com/microsoft/DirectXShaderCompiler/blob/main/lib/DxilContainer/DxilContainer.cpp
HRESULT ReadShaderModel(const IRhiShader& shader, u32& required) noexcept
{
    required = 0u;
    // IRhiShaderから借用するバイト列。FDx12Shaderへの変換は不要。
    const byte* data = shader.Bytecode();
    // 宣言長ではなく実際に渡された可読長。
    const usize size = shader.BytecodeSize();
    if (!data || size < 32u) return E_INVALIDARG;
    if (ReadUint32(data) != 0x43425844u || (ReadUint32(data + 20u) & 0xffffu) != 1u) return E_INVALIDARG;
    // DXBC外枠が宣言する長さ。公式上限と実際の可読長を両方確認する。
    const usize container_size = ReadUint32(data + 24u);
    if (container_size < 32u || container_size > 0x80000000u || container_size > size) return E_INVALIDARG;
    // オフセット表の項目数。表の乗算や読み出しより先に検査する。
    const u32 part_count = ReadUint32(data + 28u);
    if (part_count > (container_size - 32u) / 4u) return E_INVALIDARG;
    // 次の部品は表の直後から順に隙間なく置かれる。
    usize next_part = 32u + static_cast<usize>(part_count) * 4u;
    // 本体の重複は要求版の大小にかかわらず拒否する。
    bool found_dxil = false;
    // 部品表全体を調べ、DXIL以後の破損も照会前に拒否する。
    for (u32 i = 0u; i < part_count; ++i) {
        // 部品の先頭位置。表・他部品との重なりや隙間もここで検査する。
        const usize offset = ReadUint32(data + 32u + static_cast<usize>(i) * 4u);
        if (offset != next_part || offset > container_size || container_size - offset < 8u) return E_INVALIDARG;
        // 部品のヘッダーを除いた長さ。
        const usize part_size = ReadUint32(data + offset + 4u);
        if (part_size > container_size - offset - 8u) return E_INVALIDARG;
        if (ReadUint32(data + offset) == 0x4c495844u) {
            if (found_dxil) return E_INVALIDARG;
            found_dxil = true;
            // 本体ProgramVersionを検査した結果。失敗時はそのHRESULTを保持する。
            const HRESULT result = ReadProgramModel(data + offset + 8u, part_size, shader.Stage(), required);
            if (FAILED(result)) return result;
        }
        next_part = offset + 8u + part_size;
    }
    if (next_part != container_size) return E_INVALIDARG;
    // DXILを持たない旧バイトコードは照会を省略する。実行可能性の検証は既存PSOに残す。
    return S_OK;
}

} // namespace

// 全入力を先に検査し、必要な最大SMに対して一度だけ照会する。失敗時は再試行しない。
HRESULT CheckDx12ShaderModel_Internal(const IRhiShader& primary, const IRhiShader* pixel, Dx12ShaderModelQuery query, void* context) noexcept
{
    // 主シェーダーから始める最大要求版。零は照会不要を表す。
    u32 required = 0u;
    // 解析または機能照会のHRESULTをそのまま呼出し元へ渡す。
    HRESULT result = ReadShaderModel(primary, required);
    if (FAILED(result)) return result;
    if (pixel) {
        // 省略可能なPSの要求版。
        u32 pixel_required = 0u;
        result = ReadShaderModel(*pixel, pixel_required);
        if (FAILED(result)) return result;
        if (pixel_required > required) required = pixel_required;
    }
    if (required == 0u) return S_OK;
    if (!query) return E_INVALIDARG;
    // 入力に必要版、出力にその版以下の対応上限を受ける。低い版への再試行は行わない。
    // https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_shader_model
    D3D12_FEATURE_DATA_SHADER_MODEL model{static_cast<D3D_SHADER_MODEL>(required)};
    result = query(context, model);
    if (FAILED(result)) return result;
    if (static_cast<u32>(model.HighestShaderModel) < required) return DXGI_ERROR_UNSUPPORTED;
    return result;
}

} // namespace acs
