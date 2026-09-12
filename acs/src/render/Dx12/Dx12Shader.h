// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "render/IRhiShader.h"
#include "render/Dx12/Dx12Common.h"
#include "container/Array.h"

#include <d3dcompiler.h>

namespace acs {

/**
 * 指定した形式でHLSLを作成し、命令を単独所有するDX12版シェーダ。
 *
 * @details
 * target未指定時は従来の5.1を使い、明示した6.0〜6.9は同梱DXCで作成する。
 * 結果は検証後にID3DBlobへ格納し、DXCの一時所有者やDLLの寿命から切り離す。
 * Bytecode / BytecodeSize で blob の中身を返し、パイプライン作成時にバックエンドが
 * 内部参照する。IRhiShader を実装する final クラス。
 */
class FDx12Shader final : public IRhiShader {
public:
    /** 空状態で構築する (blob は Init でコンパイルして確保)。 */
    FDx12Shader() noexcept = default;

    /** 保持しているバイトコード blob を解放する。 */
    ~FDx12Shader() noexcept override;

    /**
     * HLSL ソースをコンパイルしてバイトコードを確保する。
     *
     * @details
     * desc.target が null なら desc.stage から vs_5_1 / ps_5_1 / cs_5_1 を自動選択する。
     * デバッグビルドでは最適化なし + デバッグ情報付き、リリースでは最適化レベル3 で
     * コンパイルする。明示SM6ではこの実装を収録したexeまたはDLLの横に
     * dxcompiler.dllとdxil.dllが必要。欠落・作成失敗・検証失敗はエラーを返す。
     * DXCと検証器のDLL本体は初回利用後からプロセス終了まで保持する。
     * ソースや関数名の欠落、記述と命令の段階不一致はE_INVALIDARGとなる。
     * 失敗時に以前の命令を残さず、別形式への暗黙の切替は行わない。
     * @param desc コンパイル対象の HLSL ソース・エントリポイント等の生成パラメータ。
     * @return 成功なら正常な FHrResult、失敗なら HRESULT を含むエラー。
     */
    FHrResult Init(const FShaderDesc& desc) noexcept;

    /**
     * このシェーダのステージ種別を返す。
     *
     * @return Init で確定したシェーダステージ。
     */
    EShaderStage Stage() const noexcept override { return m_Stage; }

    /**
     * コンパイル済みバイトコードの先頭ポインタを返す。
     *
     * @return バイトコード先頭へのポインタ (未コンパイルなら nullptr)。
     */
    const byte* Bytecode() const noexcept override {
        return m_Blob ? static_cast<const byte*>(m_Blob->GetBufferPointer()) : nullptr;
    }

    /**
     * コンパイル済みバイトコードのバイト数を返す。
     *
     * @return バイトコードのサイズ (バイト、未コンパイルなら 0)。
     */
    usize BytecodeSize() const noexcept override {
        return m_Blob ? m_Blob->GetBufferSize() : 0;
    }

private:
    /** コンパイル済み blob を解放し、空状態へ戻す。 */
    void Reset() noexcept;

    /** コンパイル済みバイトコードを保持する blob (単独所有)。 */
    ID3DBlob*    m_Blob  = nullptr;

    /** このシェーダのステージ種別。 */
    EShaderStage  m_Stage = EShaderStage::Vertex;
};

} // namespace acs
