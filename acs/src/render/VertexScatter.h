// SPDX-License-Identifier: Apache-2.0
// 頂点空間サブサーフェススキャタリング (Vertex-Space Subsurface Scattering)
//
// 概要:
//   メッシュ表面の放射照度 (irradiance) を «頂点空間» (= メッシュの接続グラフ上) で拡散し、
//   皮膚/ロウ/玉のような内部散乱の «にじみ» を作る。スクリーン空間 SSS と違い、深度不連続を
//   越えた光漏れが無く、視点非依存。拡散はメッシュ隣接グラフ上の熱拡散 (ヤコビ反復) で行い、
//   RGB チャンネルごとに反復回数を変えることで波長依存の散乱 (赤が最も遠くまで散る) を再現する。
//
// パイプライン:
//   1. Build(mesh): 頂点を位置で溶接 (UV シーム/極の重複頂点を 1 ノードに) し、辺長の逆数を
//      正規化した拡散演算子 (CSR) を構築する。1 メッシュにつき 1 度。
//   2. 毎フレーム: 各頂点の Lambert 放射照度 (Σ ライト) を計算 → Scatter() で頂点空間拡散 →
//      得られた «散乱後の放射照度» を頂点カラー等として描画に渡す (内部散乱項)。
//
// 設計: GPU 非依存・決定的 (ユニットテスト可)。重い前計算は Build に閉じ、Scatter は軽い反復。
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "container/Array.h"
#include "asset/MeshAsset.h"

namespace acs {

/**
 * RGB チャンネル別の散乱プロファイル (反復回数で拡散半径を表す)。
 *
 * @details 値が大きいほど遠くまで散る。皮膚は赤 > 緑 > 青 (例 {12,5,3})。各反復は隣接平均への
 * 緩和 (α=0.5 ヤコビ) で、半径は概ね sqrt(iterations) に比例する。0 はそのチャンネル無散乱。
 */
struct FScatterProfile {
    u32 iterR = 12;   ///< 赤チャンネルの拡散反復回数。
    u32 iterG = 5;    ///< 緑チャンネルの拡散反復回数。
    u32 iterB = 3;    ///< 青チャンネルの拡散反復回数。
};

/**
 * 頂点空間 SSS の拡散演算子 (メッシュ隣接グラフ + 正規化辺重み)。
 *
 * @details
 * Build でメッシュから «位置溶接済み» の隣接グラフ (CSR) を構築し、Scatter で per-vertex の
 * RGB 放射照度をグラフ上で拡散する。拡散ステップは行確率的 (各頂点の隣接重みの総和=1) な
 * 凸結合なので無条件安定かつ大域平均を保存する (= エネルギー保存)。
 */
class CVertexScatter {
public:
    /** 空状態で構築する (Build で確保)。 */
    CVertexScatter() noexcept = default;

    /**
     * メッシュから拡散演算子を構築する。
     *
     * @details 同位置の頂点 (UV シーム/極で複製されたもの) を溶接して 1 ノードに束ね、辺長の
     * 逆数を頂点ごとに正規化した重みを持つ隣接 (CSR) を作る。孤立頂点は隣接 0 = 無散乱。
     * @param mesh 対象メッシュ (頂点位置とインデックスを使う)。
     * @param weld_epsilon 位置溶接のしきい値 (これ以下の距離の頂点を同一ノードに束ねる)。
     * @return 頂点数 >= 1 かつ三角形が 1 つ以上あれば true。
     */
    bool Build(const AMeshAsset& mesh, f32 weld_epsilon = 1e-4f) noexcept;

    /** 頂点配列・インデックス配列から直接構築する (アセット非依存の検証/汎用用)。 */
    bool Build(const FVec3* positions, u32 vcount, const u32* indices, u32 icount,
               f32 weld_epsilon = 1e-4f) noexcept;

    /** 構築済みか (頂点数 >= 1)。 */
    bool IsBuilt() const noexcept { return m_VertexCount > 0; }

    /** 元メッシュの頂点数。 */
    u32 VertexCount() const noexcept { return m_VertexCount; }

    /**
     * per-vertex の放射照度を頂点空間で拡散する。
     *
     * @details in[i] (頂点 i の RGB 放射照度) を隣接グラフ上で profile に従い拡散し out[i] へ書く。
     * in==out (in-place) も可。RGB それぞれ独立に反復するため波長依存の «にじみ» が出る。
     * @param in 入力放射照度配列 (要素数 = VertexCount)。
     * @param out 出力配列 (要素数 = VertexCount。in と同一可)。
     * @param profile RGB 別の拡散反復回数。
     */
    void Scatter(const FVec3* in, FVec3* out, const FScatterProfile& profile) const noexcept;

private:
    /** 1 チャンネルを iters 回拡散する (src→dst、ping-pong バッファ work を使う)。 */
    void DiffuseChannel(const f32* src, f32* dst, f32* work, u32 iters) const noexcept;

    u32          m_VertexCount = 0;   ///< 元メッシュの頂点数。
    TArray<i32>  m_Weld;              ///< 頂点 i → 溶接ノード番号 (0..node_count-1)。
    u32          m_NodeCount = 0;     ///< 溶接後のノード数。
    TArray<u32>  m_Offset;            ///< CSR: ノード i の隣接は [Offset[i], Offset[i+1])。
    TArray<u32>  m_Neighbor;          ///< CSR: 隣接ノード番号。
    TArray<f32>  m_Weight;            ///< CSR: 対称辺重み (辺長の逆数。正規化なし)。
    TArray<f32>  m_Degree;            ///< ノードごとの次数 D_n = Σ_e w (安定 λ の算出に使用)。
    f32          m_Lambda = 0.0f;     ///< 拡散係数 0.5/max(D_n) (λ·D ≤ 0.5 で安定)。
};

/** 旧名を使う既存コード向けの互換別名。 */
using FVertexScatter = CVertexScatter;


} // namespace acs
