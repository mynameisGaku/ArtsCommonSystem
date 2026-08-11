// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "render/IRhiDevice.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiShader.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiTexture.h"
#include "render/RhiTypes.h"

namespace acs {

/**
 * フルスクリーン三角形で 1 つのテクスチャを別のテクスチャへコピーするブリット。
 *
 * @details
 * 直接 GPU copy が RHI に無いため、フルスクリーン三角形 + テクスチャ sample で
 * pixel-perfect コピーを行う標準テクニック。出力 RT のフォーマットは Init 時に
 * PSO へ焼き込むため、別フォーマットへコピーしたい場合は別インスタンスを使う。
 */
class CBlit {
public:
    /** Compiled shader handles awaiting owner-thread PSO creation. */
    struct FCompiledShaders {
        TUniquePtr<IRhiShader> vertex;
        TUniquePtr<IRhiShader> pixel;

        EShaderStatus Status() const noexcept;
    };

    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    CBlit() noexcept = default;

    /** 破棄する (GPU リソースは TUniquePtr が解放)。 */
    ~CBlit() noexcept = default;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    CBlit(const CBlit&)            = delete;

    /** コピー代入も禁止。 */
    CBlit& operator=(const CBlit&) = delete;

    /**
     * シェーダとパイプラインを生成して初期化する。
     *
     * @details
     * rt_format は Copy の出力 RT のフォーマットで、PSO に焼き込まれる。出力 RT を
     * 別フォーマットに切り替えたい場合は別の CBlit インスタンスを使うこと。
     * @param device シェーダ・パイプライン生成に使う RHI デバイス。
     * @param rt_format 出力 RT のフォーマット (PSO に焼き込む)。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> Init(IRhiDevice& device, EFormat rt_format) noexcept;

    /** Compile raw-DX12 shader bytecode without touching an RHI device. */
    static TResult<FCompiledShaders> CompileShadersCpu() noexcept;

    /** Submit both shaders to a backend-managed compiler pool. */
    static TResult<FCompiledShaders> BeginCompileShadersAsync(
        IRhiDevice& device) noexcept;

    /** Create and atomically publish the PSO from ready shader handles. */
    TResult<void> InitWithCompiledShaders(
        IRhiDevice& device,
        FCompiledShaders&& shaders,
        EFormat rt_format) noexcept;

    /** 確保した GPU リソースを解放する (多重呼び出し安全)。 */
    void Shutdown() noexcept;

    /**
     * src を dst へフルスクリーン pass でコピーする。
     *
     * @details
     * dst は is_render_target=true で Init 時の rt_format と一致すること。内部で
     * BeginRenderToTextureLoad(dst) → SetPipeline → SetTexture → Draw(3) →
     * EndRenderToTexture(dst) を行う。全 pixel を上書きするので clear 不要、
     * viewport は dst のサイズに自動設定される。
     * @param cmd コマンドを積むコマンドリスト。
     * @param src コピー元テクスチャ。
     * @param dst コピー先 RT (rt_format に一致すること)。
     */
    void Copy(IRhiCommandList& cmd, IRhiTexture& src, IRhiTexture& dst) noexcept;

    /**
     * 内部のブリット用パイプラインを返す。
     *
     * @return ブリット用パイプライン (未初期化なら nullptr)。
     */
    IRhiPipeline* Pipeline() const noexcept { return m_Pipeline.Get(); }

private:
    /** フルスクリーン三角形の頂点シェーダ。 */
    TUniquePtr<IRhiShader>   m_Vs;

    /** source texture を素 sample するピクセルシェーダ。 */
    TUniquePtr<IRhiShader>   m_Ps;

    /** ブリット描画のパイプライン。 */
    TUniquePtr<IRhiPipeline> m_Pipeline;
};

/** 旧名を使う既存コード向けの互換別名。 */
using FBlit = CBlit;


} // namespace acs
