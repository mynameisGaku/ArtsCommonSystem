// グラフィックスパイプライン抽象（VS+PS+頂点レイアウト+ターゲットフォーマット）
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "render/RhiTypes.h"
#include "render/IRhiShader.h"

namespace acs {

class IRhiDevice;

// 頂点入力 1 要素（POSITION / COLOR / TEXCOORD など）
struct InputElement {
    const char* semantic_name  = "POSITION";  // HLSL のセマンティック名
    u32         semantic_index = 0;
    Format      format         = Format::R32G32B32_Float;
    u32         offset         = 0;            // 頂点構造体内のバイトオフセット
};

struct PipelineDesc {
    IRhiShader*       vs            = nullptr;
    IRhiShader*       ps            = nullptr;
    PrimitiveTopology topology      = PrimitiveTopology::TriangleList;
    Format            rt_format     = Format::B8G8R8A8_UNorm;  // 描画先フォーマット
    Format            depth_format  = Format::Unknown;          // Unknown なら深度なし
    u32               vertex_stride = 0;                        // 1 頂点のバイト数
    InputElement      layout[8]     = {};                       // 入力レイアウト
    u32               layout_count  = 0;
};

class IRhiPipeline {
public:
    virtual ~IRhiPipeline() noexcept = default;
};

Result<class UniquePtr<IRhiPipeline>> CreateRhiPipeline(IRhiDevice& device,
                                                       const PipelineDesc& desc) noexcept;

} // namespace acs
