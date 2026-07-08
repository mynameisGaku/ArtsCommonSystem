// SPDX-License-Identifier: Apache-2.0
// サンプラ抽象（テクスチャをシェーダで読む際のフィルタ・アドレッシング設定）
//
// パイプライン作成時に「静的サンプラ」として埋め込む方式と、
// バインドポイントとして公開して Draw 時に切り替える方式があるが、
// 本フレームワークでは初学者向けに前者（静的サンプラ）のみサポートする。
// FPipelineDesc::static_samplers にそのまま渡す。
#pragma once

#include "foundation/Types.h"

namespace acs {

/**
 * テクスチャをサンプルするときの拡大・縮小フィルタ。
 */
enum class ESamplerFilter : u8 {
    /** ニアレストネイバー（ドット絵向け）。 */
    Point,

    /** バイリニア / トライリニア（普通の写真向け）。 */
    Linear,

    /** 異方性フィルタ（高品質）。 */
    Anisotropic,
};

/**
 * UV がテクスチャ範囲外に出たときのラップ方式。
 */
enum class ESamplerAddress : u8 {
    /** 繰り返し（タイリング）。 */
    Wrap,

    /** 鏡像繰り返し。 */
    Mirror,

    /** 端の色で塗る。 */
    Clamp,

    /** 境界色（黒）で塗る。 */
    Border,
};

/**
 * 静的サンプラの設定。
 *
 * @details
 * フィルタとアドレッシング（U/V/W 各軸）、LOD 範囲、異方性度数をまとめて持つ。
 * FPipelineDesc::static_samplers にそのまま渡してパイプラインへ埋め込む。
 */
struct SamplerDesc {
    /** 拡大・縮小フィルタ。 */
    ESamplerFilter  filter      = ESamplerFilter::Linear;

    /** U 軸（横）のラップ方式。 */
    ESamplerAddress address_u   = ESamplerAddress::Wrap;

    /** V 軸（縦）のラップ方式。 */
    ESamplerAddress address_v   = ESamplerAddress::Wrap;

    /** W 軸（奥行き／3D テクスチャ用）のラップ方式。 */
    ESamplerAddress address_w   = ESamplerAddress::Wrap;

    /** サンプルに使う最小 LOD（ミップレベル）。 */
    f32            min_lod     = 0.0f;

    /** サンプルに使う最大 LOD（ミップレベル）。 */
    f32            max_lod     = 1000.0f;

    /** 異方性度数（ESamplerFilter::Anisotropic のときのみ有効）。 */
    u32            max_anisotropy = 16;

    /**
     * 比較サンプラにするか（シャドウの SampleCmpLevelZero 用）。
     *
     * @details
     * true にすると HW 深度比較 PCF サンプラになる（フィルタは比較版＝タップごとに 2x2
     * バイリニア比較）。比較関数は LessEqual 固定（lit ⇔ cmp ≤ stored_depth）。シェーダ側は
     * SamplerComparisonState で受け、SampleCmpLevelZero(s, uv, my_d - bias) で滑らかな
     * penumbra を得る。WickedEngine の sampler_cmp_depth 相当。通常テクスチャには使わない。
     */
    bool           comparison  = false;
};

} // namespace acs
