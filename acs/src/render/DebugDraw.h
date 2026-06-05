// SPDX-License-Identifier: Apache-2.0
// FDebugDraw3D — 3D ライン (LineList) のデバッグ描画。コライダーの wireframe、
// AABB、レイ等を色付きで重ねるのに使う。
//
//   FDebugDraw3D dd;
//   dd.Init(*dev, renderer.ColorFormat());
//   ...
//   dd.Begin();
//   dd.Wireframe(positions, vcount, indices, icount, {0.4f,1,0.4f,1});  // メッシュ
//   dd.Aabb(box, {1,1,0,1});
//   dd.Line(a, b, {0,1,1,1});                                           // レイ等
//   dd.End(*cl, view_proj);   // backbuffer / RT が bind 済みの状態で
//
// depth 無し (常に手前に重なる overlay)。ACS 規約準拠 (noexcept / TResult / 非コピー)。
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "container/Array.h"
#include "math/Vec.h"
#include "math/Mat.h"
#include "math/Collision3D.h"
#include "render/IRhiDevice.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiShader.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiBuffer.h"
#include "render/RhiTypes.h"

namespace acs {

/**
 * 3D ライン (LineList) のデバッグ描画。
 *
 * @details
 * コライダーの wireframe、AABB、レイ等を色付きで overlay するのに使う。Begin で
 * 頂点バッファをクリアし、Line/Aabb/Wireframe でライン頂点を積み、End で現在 bind
 * 中のターゲットへ描画する。depth テスト無しで常に手前に重なる。ACS 規約準拠
 * (noexcept / TResult / 非コピー)。
 */
class FDebugDraw3D {
public:
    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    FDebugDraw3D() noexcept = default;

    /** 破棄する (GPU リソースは TUniquePtr が解放)。 */
    ~FDebugDraw3D() noexcept = default;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    FDebugDraw3D(const FDebugDraw3D&)            = delete;

    /** コピー代入も禁止。 */
    FDebugDraw3D& operator=(const FDebugDraw3D&) = delete;

    /**
     * GPU リソース (シェーダ・パイプライン・頂点/定数バッファ) を確保する。
     *
     * @param device リソース生成に使う RHI デバイス。
     * @param rt_format 描画先レンダーターゲットの色フォーマット。
     * @param max_lines 1 フレームで積めるライン本数の上限 (頂点バッファ容量)。
     * @return 成功なら空の TResult、確保失敗ならエラー。
     */
    TResult<void> Init(IRhiDevice& device, EFormat rt_format, u32 max_lines = 16384) noexcept;

    /** 確保した GPU リソースを解放する。 */
    void Shutdown() noexcept;

    /** フレームのライン蓄積を開始する (頂点バッファをクリアする)。 */
    void Begin() noexcept;

    /**
     * 2 点を結ぶ 1 本の線を積む。
     *
     * @param a 始点。
     * @param b 終点。
     * @param color 線の色 (RGBA)。
     */
    void Line(FVec3 a, FVec3 b, FVec4 color) noexcept;

    /**
     * AABB の 12 本のエッジを線で積む。
     *
     * @param box 描画する軸並行境界ボックス。
     * @param color 線の色 (RGBA)。
     */
    void Aabb(const Aabb3& box, FVec4 color) noexcept;

    /**
     * 三角形インデックス列の全エッジを線で積む (メッシュ/凸包の wireframe)。
     *
     * @param positions 頂点座標配列。
     * @param vertex_count positions の頂点数。
     * @param indices 三角形を構成するインデックス列。
     * @param index_count indices の要素数 (3 の倍数)。
     * @param color 線の色 (RGBA)。
     */
    void Wireframe(const FVec3* positions, u32 vertex_count,
                   const u32* indices, u32 index_count, FVec4 color) noexcept;

    /**
     * 積んだ全ラインを現在 bind 中のターゲットへ描画する。
     *
     * @param cl コマンドを積むコマンドリスト。
     * @param view_proj 適用する view-projection 行列 (model を畳んだ MVP でもよい)。
     */
    void End(IRhiCommandList& cl, const FMat4& view_proj) noexcept;

    /**
     * 現在積まれているライン本数を返す。
     *
     * @return ライン本数 (頂点数 / 2)。
     */
    u32 LineCount() const noexcept { return static_cast<u32>(m_Verts.Size() / 2); }

private:
    /** 1 本のラインを構成する 1 頂点 (位置 + RGBA カラー)。 */
    struct LineVtx { f32 px, py, pz, r, g, b, a; };

    /** ライン描画の頂点シェーダ。 */
    TUniquePtr<IRhiShader>   m_Vs;

    /** ライン描画のピクセルシェーダ。 */
    TUniquePtr<IRhiShader>   m_Ps;

    /** ライン描画のパイプライン (LineList、depth 無し)。 */
    TUniquePtr<IRhiPipeline> m_Pipeline;

    /** ライン頂点を保持する GPU 頂点バッファ。 */
    TUniquePtr<IRhiBuffer>   m_Vb;

    /** view-projection 行列を渡す定数バッファ。 */
    TUniquePtr<IRhiBuffer>   m_Cb;

    /** CPU 側のライン頂点蓄積 (End で頂点バッファへ転送)。 */
    TArray<LineVtx>          m_Verts;

    /** 頂点バッファに収まる頂点数の上限 (max_lines * 2)。 */
    u32                      m_MaxVerts = 0;
};

} // namespace acs
