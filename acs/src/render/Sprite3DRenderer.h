// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/Array.h"
#include "foundation/Result.h"
#include "math/Mat.h"
#include "math/Vec.h"
#include "memory/UniquePtr.h"
#include "render/IRhiBuffer.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiDevice.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiShader.h"
#include "render/IRhiTexture.h"

namespace acs {

/** 固定向きのローカルXY板へ画像を透過描画する3Dスプライト描画器。 */
class CSprite3DRenderer final {
public:
    /** owner threadでのパイプライン生成を待つコンパイル済みシェーダ群。 */
    struct FCompiledShaders {
        TUniquePtr<IRhiShader> Vertex;
        TUniquePtr<IRhiShader> Pixel;

        /** 両シェーダの完了状態を一つにまとめて返す。 */
        EShaderStatus Status() const noexcept;
    };

    /** CPUで生成してGPUへ送る1頂点。 */
    struct FVertex {
        /** ワールド空間位置。 */
        FVec3 Position;
        /** 画像を参照するUV座標。 */
        FVec2 Uv;
    };

    /** 1スプライトの入力状態。 */
    struct FDraw {
        /** ノードの完全なワールド変換。 */
        FMat4 World;
        /** 描画に使うGPU画像。nullは不正入力。 */
        IRhiTexture* Texture = nullptr;
    };

    /** 空の描画器を構築する。 */
    CSprite3DRenderer() noexcept = default;

    /** 所有GPU資源を解放する。 */
    ~CSprite3DRenderer() noexcept = default;

    /** GPU資源を単独所有するためコピーを禁止する。 */
    CSprite3DRenderer(const CSprite3DRenderer&) = delete;

    /** GPU資源を単独所有するためコピー代入を禁止する。 */
    CSprite3DRenderer& operator=(const CSprite3DRenderer&) = delete;

    /**
     * 3Dスプライト用資源を同期生成する。
     *
     * @param device GPU資源を生成するデバイス。
     * @param render_target_format 描画先HDR色バッファの形式。
     * @param depth_format 不透明描画と共有する深度バッファの形式。
     * @param max_sprite_count 1回のbatchで受け付ける最大スプライト数。
     * @return 全資源を公開できた場合だけ成功。
     */
    TResult<void> Init(IRhiDevice& device, EFormat render_target_format, EFormat depth_format, u32 max_sprite_count) noexcept;

    /** raw DX12向けにRHIデバイスへ触れずシェーダをCPUコンパイルする。 */
    static TResult<FCompiledShaders> CompileShadersCpu() noexcept;

    /** backend管理の非同期コンパイラへ両シェーダを投入する。 */
    static TResult<FCompiledShaders> BeginCompileShadersAsync(IRhiDevice& device) noexcept;

    /**
     * ready済みシェーダからowner threadでGPU資源を一括生成する。
     *
     * @return 失敗時は既存資源を変更せずエラーを返す。
     */
    TResult<void> InitWithCompiledShaders(IRhiDevice& device, FCompiledShaders&& shaders, EFormat render_target_format, EFormat depth_format, u32 max_sprite_count) noexcept;

    /** 全GPU資源とCPU作業領域を解放する。 */
    void Shutdown() noexcept;

    /**
     * 変換だけからエディタ互換の6頂点を決定論的に生成する。
     *
     * @param world ローカルXY板をワールドへ移す行列。
     * @param output 左上から始まる2三角形の受け取り先。
     * @return 入力と生成位置が有限ならtrue。
     */
    static bool TryBuildVertices(const FMat4& world, FVertex (&output)[6]) noexcept;

    /**
     * 検証済みbatchを現在の色/深度ターゲットへ透過描画する。
     *
     * @param command_list 描画命令の記録先。
     * @param view_projection 現在カメラのview×projection。
     * @param draws 入力配列。countが0以外ならnull不可。
     * @param count 描画数。初期化時の上限以下でなければならない。
     * @return 全入力をGPUへ反映して描画命令を記録できた場合にtrue。
     */
    bool DrawBatch(IRhiCommandList& command_list, const FMat4& view_projection, const FDraw* draws, u32 count) noexcept;

    /** 初期化済みパイプラインを返す。 */
    IRhiPipeline* Pipeline() const noexcept;

private:
    /** 頂点シェーダ。 */
    TUniquePtr<IRhiShader> m_VertexShader;

    /** 画像をsampleするピクセルシェーダ。 */
    TUniquePtr<IRhiShader> m_PixelShader;

    /** 深度testとalpha blendを行う描画パイプライン。 */
    TUniquePtr<IRhiPipeline> m_Pipeline;

    /** 1batch分の動的頂点バッファ。 */
    TUniquePtr<IRhiBuffer> m_VertexBuffer;

    /** view×projectionを保持する定数バッファ。 */
    TUniquePtr<IRhiBuffer> m_FrameBuffer;

    /** GPU更新前にbatch全体を組み立てるCPU頂点列。 */
    TArray<FVertex> m_Vertices;

    /** 初期化時に確定した最大スプライト数。 */
    u32 m_MaxSpriteCount = 0u;
};

} // namespace acs
