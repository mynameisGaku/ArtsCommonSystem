// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <d3d12.h>

namespace acs {

// コンパイル済みバイト列を参照する既存インタフェース。
class IRhiShader;

// 要求版を入力し対応版を返す内部照会。context は呼出し中だけ借用する。
using Dx12ShaderModelQuery = HRESULT (*)(void* context, D3D12_FEATURE_DATA_SHADER_MODEL& model) noexcept;

// 主シェーダーと省略可能なPSの最大要求SMを照会する。バイト列は呼出し中不変とする。
// 破損はE_INVALIDARG、対応版不足はDXGI_ERROR_UNSUPPORTED、照会失敗は元のHRESULTを返す。
// SM5は照会しない。署名・命令・追加機能の検証は既存のPSO作成に残す。
HRESULT CheckDx12ShaderModel_Internal(const IRhiShader& primary, const IRhiShader* pixel, Dx12ShaderModelQuery query, void* context) noexcept;

} // namespace acs
