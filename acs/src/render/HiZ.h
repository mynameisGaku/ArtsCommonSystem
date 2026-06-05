// SPDX-License-Identifier: Apache-2.0
// Hi-Z (Hierarchical-Z) coarse min-depth buffer
//
// scene_depth から 1/8 解像度の "min depth" RT を一発で焼く。各 pixel は元の
// 8x8 ブロック中の 最近接 (= camera から最も近い) NDC depth を持つ。SSR 等の
// screen-space ray march で「ray より遠くに surface が無い」と分かった場合に
// 複数 texel を一気にスキップする (skip-ahead) のに使う。
//
// 設計上の選択:
//   ・mip_levels=1 の単独 RT を採用 (true Hi-Z mip chain は Diligent の
//     whole-resource transition との相性が悪いため、同一テクスチャ内 mip 間
//     read-write を避ける)
//   ・EFormat は R16G16_Float (=BRDF LUT と同じ)。.r に min depth、.g は未使用。
//     16-bit half は [0,1] NDC depth に対し ~0.1% 精度 → skip 用途十分。
//   ・sky pixel (depth >= 0.9999) は無視 — 屋外シーンで「ground - sky」の
//     skip が大きく走るのは安全 (空には反射先が無い)
//
// 使い方 (SSR 統合):
//   FHiZ hiz;
//   hiz.Init(*dev, w, h);
//   // 毎フレーム main pass の depth が完成したあと、SSR 前に:
//   hiz.Build(*dev, *cl, scene_depth);
//   ssr.Render(..., hiz.Texture());     // SSR shader が hiz_min を読む
//
// 上位の SSR shader 側で skip-ahead する具体実装 (FSsr.cpp) と一体で機能する。
// Hi-Z を渡さない場合も FSsr で OK (nullptr fallback)。
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "render/IRhiDevice.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiTexture.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiShader.h"
#include "render/IRhiBuffer.h"
#include "render/RhiTypes.h"

namespace acs {

/**
 * scene_depth から 1/8 解像度の粗い min-depth バッファを焼く Hi-Z。
 *
 * @details
 * 各 texel は元の 8x8 ブロック中の最近接 (= camera から最も近い) NDC depth を持つ。
 * SSR の screen-space ray march で「ray より遠くに surface が無い」と分かった場合に
 * 複数 texel を一気にスキップする (skip-ahead) のに使う。RT は mip_levels=1 の
 * R16G16_Float (.r=min depth、.g 未使用) を単独所有する。
 */
class FHiZ {
public:
    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    FHiZ() noexcept = default;

    /** 破棄する (GPU リソースは TUniquePtr が解放)。 */
    ~FHiZ() noexcept = default;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    FHiZ(const FHiZ&) = delete;

    /** コピー代入も禁止。 */
    FHiZ& operator=(const FHiZ&) = delete;

    /**
     * GPU リソースを確保する。
     *
     * @details Hi-Z は内部で ceil(src_w / 8) x ceil(src_h / 8) サイズで確保される。
     * @param device RT・パイプライン生成に使う RHI デバイス。
     * @param src_width 入力 scene_depth の幅。
     * @param src_height 入力 scene_depth の高さ。
     * @return 成功なら空の TResult、確保失敗ならエラー。
     */
    TResult<void> Init(IRhiDevice& device, u32 src_width, u32 src_height) noexcept;

    /** 確保した GPU リソースを解放する (多重呼び出し安全)。 */
    void Shutdown() noexcept;

    /**
     * 解像度変更時に内部 RT を作り直す (ウィンドウリサイズで呼ぶ)。
     *
     * @param src_width 新しい入力 scene_depth の幅。
     * @param src_height 新しい入力 scene_depth の高さ。
     * @return 成功なら空の TResult、再確保失敗ならエラー。
     */
    TResult<void> Resize(u32 src_width, u32 src_height) noexcept;

    /**
     * scene_depth から coarse min-depth を焼く。
     *
     * @param device 描画に使う RHI デバイス。
     * @param cl コマンドを積むコマンドリスト。
     * @param scene_depth 入力 depth (shader_visible_depth=true の D32_Float)。
     */
    void Build(IRhiDevice& device, IRhiCommandList& cl,
               IRhiTexture& scene_depth) noexcept;

    /**
     * 1/8 解像度の min-depth RT を返す。
     *
     * @return min-depth RT (R16G16_Float、.r=min depth)。SSR が skip-ahead で sample する。
     */
    IRhiTexture* Texture() const noexcept { return m_Hiz.Get(); }

    /**
     * 入力 scene_depth の幅を返す。
     *
     * @return Init/Resize で渡された src 幅。
     */
    u32 SrcWidth() const noexcept { return m_SrcW; }

    /**
     * 入力 scene_depth の高さを返す。
     *
     * @return Init/Resize で渡された src 高さ。
     */
    u32 SrcHeight() const noexcept { return m_SrcH; }

    /**
     * Hi-Z RT の幅を返す。
     *
     * @return Hi-Z RT の幅 (src/8)。
     */
    u32 Width() const noexcept { return m_HizW; }

    /**
     * Hi-Z RT の高さを返す。
     *
     * @return Hi-Z RT の高さ (src/8)。
     */
    u32 Height() const noexcept { return m_HizH; }

    /** 1 Hi-Z texel が覆う元 depth のブロック辺長。 */
    static constexpr u32 kBlockSize = 8;

private:
    /**
     * min-depth RT を生成する。
     *
     * @param device RT 生成に使う RHI デバイス。
     * @param src_w 入力 scene_depth の幅。
     * @param src_h 入力 scene_depth の高さ。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> CreateRT(IRhiDevice& device, u32 src_w, u32 src_h) noexcept;

    /**
     * ダウンサンプル用の VS/PS/PSO を生成する。
     *
     * @param device パイプライン生成に使う RHI デバイス。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> CreatePipeline(IRhiDevice& device) noexcept;

    /** Init で受け取った device (Resize で再利用)。 */
    IRhiDevice* m_Device = nullptr;

    /** 入力 scene_depth の幅。 */
    u32 m_SrcW = 0;

    /** 入力 scene_depth の高さ。 */
    u32 m_SrcH = 0;

    /** Hi-Z RT の幅 (src/8)。 */
    u32 m_HizW = 0;

    /** Hi-Z RT の高さ (src/8)。 */
    u32 m_HizH = 0;

    /** 出力 min-depth RT (R16G16_Float, 1 mip, src/8)。 */
    TUniquePtr<IRhiTexture> m_Hiz;

    /** フルスクリーン三角形の頂点シェーダ。 */
    TUniquePtr<IRhiShader> m_Vs;

    /** 8x8 ブロックの最小 depth を求めるピクセルシェーダ。 */
    TUniquePtr<IRhiShader> m_Ps;

    /** ダウンサンプル描画のパイプライン。 */
    TUniquePtr<IRhiPipeline> m_Pipeline;

    /** 定数バッファ (Hi-Z 解像度などを渡す)。 */
    TUniquePtr<IRhiBuffer> m_Cb;
};

} // namespace acs
