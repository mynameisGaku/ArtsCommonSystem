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

/**
 * Diligent Engine 経由で生成する HLSL シェーダ (IRhiShader 実装)。
 *
 * @details
 * Diligent が HLSL を内部で SPIRV/MSL/GLSL に変換するため bytecode 取得は不可で、
 * Bytecode() は nullptr を返し PSO 作成時は Native() の IShader* を直接渡す。
 * Diligent の ShaderResourceDesc には register の BindPoint が無いため、Init で
 * HLSL source を簡易 parse して `cbuffer/Texture2D : register(bN/tN)` の slot→名前
 * マッピングを構築し、PSO のリソース紐付けに供給する。
 */
class DiligentShader final : public IRhiShader {
public:
    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    DiligentShader() noexcept = default;

    /** Diligent シェーダオブジェクトを解放する。 */
    ~DiligentShader() noexcept override;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    DiligentShader(const DiligentShader&) = delete;

    /** コピー代入も禁止。 */
    DiligentShader& operator=(const DiligentShader&) = delete;

    /**
     * HLSL source をコンパイルしてシェーダを生成する。
     *
     * @details
     * FXC (SM 5.1) でコンパイルし、UseCombinedTextureSamplers で sampler を
     * `<texture>m_Sampler` 名に自動紐付けする。生成後に HLSL source を parse して
     * register binding の slot→名前マッピングを構築する。
     * @param device シェーダ生成に使う Diligent デバイス。
     * @param desc シェーダ記述 (HLSL source・エントリポイント・ステージ等)。
     * @return 成功なら空の TResult、device 未初期化・source 欠落・コンパイル失敗ならエラー。
     */
    TResult<void> Init(DiligentDevice& device, const FShaderDesc& desc,
                       bool combined_samplers = true) noexcept;

    /**
     * シェーダステージを返す。
     *
     * @return Vertex/Pixel 等のシェーダステージ。
     */
    EShaderStage Stage()         const noexcept override { return m_Stage; }

    /**
     * バイトコード先頭ポインタを返す (Diligent では常に nullptr)。
     *
     * @return 常に nullptr (Diligent はバイトコードを公開しない)。
     */
    const byte* Bytecode()      const noexcept override { return nullptr; }

    /**
     * バイトコードのバイト数を返す (Diligent では常に 0)。
     *
     * @return 常に 0 (Diligent はバイトコードを公開しない)。
     */
    usize       BytecodeSize()  const noexcept override { return 0; }

    /**
     * Diligent ネイティブのシェーダオブジェクトを返す (PSO 作成に直接使う)。
     *
     * @return Diligent::IShader へのポインタ (未初期化なら nullptr)。
     */
    Diligent::IShader* Native() const noexcept { return m_Shader; }

    /** バインディング配列の最大 slot 数 (b0..b15 / t0..t15)。 */
    static constexpr u32 kMaxSlots    = 16;

    /** リソース名の最大バイト長 (終端含む)。 */
    static constexpr u32 kMaxNameLen  = 64;

    /**
     * 指定 slot に割り当てられた cbuffer の名前を返す。
     *
     * @param slot register(bN) の N (0..kMaxSlots-1)。
     * @return 該当 cbuffer 名 (未割り当て・範囲外なら nullptr)。
     */
    const char* CbufferNameAt(u32 slot) const noexcept {
        return slot < kMaxSlots && m_CbNames[slot][0] ? m_CbNames[slot] : nullptr;
    }

    /**
     * 指定 slot に割り当てられたテクスチャの名前を返す。
     *
     * @param slot register(tN) の N (0..kMaxSlots-1)。
     * @return 該当テクスチャ名 (未割り当て・範囲外なら nullptr)。
     */
    const char* TextureNameAt(u32 slot) const noexcept {
        return slot < kMaxSlots && m_TexNames[slot][0] ? m_TexNames[slot] : nullptr;
    }

private:
    /** Init で受け取った Diligent デバイス。 */
    DiligentDevice*    m_Device = nullptr;

    /** Diligent ネイティブのシェーダオブジェクト。 */
    Diligent::IShader* m_Shader = nullptr;

    /** シェーダステージ (Vertex/Pixel 等)。 */
    EShaderStage        m_Stage  = EShaderStage::Vertex;

    /** HLSL source から抽出した cbuffer slot→名前マッピング (Diligent に BindPoint が無いため自前管理)。 */
    char m_CbNames [kMaxSlots][kMaxNameLen] = {};

    /** HLSL source から抽出したテクスチャ slot→名前マッピング。 */
    char m_TexNames[kMaxSlots][kMaxNameLen] = {};
};

} // namespace acs
