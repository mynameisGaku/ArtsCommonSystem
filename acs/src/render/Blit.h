// SPDX-License-Identifier: Apache-2.0
// フルスクリーン texture コピー (ブリット) ユーティリティ
//
// 用途: 1 つの Texture2D (通常は描画済みの RT) をもう 1 つの Texture2D
//       (is_render_target=true で作成済) へ pixel-perfect にコピーする。
//       直接 GPU copy が RHI に無いため、フルスクリーン三角形 + テクスチャ
//       sample で代替する標準テクニック。
//
// 想定ユースケース: スクリーンスペース屈折のための background capture
// (HDR scene color → 屈折オブジェクトが sample する複製テクスチャ)。
//
// 使い方:
//   FBlit blit;
//   blit.Init(*device, hdr_format);                    // 1 度だけ
//   // フレーム中、コピーしたい時点で:
//   blit.Copy(*cl, *src_hdr, *dst_bg);                 // dst の format == hdr_format
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
class FBlit {
public:
    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    FBlit() noexcept = default;

    /** 破棄する (GPU リソースは TUniquePtr が解放)。 */
    ~FBlit() noexcept = default;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    FBlit(const FBlit&)            = delete;

    /** コピー代入も禁止。 */
    FBlit& operator=(const FBlit&) = delete;

    /**
     * シェーダとパイプラインを生成して初期化する。
     *
     * @details
     * rt_format は Copy の出力 RT のフォーマットで、PSO に焼き込まれる。出力 RT を
     * 別フォーマットに切り替えたい場合は別の FBlit インスタンスを使うこと。
     * @param device シェーダ・パイプライン生成に使う RHI デバイス。
     * @param rt_format 出力 RT のフォーマット (PSO に焼き込む)。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> Init(IRhiDevice& device, EFormat rt_format) noexcept;

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

} // namespace acs
