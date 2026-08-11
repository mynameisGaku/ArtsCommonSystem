// SPDX-License-Identifier: Apache-2.0
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
 * scene_depth から 1/8 解像度を level 0 とする min-depth pyramid を焼く Hi-Z。
 *
 * @details
 * level 0 の各 texel は元 depth の 8x8 ブロック中の最近接値を持ち、後続 level は
 * 直前 level の厳密な 2x2 min 縮約になる。2 本の R32G32_Float texture に level を
 * 偶奇で分けることで、同一 resource の SRV/RTV 同時利用を避ける。
 */
class CHiZ {
public:
    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    CHiZ() noexcept = default;

    /** 破棄する (GPU リソースは TUniquePtr が解放)。 */
    ~CHiZ() noexcept = default;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    CHiZ(const CHiZ&) = delete;

    /** コピー代入も禁止。 */
    CHiZ& operator=(const CHiZ&) = delete;

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
     * scene_depth から min-depth pyramid 全 level を焼く。
     *
     * @param device 描画に使う RHI デバイス。
     * @param cl コマンドを積むコマンドリスト。
     * @param scene_depth 入力 depth (shader_visible_depth=true の D32_Float)。
     */
    void Build(IRhiDevice& device, IRhiCommandList& cl,
               IRhiTexture& scene_depth) noexcept;

    /**
     * level 0 を保持する physical texture を返す。
     *
     * @details 旧 coarse-only SSR との互換 API。返値は EvenTexture() と同じで、
     * level 0 は mip 0 に格納される。それ以外の even mip も有効だが odd mip は未定義。
     * @return even-level texture (R32G32_Float、.r=min depth)。
     */
    IRhiTexture* Texture() const noexcept { return m_HizEven.Get(); }

    /**
     * 偶数 level を保持する physical texture を返す。
     *
     * @return level 0,2,4,... が同番号 mip に格納された texture。
     */
    IRhiTexture* EvenTexture() const noexcept { return m_HizEven.Get(); }

    /**
     * 奇数 level を保持する physical texture を返す。
     *
     * @return level 1,3,5,... が同番号 mip に格納された texture。
     */
    IRhiTexture* OddTexture() const noexcept { return m_HizOdd.Get(); }

    /**
     * pyramid の有効 level 数を返す。
     *
     * @return level 0 を含み、最終 1x1 level までの段数。
     */
    u32 MipCount() const noexcept { return m_MipCount; }

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

    /**
     * padded physical texture の幅を返す。
     *
     * @return NextPowerOfTwo(Width())。SSR の mip texel address 計算に使う。
     */
    u32 PhysicalWidth() const noexcept { return m_PhysicalW; }

    /**
     * padded physical texture の高さを返す。
     *
     * @return NextPowerOfTwo(Height())。SSR の mip texel address 計算に使う。
     */
    u32 PhysicalHeight() const noexcept { return m_PhysicalH; }

    /** 1 Hi-Z texel が覆う元 depth のブロック辺長。 */
    static constexpr u32 kBlockSize = 8;

    /**
     * RHI texture に確保する最大 level 数。
     *
     * @details level 0 が 1/8 解像度なので、D3D12 の最大 16384px texture でも
     * 必要なのは 12 level。16 は十分な余裕を持つ。
     */
    static constexpr u32 kMaxMipLevels = 16;

private:
    /**
     * 偶奇 2 本の min-depth mip texture を生成する。
     *
     * @param device RT 生成に使う RHI デバイス。
     * @param src_w 入力 scene_depth の幅。
     * @param src_h 入力 scene_depth の高さ。
     * @return 成功なら空の TResult、生成失敗ならエラー。
     */
    TResult<void> CreateRT(IRhiDevice& device, u32 src_w, u32 src_h) noexcept;

    /**
     * level 0 抽出と pyramid 縮約用の VS/PS/PSO を生成する。
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

    /** power-of-two padding 後の physical texture 幅。 */
    u32 m_PhysicalW = 0;

    /** power-of-two padding 後の physical texture 高さ。 */
    u32 m_PhysicalH = 0;

    /** level 0 から最終 1x1 までの有効 level 数。 */
    u32 m_MipCount = 0;

    /** 偶数 level を同番号 mip に保持する physical texture。 */
    TUniquePtr<IRhiTexture> m_HizEven;

    /** 奇数 level を同番号 mip に保持する physical texture。 */
    TUniquePtr<IRhiTexture> m_HizOdd;

    /** フルスクリーン三角形の頂点シェーダ。 */
    TUniquePtr<IRhiShader> m_Vs;

    /** 8x8 ブロックから level 0 を抽出するピクセルシェーダ。 */
    TUniquePtr<IRhiShader> m_PsBase;

    /** 直前 level から次 level を min 縮約するピクセルシェーダ。 */
    TUniquePtr<IRhiShader> m_PsReduce;

    /** scene depth から level 0 を作るパイプライン。 */
    TUniquePtr<IRhiPipeline> m_BasePipeline;

    /** pyramid の level N-1 から N を作るパイプライン。 */
    TUniquePtr<IRhiPipeline> m_ReducePipeline;

    /**
     * level ごとの immutable 定数バッファ。
     *
     * @details index N は src mip=N-1 / dst mip=N を保持する。各 draw が別 resource
     * を使うため、Raw DX12 で同一 upload CB を同フレーム中に上書きしない。
     */
    TUniquePtr<IRhiBuffer> m_LevelCb[kMaxMipLevels];
};

/** 旧名を使う既存コード向けの互換別名。 */
using FHiZ = CHiZ;


} // namespace acs
