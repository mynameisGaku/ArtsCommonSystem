// SPDX-License-Identifier: Apache-2.0
// 紙が燃えて消える per-pixel ディゾルブ (2D オーバーレイ用シェーダ効果)
//
// 用途: 2D の矩形領域に「燃える紙」を 1 枚かぶせ、progress 0→1 で燃え際 (白熱→橙→
//       黒コゲ) が進みながら下の描画内容を出現させる。ピクセル単位の手続きノイズで
//       描くのでセル/ドットにならず高解像度。FSky と同じく自前の VS/PS/PSO/CB を持ち、
//       コマンドリストに 6 頂点のクアッドを 1 回 Draw するだけ (VB 不要)。
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "math/Vec.h"
#include "render/IRhiDevice.h"
#include "render/IRhiShader.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiBuffer.h"
#include "render/IRhiCommandList.h"

namespace acs {

/** 燃えディゾルブのパラメータ。 */
struct FBurnParams {
    /** 0=全部紙, 1=全部燃えて下が見える。 */
    f32   progress = 0.0f;

    /** 燃え際の帯幅 (0.05〜0.2)。 */
    f32   edge     = 0.12f;

    /** 燃え際の基準色 (橙)。 */
    FVec3 ember    = FVec3{1.0f, 0.45f, 0.06f};

    /** 紙 (覆い) の色。 */
    FVec3 paper    = FVec3{0.88f, 0.84f, 0.74f};

    /** ノイズ周波数 (大きいほど細かい炎縁)。 */
    f32   freq     = 7.0f;

    /** アニメ用の時間 (炎のゆらぎ。0 で静止)。 */
    f32   time     = 0.0f;

    /** ドット調の分割数 (0=なめらか最高解像度、N=N分割の四角ドット)。 */
    f32   cells    = 0.0f;
};

/**
 * 紙が燃えるディゾルブをピクセル単位で描く 2D オーバーレイ効果。
 *
 * @details DX12 raw / Diligent どちらでも動く (FSky と同じ生パイプライン)。アルファ
 *          ブレンドで重ね、燃え尽きた画素は discard して下の内容を見せる。
 */
class FBurnEffect {
public:
    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    FBurnEffect() noexcept = default;

    /** 破棄する (GPU リソースは TUniquePtr が解放)。 */
    ~FBurnEffect() noexcept = default;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    FBurnEffect(const FBurnEffect&) = delete;

    /** コピー代入も禁止。 */
    FBurnEffect& operator=(const FBurnEffect&) = delete;

    /**
     * VS/PS/PSO/定数バッファを生成する。
     *
     * @param device リソース生成に使う RHI デバイス。
     * @param rt_format 描画先カラーターゲットのフォーマット (バッチと一致させる)。
     * @return 成功なら空の TResult、確保失敗ならエラー。
     */
    TResult<void> Init(IRhiDevice& device, EFormat rt_format) noexcept;

    /** 確保した GPU リソースを解放する。 */
    void Shutdown() noexcept;

    /** Init 済みで描画可能か。 */
    bool Ready() const noexcept { return m_Pipeline.Get() != nullptr && m_Cb[0].Get() != nullptr; }

    /**
     * 矩形領域に燃えディゾルブを 1 枚描く。
     *
     * @param cl コマンドを積むコマンドリスト。
     * @param x,y 矩形左上 (スクリーン px)。
     * @param w,h 矩形サイズ (px)。
     * @param screen_w,screen_h 出力解像度 (NDC 変換用)。
     * @param p 燃えパラメータ。
     */
    void Draw(IRhiCommandList& cl, f32 x, f32 y, f32 w, f32 h,
              f32 screen_w, f32 screen_h, const FBurnParams& p) noexcept;

private:
    /** クアッドを出す頂点シェーダ。 */
    TUniquePtr<IRhiShader>   m_Vs;

    /** 燃えを計算するピクセルシェーダ。 */
    TUniquePtr<IRhiShader>   m_Ps;

    /** 描画パイプライン (アルファブレンド)。 */
    TUniquePtr<IRhiPipeline> m_Pipeline;

    /** 1 フレーム内で複数回 Draw しても CB が衝突しないようリングで持つ。 */
    static constexpr u32     kRing = 32;

    /** パラメータ定数バッファのリング。 */
    TUniquePtr<IRhiBuffer>   m_Cb[kRing];

    /** 次に使う CB スロット (Draw ごとに進める)。 */
    u32                      m_CbIdx = 0;
};

} // namespace acs
