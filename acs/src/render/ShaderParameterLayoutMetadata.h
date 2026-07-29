// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "render/IRhiPipeline.h"

namespace acs {

/** シェーダー資源と頂点レイアウトの静的な個数情報。 */
struct FShaderParameterLayoutMetadata {
    /** 定数バッファのスロット数。 */
    u32 cbuffer_slots = 0u;
    /** 読み取り資源のスロット数。 */
    u32 texture_slots = 0u;
    /** 静的サンプラーのスロット数。 */
    u32 sampler_slots = 0u;
    /** 頂点入力要素数。 */
    u32 input_elements = 0u;

    /** graphics 用の固定上限内か返す。 */
    constexpr bool IsValidGraphics() const noexcept {
        return cbuffer_slots <= 16u && texture_slots <= 16u && sampler_slots <= 16u &&
               input_elements <= 8u;
    }

    /** compute 用の固定上限内か返す。 */
    constexpr bool IsValidCompute(u32 uav_slots) const noexcept {
        return cbuffer_slots <= 16u && texture_slots <= 16u && sampler_slots <= 16u &&
               input_elements == 0u && uav_slots <= 16u;
    }
};

/** graphics 記述子から個数情報を作る。 */
constexpr FShaderParameterLayoutMetadata ShaderLayoutMetadata(const FPipelineDesc& desc) noexcept {
    return {desc.cbuffer_slots, desc.texture_slots, desc.static_sampler_count, desc.layout_count};
}

/** compute 記述子から個数情報を作る。 */
constexpr FShaderParameterLayoutMetadata ShaderLayoutMetadata(const FComputePipelineDesc& desc) noexcept {
    return {desc.cbuffer_slots, desc.srv_slots, desc.static_sampler_count, 0u};
}

} // namespace acs
