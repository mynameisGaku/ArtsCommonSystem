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

/**
 * Diligent の IPipelineState + ShaderResourceBinding をラップしたパイプライン実装。
 *
 * @details
 * FPipelineDesc から PSO を生成し、定数バッファ / テクスチャの HLSL リソース名を slot 別に
 * 保持する。CommandList の SetConstantBuffer / SetTexture はこの名前で SRB 変数を lookup する。
 * 名前が未指定の slot は shader の reflection から補完し、それでも無ければ "cbN"/"tN" 形式の
 * フォールバック名を返す。
 */
class DiligentPipeline final : public IRhiPipeline {
public:
    /** 空状態で構築する (PSO は Init で生成)。 */
    DiligentPipeline() noexcept = default;

    /** PSO と SRB を Release して破棄する。 */
    ~DiligentPipeline() noexcept override;

    /** コピー禁止 (PSO/SRB を単独所有するため)。 */
    DiligentPipeline(const DiligentPipeline&) = delete;

    /** コピー代入も禁止。 */
    DiligentPipeline& operator=(const DiligentPipeline&) = delete;

    /**
     * FPipelineDesc から PSO と SRB を生成する。
     *
     * @details VS は必須、PS は depth-only pass で null 可。リソース名は desc 指定を優先し、
     * 未指定 slot は shader の reflection (VS 優先) から補完する。
     * @param device PSO 生成に使う Diligent デバイス。
     * @param desc パイプラインの記述 (シェーダ・入力レイアウト・ブレンド・深度等)。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> Init(DiligentDevice& device, const FPipelineDesc& desc) noexcept;

    /**
     * FComputePipelineDesc から compute PSO と SRB を生成する (Phase 0)。
     *
     * @param device PSO 生成に使う Diligent デバイス。
     * @param desc compute パイプラインの記述 (CS・cbuffer/SRV/UAV slot と名前)。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> InitCompute(DiligentDevice& device, const FComputePipelineDesc& desc) noexcept;

    /** compute パイプラインか (Dispatch 用)。 */
    bool IsCompute() const noexcept { return m_IsCompute; }

    /** slot に対応する UAV の HLSL リソース名を返す (compute、未指定なら "uN")。 */
    const char* UavName(u32 slot) const noexcept;

    /**
     * ネイティブの PSO を返す。
     *
     * @return Diligent の IPipelineState (未初期化なら nullptr)。
     */
    Diligent::IPipelineState*           Native() const noexcept { return m_Pso; }

    /**
     * ShaderResourceBinding を返す。
     *
     * @return Diligent の IShaderResourceBinding (未初期化なら nullptr)。
     */
    Diligent::IShaderResourceBinding*   Srb()    const noexcept { return m_Srb; }

    /**
     * 定数バッファのスロット数を返す。
     *
     * @return FPipelineDesc.cbuffer_slots の値。
     */
    u32 CbufferSlots() const noexcept { return m_CbSlots; }

    /**
     * テクスチャのスロット数を返す。
     *
     * @return FPipelineDesc.texture_slots の値。
     */
    u32 TextureSlots() const noexcept { return m_TexSlots; }

    /**
     * slot に対応する定数バッファの HLSL リソース名を返す。
     *
     * @details CommandList::SetConstantBuffer で SRB 変数の lookup に使う。desc 指定または
     * reflection 由来の名前が無ければ "cbN" 形式のフォールバック名を返す。
     * @param slot 定数バッファのスロット番号。
     * @return リソース名 (常に非 null)。
     */
    const char* CbufferName(u32 slot) const noexcept;

    /**
     * slot に対応するテクスチャの HLSL リソース名を返す。
     *
     * @details CommandList::SetTexture で SRB 変数の lookup に使う。desc 指定または
     * reflection 由来の名前が無ければ "tN" 形式のフォールバック名を返す。
     * @param slot テクスチャのスロット番号。
     * @return リソース名 (常に非 null)。
     */
    const char* TextureName(u32 slot) const noexcept;

private:
    /** PSO 生成に使った Diligent デバイス。 */
    DiligentDevice*                  m_Device = nullptr;

    /** ネイティブのパイプラインステート。 */
    Diligent::IPipelineState*        m_Pso    = nullptr;

    /** リソースバインディング。 */
    Diligent::IShaderResourceBinding* m_Srb   = nullptr;

    /** 定数バッファのスロット数。 */
    u32                              m_CbSlots  = 0;

    /** テクスチャのスロット数。 */
    u32                              m_TexSlots = 0;

    /** リソース名配列の最大 slot 数。 */
    static constexpr u32 kMaxResourceSlots = 16;

    /** slot 別の定数バッファ名 (null なら fallback 名を返す)。 */
    const char* m_CbNames[kMaxResourceSlots]  = {};

    /** slot 別のテクスチャ名 (null なら fallback 名を返す)。 */
    const char* m_TexNames[kMaxResourceSlots] = {};

    /** compute パイプラインか。 */
    bool        m_IsCompute = false;

    /** UAV のスロット数 (compute)。 */
    u32         m_UavSlots  = 0;

    /** slot 別の UAV 名 (compute)。 */
    const char* m_UavNames[kMaxResourceSlots] = {};

    /**
     * 定数バッファのフォールバック名を返す。
     *
     * @param slot 定数バッファのスロット番号。
     * @return "cb0".."cb15" (範囲外は "cb_invalid")。
     */
    static const char* FallbackCbName(u32 slot) noexcept;

    /**
     * テクスチャのフォールバック名を返す。
     *
     * @param slot テクスチャのスロット番号。
     * @return "t0".."t15" (範囲外は "t_invalid")。
     */
    static const char* FallbackTexName(u32 slot) noexcept;
};

} // namespace acs
