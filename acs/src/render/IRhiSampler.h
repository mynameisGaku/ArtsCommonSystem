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

// テクスチャのフィルタ
enum class ESamplerFilter : u8 {
    Point,        // ニアレストネイバー（ドット絵向け）
    Linear,       // バイリニア / トライリニア（普通の写真向け）
    Anisotropic,  // 異方性フィルタ（高品質）
};

// UV のラップ方式
enum class ESamplerAddress : u8 {
    Wrap,    // 繰り返し（タイリング）
    Mirror,  // 鏡像繰り返し
    Clamp,   // 端の色で塗る
    Border,  // 境界色（黒）で塗る
};

struct FSamplerDesc {
    ESamplerFilter  filter      = ESamplerFilter::Linear;
    ESamplerAddress address_u   = ESamplerAddress::Wrap;
    ESamplerAddress address_v   = ESamplerAddress::Wrap;
    ESamplerAddress address_w   = ESamplerAddress::Wrap;
    f32            min_lod     = 0.0f;
    f32            max_lod     = 1000.0f;
    u32            max_anisotropy = 16;       // ESamplerFilter::Anisotropic のときのみ有効
};

} // namespace acs
