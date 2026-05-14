// グラフィックスパイプライン抽象（VS+PS+頂点レイアウト+ターゲットフォーマット）
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "render/RhiTypes.h"
#include "render/IRhiShader.h"
#include "render/IRhiSampler.h"

namespace acs {

class IRhiDevice;

// 頂点入力 1 要素（POSITION / COLOR / TEXCOORD など）
struct InputElement {
    const char* semantic_name  = "POSITION";  // HLSL のセマンティック名
    u32         semantic_index = 0;
    Format      format         = Format::R32G32B32_Float;
    u32         offset         = 0;            // 頂点構造体内のバイトオフセット
};

// カリング設定
enum class CullMode : u8 {
    None,   // カリング無し（両面描画）
    Front,  // 表面カリング
    Back,   // 裏面カリング（一般的）
};

// ブレンド設定（不透明 / 半透明）
enum class BlendMode : u8 {
    Opaque,        // 不透明（既定）
    AlphaBlend,    // src_alpha * src + (1-src_alpha) * dst（一般的な半透明）
    Additive,      // src + dst（加算）
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

    // === シェーダ binding 設定 ===
    // 定数バッファ b0..b{cbuffer_slots-1}（root CBV、Update で動的に書き換え可能）
    u32               cbuffer_slots = 0;
    // テクスチャ t0..t{texture_slots-1}（SRV、Draw 前に SetTexture で割当）
    u32               texture_slots = 0;
    // サンプラ s0..s{static_sampler_count-1}（パイプラインに焼き込み）
    SamplerDesc       static_samplers[8]      = {};
    u32               static_sampler_count    = 0;

    // --- HLSL リソース名（Diligent backend が名前ベースで SRB lookup するために必要） ---
    // 各 slot に対応する HLSL の cbuffer 名 / Texture2D 名。
    // null のままなら "cb{slot}" / "t{slot}" がフォールバックとして使われる。
    // DX12 raw backend は register slot で直接バインドするためここは無視される。
    //
    // 例: StandardShader の場合
    //   cbuffer_names[0] = "Frame";   // cbuffer Frame : register(b0)
    //   cbuffer_names[1] = "Object";  // cbuffer Object : register(b1)
    //   texture_names[0] = "albedo";    // Texture2D albedo : register(t0)
    //   texture_names[1] = "shadow_map"; // Texture2D shadow_map : register(t1)
    //
    // Diligent では HLSL サンプラ名は「<texture>_sampler」固定（CombinedSamplerSuffix）。
    // 例: Texture2D albedo;   SamplerState albedo_sampler;
    const char*       cbuffer_names[8] = {};
    const char*       texture_names[8] = {};

    // === ラスタライザ / ブレンド ===
    CullMode          cull_mode  = CullMode::None;
    BlendMode         blend_mode = BlendMode::Opaque;
    bool              depth_test  = false;   // depth_format != Unknown のとき有効
    bool              depth_write = true;
};

class IRhiPipeline {
public:
    virtual ~IRhiPipeline() noexcept = default;
};

Result<UniquePtr<IRhiPipeline>> CreateRhiPipeline(IRhiDevice& device,
                                                       const PipelineDesc& desc) noexcept;

} // namespace acs
