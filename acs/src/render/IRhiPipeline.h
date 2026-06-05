// SPDX-License-Identifier: Apache-2.0
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

/**
 * 頂点入力 1 要素 (POSITION / COLOR / TEXCOORD など)。
 *
 * @details HLSL のセマンティック名・インデックス・フォーマットと、頂点構造体内オフセットを持つ。
 */
struct InputElement {
    /** HLSL のセマンティック名。 */
    const char* semantic_name  = "POSITION";

    /** 同一セマンティック名内のインデックス。 */
    u32         semantic_index = 0;

    /** この要素のデータフォーマット。 */
    EFormat      format         = EFormat::R32G32B32_Float;

    /** 頂点構造体内のバイトオフセット。 */
    u32         offset         = 0;
};

/**
 * カリング設定。
 */
enum class ECullMode : u8 {
    /** カリング無し (両面描画)。 */
    None,

    /** 表面カリング。 */
    Front,

    /** 裏面カリング (一般的)。 */
    Back,
};

/**
 * ブレンド設定 (不透明 / 半透明)。
 */
enum class EBlendMode : u8 {
    /** 不透明 (既定)。 */
    Opaque,

    /** src_alpha * src + (1-src_alpha) * dst (一般的な半透明)。 */
    AlphaBlend,

    /** src + dst (加算)。 */
    Additive,
};

/**
 * 比較関数 (深度 / ステンシルテスト共通)。
 */
enum class ECompareFunc : u8 {
    /** 常に不合格。 */
    Never,

    /** 参照値より小さければ合格。 */
    Less,

    /** 参照値と等しければ合格。 */
    Equal,

    /** 参照値以下なら合格。 */
    LessEqual,

    /** 参照値より大きければ合格。 */
    Greater,

    /** 参照値と異なれば合格。 */
    NotEqual,

    /** 参照値以上なら合格。 */
    GreaterEqual,

    /** 常に合格。 */
    Always,
};

/**
 * ステンシル操作 (テスト結果に応じてステンシル値をどう更新するか)。
 */
enum class EStencilOp : u8 {
    /** 現在値を保持する。 */
    Keep,

    /** 0 にする。 */
    Zero,

    /** 参照値 (SetStencilRef) で置換する。 */
    Replace,

    /** +1 (飽和)。 */
    IncrSat,

    /** -1 (飽和)。 */
    DecrSat,

    /** ビット反転する。 */
    Invert,

    /** +1 (ラップ)。 */
    IncrWrap,

    /** -1 (ラップ)。 */
    DecrWrap,
};

/**
 * ステンシル設定 (マスク描画用)。
 *
 * @details
 * enable=false (既定) なら無効。有効化には depth_format が stencil 成分を持つ format
 * (D24_UNorm_S8_UInt) であること、かつ stencil 付き深度バッファが bind されていることが
 * 必要。参照値は実行時に IRhiCommandList::SetStencilRef で動的に与える。
 */
struct FStencilDesc {
    /** ステンシルテストを有効にするか (既定 false)。 */
    bool         enable        = false;

    /** ステンシル比較関数。 */
    ECompareFunc func          = ECompareFunc::Always;

    /** stencil・depth 共に pass したときの操作。 */
    EStencilOp   pass_op       = EStencilOp::Keep;

    /** stencil fail したときの操作。 */
    EStencilOp   fail_op       = EStencilOp::Keep;

    /** stencil pass / depth fail したときの操作。 */
    EStencilOp   depth_fail_op = EStencilOp::Keep;

    /** 比較時に AND する読み出しマスク。 */
    u8           read_mask     = 0xFF;

    /** 書込み時に AND する書込みマスク。 */
    u8           write_mask    = 0xFF;
};

/**
 * グラフィックスパイプライン生成パラメータ。
 *
 * @details
 * VS/PS・トポロジ・レンダーターゲット/深度フォーマット・入力レイアウト・シェーダ binding
 * (cbuffer / texture / sampler スロットと HLSL リソース名)・ラスタライザ/ブレンド/深度/
 * ステンシル設定をまとめて指定する。
 */
struct FPipelineDesc {
    /** 頂点シェーダ。 */
    IRhiShader*       vs            = nullptr;

    /** ピクセルシェーダ。 */
    IRhiShader*       ps            = nullptr;

    /** プリミティブトポロジ。 */
    EPrimitiveTopology topology      = EPrimitiveTopology::TriangleList;

    /** 単一 RT (legacy / 既定経路) のフォーマット。rt_count == 0 のとき使われる。 */
    EFormat            rt_format     = EFormat::B8G8R8A8_UNorm;

