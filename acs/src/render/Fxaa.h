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
 * FXAA をフルスクリーン三角形 1 パスで掛けるポストエフェクト。
 *
 * @details
 * FXAA 3.11 の簡易品質版。中心+対角 4 近傍の輝度からエッジ方向を推定し、
 * エッジに沿って 2/4 タップのブレンドを行う。低コントラスト画素は早期 return で
 * 素通しするため、ベタ塗り領域やテキストのにじみは最小限。入力サイズは
 * GetDimensions で取得するので cbuffer 不要。
 */
class CFxaa {
public:
    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    CFxaa() noexcept = default;

    /** 破棄する (GPU リソースは TUniquePtr が解放)。 */
    ~CFxaa() noexcept = default;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    CFxaa(const CFxaa&)            = delete;

    /** コピー代入も禁止。 */
    CFxaa& operator=(const CFxaa&) = delete;

    /**
     * シェーダとパイプラインを生成して初期化する。
     *
     * @param device シェーダ・パイプライン生成に使う RHI デバイス。
     * @param rt_format 出力先 RT のフォーマット (PSO に焼き込む)。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> Init(IRhiDevice& device, EFormat rt_format) noexcept;

    /** 確保した GPU リソースを解放する (多重呼び出し安全)。 */
    void Shutdown() noexcept;

    /**
     * 現在 bind 中のターゲットへ src を FXAA 解決しつつ全画面描画する。
     *
     * @details
     * CBlit::Copy と違い出力 RT の bind は行わない。呼ぶ前に BeginRenderToSwapchain
     * 等で出力先を bind し、viewport を設定しておくこと (全 pixel を上書きする)。
     * @param cmd コマンドを積むコマンドリスト。
     * @param src FXAA を掛けるシーンテクスチャ (SRV 状態であること)。
     */
    void Apply(IRhiCommandList& cmd, IRhiTexture& src) noexcept;

private:
    /** フルスクリーン三角形の頂点シェーダ。 */
    TUniquePtr<IRhiShader>   m_Vs;

    /** FXAA ピクセルシェーダ。 */
    TUniquePtr<IRhiShader>   m_Ps;

    /** FXAA 描画のパイプライン。 */
    TUniquePtr<IRhiPipeline> m_Pipeline;
};

/** 旧名を使う既存コード向けの互換別名。 */
using FFxaa = CFxaa;


} // namespace acs
