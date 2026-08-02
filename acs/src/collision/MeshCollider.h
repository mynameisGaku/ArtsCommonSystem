// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Result.h"
#include "container/Array.h"
#include "math/Vec.h"
#include "math/Collision3D.h"

namespace acs {

class AMeshAsset;

/**
 * 三角形メッシュから median-split AABB BVH を構築する 3D メッシュコライダー。
 *
 * @details
 * レンダリング用の AMeshAsset や生の頂点/インデックス配列から collider を作り、
 * レイキャスト (最近接ヒット) と AABB 重なりクエリを BVH で高速化する。中身は
 * 三角形リスト + 最長軸の空間中点で分割する BVH。視覚に依存しない純 CPU ロジックで、
 * ACS 規約に従い non-copy・全 noexcept・TResult を用いる。
 */
class CMeshCollider {
public:
    /** 空のコライダーを構築する (三角形・BVH は Build* で作る)。 */
    CMeshCollider() noexcept = default;

    /** 破棄する (配列は TArray が解放)。 */
    ~CMeshCollider() noexcept = default;

    /** コピー禁止 (BVH を単独所有するため)。 */
    CMeshCollider(const CMeshCollider&)            = delete;

    /** コピー代入も禁止。 */
    CMeshCollider& operator=(const CMeshCollider&) = delete;

    /**
     * レンダリング用メッシュから collider を構築する (頂点位置のみ使用)。
     *
     * @details 頂点位置を抜き出して BuildFromTriangles に委譲する。法線/UV は無視する。
     * @param mesh 三角形列を持つ入力メッシュアセット。
     * @return 成功なら空の TResult、頂点が無ければエラー。
     */
    TResult<void> BuildFromMesh(const AMeshAsset& mesh) noexcept;

    /**
     * 生の頂点位置 + 三角形インデックス列から collider を構築する。
     *
     * @details
     * indices が null または index_count < 3 のときは positions を連番三角形
     * (0,1,2),(3,4,5),... として扱う。範囲外インデックスや頂点不足はエラー。構築後に
     * BVH を張る。
     * @param positions 頂点位置配列の先頭ポインタ。
     * @param vertex_count positions の頂点数。
     * @param indices 三角形インデックス配列 (null 可)。
     * @param index_count indices の要素数。
     * @return 成功なら空の TResult、引数不正や三角形が作れなければエラー。
     */
    TResult<void> BuildFromTriangles(const FVec3* positions, u32 vertex_count,
                                     const u32* indices, u32 index_count) noexcept;

    /** 三角形・BVH・境界をすべて破棄して空状態に戻す。 */
    void Clear() noexcept;

    /**
     * レイを撃って最近接ヒットを返す (三角形面法線つき)。
     *
     * @details BVH を走査し t_max 未満で最も近い三角形交差を求める。未ヒットなら hit=false。
     * @param ray 始点と方向を持つレイ。
     * @param t_max 探索する最大パラメータ距離 (既定はほぼ無限大)。
     * @return 最近接ヒット情報 (hit / t / point / normal)。
     */
    FRayHit3 Raycast(const FRay3& ray, f32 t_max = 3.4028235e38f) const noexcept;

    /**
     * AABB がメッシュと重なるかを判定する (broadphase 用)。
     *
     * @details BVH ノード境界で枝刈りし、葉では各三角形の AABB と box の重なりを調べる。
     * @param box 重なりを調べる AABB。
     * @return いずれかの三角形 AABB と重なれば true。
     */
    bool OverlapsAabb(const FAabb3& box) const noexcept;

    /**
     * メッシュ全体の AABB を返す。
     *
     * @return 構築時に求めた境界ボックス。
     */
    FAabb3 Bounds()        const noexcept { return m_Bounds; }

    /**
     * 三角形数を返す。
     *
     * @return 構築された三角形の数。
     */
    u32   TriangleCount() const noexcept { return m_Tris.Size(); }

    /**
     * BVH ノード数を返す。
     *
     * @return BVH のノード数 (内部 + 葉)。
     */
    u32   NodeCount()     const noexcept { return m_Nodes.Size(); }

private:
    /**
     * BVH の葉が参照する 1 三角形 (頂点 3 点 + 重心)。
     *
     * @details 重心は BVH 分割の軸キーに使う。
     */
    struct FTri {
        /** 頂点 0 の位置。 */
        FVec3 v0;

        /** 頂点 1 の位置。 */
        FVec3 v1;

        /** 頂点 2 の位置。 */
        FVec3 v2;

        /** 3 頂点の重心 (BVH 分割キー)。 */
        FVec3 centroid;
    };

    /**
     * median-split BVH の 1 ノード。
     *
     * @details
     * 葉のときは tri_count>0 で [first_tri, first_tri+tri_count) が m_TriIndex の範囲。
     * 内部ノードのときは tri_count==0 で left / left+1 が子ノードのインデックス。
     */
    struct FBvhNode {
        /** このノードが覆う AABB。 */
        FAabb3 bounds;

        /** 内部ノードの左子のインデックス (右子は left+1)。 */
        u32   left      = 0;

        /** 葉のとき m_TriIndex 上の三角形開始インデックス。 */
        u32   first_tri = 0;

        /** 葉のとき含む三角形数 (0 なら内部ノード)。 */
        u32   tri_count = 0;
    };

    /** m_Tris から median-split AABB BVH を構築する (内部用)。 */
    void  BuildBvh() noexcept;

    /**
     * m_TriIndex の [first, first+count) 範囲の三角形を包む AABB を求める。
     *
     * @param first m_TriIndex 上の開始インデックス。
     * @param count 範囲に含む三角形数。
     * @return 範囲の三角形を包む境界ボックス。
     */
    FAabb3 ComputeBounds(u32 first, u32 count) const noexcept;

    /** 三角形リスト (頂点位置 + 重心)。 */
    TArray<FTri>     m_Tris;

    /** BVH 用の三角形並べ替えインデックス (m_Tris への参照)。 */
    TArray<u32>     m_TriIndex;

    /** BVH ノード配列 (ノード 0 が root)。 */
    TArray<FBvhNode> m_Nodes;

    /** メッシュ全体の AABB。 */
    FAabb3           m_Bounds{};
};

/** 旧名を使う既存コード向けの互換別名。 */
using FMeshCollider = CMeshCollider;

} // namespace acs