    /**
     * MRT 用レンダーターゲットフォーマット配列。
     *
     * @details
     * rt_count > 0 のとき rt_formats[0..rt_count-1] を使い、rt_format は無視される。最大 8 RT。
     * rt_count 未満の各 slot は valid (≠ Unknown) format が必須。Unknown が残っていると
     * DiligentPipeline::Init が ACS_ERR(Render, 155) を返す。Diligent backend のみ実装、
     * Dx12 raw は stub (warning ログ 1 回)。
     */
    EFormat            rt_formats[8] = {};

    /** 有効なレンダーターゲット数 (0 のとき rt_format を使う単一 RT)。 */
    u32               rt_count      = 0;

    /** 深度バッファのフォーマット (Unknown なら深度なし)。 */
    EFormat            depth_format  = EFormat::Unknown;

    /** 1 頂点のバイト数。 */
    u32               vertex_stride = 0;

    /** 入力レイアウト (最大 8 要素)。 */
    InputElement      layout[8]     = {};

    /** 入力レイアウトの有効要素数。 */
    u32               layout_count  = 0;

    /** 定数バッファのスロット数 b0..b{cbuffer_slots-1} (root CBV、Update で動的に書き換え可能)。 */
    u32               cbuffer_slots = 0;

    /** テクスチャのスロット数 t0..t{texture_slots-1} (SRV、Draw 前に SetTexture で割当)。 */
    u32               texture_slots = 0;

    /** パイプラインに焼き込む static サンプラ s0..s{static_sampler_count-1} (容量 16)。 */
    SamplerDesc       static_samplers[16]     = {};

    /** static サンプラの有効数。 */
    u32               static_sampler_count    = 0;

    /**
     * 各 cbuffer slot に対応する HLSL の cbuffer 名。
     *
     * @details
     * Diligent backend が名前ベースで SRB lookup するために必要。null のままなら "cb{slot}" が
     * フォールバックとして使われる。DX12 raw backend は register slot で直接バインドするため無視される。
     * 例: cbuffer_names[0] = "Frame" (cbuffer Frame : register(b0))、
     * cbuffer_names[1] = "Object" (cbuffer Object : register(b1))。
     */
    const char*       cbuffer_names[16] = {};

    /**
     * 各 texture slot に対応する HLSL の Texture2D 名。
     *
     * @details
     * Diligent backend が名前ベースで SRB lookup するために必要。null のままなら "t{slot}" が
     * フォールバックとして使われる。DX12 raw backend では無視される。例:
     * texture_names[0] = "albedo" (Texture2D albedo : register(t0))、
     * texture_names[1] = "shadow_map" (Texture2D shadow_map : register(t1))。Diligent では HLSL
     * サンプラ名は「<texture>_sampler」固定 (CombinedSamplerSuffix。例: Texture2D albedo;
     * SamplerState albedo_sampler;)。
     */
    const char*       texture_names[16] = {};

    /** カリング設定。 */
    ECullMode          cull_mode  = ECullMode::None;

    /** ブレンド設定。 */
    EBlendMode         blend_mode = EBlendMode::Opaque;

    /** 深度テストを行うか (depth_format != Unknown のとき有効)。 */
    bool              depth_test  = false;

    /** 深度バッファへ書き込むか。 */
    bool              depth_write = true;

    /**
     * ステンシル設定 (マスク描画用、既定 disable)。
     *
     * @details depth とは独立に有効化でき、depth_test=false でも stencil.enable=true なら
     * stencil テスト/書込みが効く。
     */
    FStencilDesc      stencil    = {};
};

/**
 * グラフィックスパイプライン (PSO) の抽象インターフェイス。
 *
 * @details
 * VS+PS+頂点レイアウト+ターゲットフォーマット等を焼き込んだ状態オブジェクト。バックエンドが
 * 実装する。生成は CreateRhiPipeline で行う。
 */
class IRhiPipeline {
public:
    /** 派生バックエンド実装を正しく破棄するための仮想デストラクタ。 */
    virtual ~IRhiPipeline() noexcept = default;
};

/**
 * グラフィックスパイプラインを生成する。
 *
 * @param device パイプライン生成に使う RHI デバイス。
 * @param desc パイプライン生成パラメータ。
 * @return 成功なら所有権付きパイプライン、生成失敗ならエラー。
 */
TResult<TUniquePtr<IRhiPipeline>> CreateRhiPipeline(IRhiDevice& device,
                                                       const FPipelineDesc& desc) noexcept;

} // namespace acs
