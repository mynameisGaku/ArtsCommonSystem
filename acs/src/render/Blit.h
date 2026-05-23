// SPDX-License-Identifier: Apache-2.0
// フルスクリーン texture コピー (ブリット) ユーティリティ (Phase 35-3b)
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
//   Blit blit;
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

class Blit {
public:
    Blit() noexcept = default;
    ~Blit() noexcept = default;

    Blit(const Blit&)            = delete;
    Blit& operator=(const Blit&) = delete;

    // 初期化。rt_format は Copy の出力 RT のフォーマット (PSO に焼き込む)。
    // 出力 RT を別フォーマットに切り替えたい場合は別 Blit インスタンスを使うこと。
    Result<void> Init(IRhiDevice& device, EFormat rt_format) noexcept;

    void Shutdown() noexcept;

    // src を dst へコピー (フルスクリーン pass)。
    // dst は is_render_target=true で Init 時の rt_format と一致すること。
    // 内部で BeginRenderToTextureLoad(dst) → SetPipeline → SetTexture → Draw(3)
    // → EndRenderToTexture(dst) を行う。全 pixel を上書きするので clear 不要。
    // viewport は dst のサイズに自動設定される。
    void Copy(IRhiCommandList& cmd, IRhiTexture& src, IRhiTexture& dst) noexcept;

    IRhiPipeline* Pipeline() const noexcept { return _pipeline.Get(); }

private:
    UniquePtr<IRhiShader>   _vs;
    UniquePtr<IRhiShader>   _ps;
    UniquePtr<IRhiPipeline> _pipeline;
};

} // namespace acs
