// SPDX-License-Identifier: Apache-2.0
// 頂点空間サブサーフェススキャタリング 実装
#include "render/VertexScatter.h"

#include <cmath>

namespace acs {

namespace {

/** 位置を weld_epsilon 格子に量子化したキー (3D)。同セル = 溶接候補。 */
struct FWeldKey {
    i64 x, y, z;
    bool operator==(const FWeldKey& o) const noexcept { return x == o.x && y == o.y && z == o.z; }
};

FWeldKey QuantizePos(FVec3 p, f32 inv_eps) noexcept {
    return FWeldKey{ static_cast<i64>(std::llround(p.x * inv_eps)),
                    static_cast<i64>(std::llround(p.y * inv_eps)),
                    static_cast<i64>(std::llround(p.z * inv_eps)) };
}

} // namespace

bool CVertexScatter::Build(const AMeshAsset& mesh, f32 weld_epsilon) noexcept {
    const auto& verts = mesh.Vertices();
    const auto& idx   = mesh.Indices();
    if (verts.Num() == 0 || idx.Num() < 3) return false;
    // 位置だけを抜き出して汎用版へ渡す。
    TArray<FVec3> pos;
    pos.SetNum(verts.Num());
    for (u32 i = 0; i < verts.Num(); ++i) pos[i] = verts[i].position;
    return Build(pos.GetData(), static_cast<u32>(verts.Num()),
                 idx.GetData(), static_cast<u32>(idx.Num()), weld_epsilon);
}

bool CVertexScatter::Build(const FVec3* positions, u32 vcount, const u32* indices, u32 icount,
                           f32 weld_epsilon) noexcept {
    m_VertexCount = 0;
    m_NodeCount   = 0;
    m_Weld.Reset();
    m_Offset.Reset();
    m_Neighbor.Reset();
    m_Weight.Reset();
    if (positions == nullptr || vcount == 0 || indices == nullptr || icount < 3) return false;

    // --- 1. 位置溶接: 同位置の頂点 (UV シーム/極で複製) を 1 ノードに束ねる ---
    //   量子化キーの線形探索 (メッシュは数千頂点程度で十分速い)。
    const f32 inv_eps = (weld_epsilon > 0.0f) ? (1.0f / weld_epsilon) : 1e4f;
    m_Weld.SetNum(vcount);
    TArray<FWeldKey> nodeKeys;
    TArray<FVec3>   nodePos;     // 各ノードの代表位置 (溶接元の先頭)
    for (u32 i = 0; i < vcount; ++i) {
        const FWeldKey k = QuantizePos(positions[i], inv_eps);
        i32 node = -1;
        for (u32 n = 0; n < nodeKeys.Num(); ++n)
            if (nodeKeys[n] == k) { node = static_cast<i32>(n); break; }
        if (node < 0) {
            node = static_cast<i32>(nodeKeys.Num());
            nodeKeys.Add(k);
            nodePos.Add(positions[i]);
        }
        m_Weld[i] = node;
    }
    m_NodeCount   = static_cast<u32>(nodeKeys.Num());
    m_VertexCount = vcount;

    // --- 2. 隣接構築: 三角形の各辺を無向辺としてノード隣接に積む (重複辺は溶接後に統合) ---
    //   まず動的隣接 (ノードごとの可変リスト) を作り、辺長の逆数を重みとして加算する。
    TArray<TArray<u32>> adj;     adj.SetNum(m_NodeCount);
    TArray<TArray<f32>> adjW;    adjW.SetNum(m_NodeCount);
    auto addEdge = [&](u32 a, u32 b) noexcept {
        if (a == b) return;
        const FVec3 d = nodePos[a] - nodePos[b];
        const f32 len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        const f32 w   = 1.0f / ((len > 1e-6f) ? len : 1e-6f);   // 近いほど強く散る
        // 既存隣接なら重みを加算 (複数三角形が共有する辺)
        for (u32 e = 0; e < adj[a].Num(); ++e)
            if (adj[a][e] == b) { adjW[a][e] += w; return; }
        adj[a].Add(b);
        adjW[a].Add(w);
    };
    const u32 tris = icount / 3;
    for (u32 t = 0; t < tris; ++t) {
        const u32 i0 = indices[t * 3 + 0], i1 = indices[t * 3 + 1], i2 = indices[t * 3 + 2];
        if (i0 >= vcount || i1 >= vcount || i2 >= vcount) continue;
        const u32 a = static_cast<u32>(m_Weld[i0]);
        const u32 b = static_cast<u32>(m_Weld[i1]);
        const u32 c = static_cast<u32>(m_Weld[i2]);
        addEdge(a, b); addEdge(b, a);
        addEdge(b, c); addEdge(c, b);
        addEdge(c, a); addEdge(a, c);
    }

    // --- 3. CSR へ変換 (重みは «対称» な辺長逆数のまま、正規化しない) ---
    //   対称ラプラシアン拡散で «総和 (= 総エネルギー) を保存» するため行正規化はしない。
    //   代わりに次数 D_n = Σ_e w を持ち、安定な拡散係数 λ = 0.5/max_n D_n を求める。
    m_Offset.SetNum(m_NodeCount + 1);
    u32 total = 0;
    for (u32 n = 0; n < m_NodeCount; ++n) { m_Offset[n] = total; total += static_cast<u32>(adj[n].Num()); }
    m_Offset[m_NodeCount] = total;
    m_Neighbor.SetNum(total);
    m_Weight.SetNum(total);
    m_Degree.SetNum(m_NodeCount);
    f32 maxDeg = 1e-6f;
    for (u32 n = 0; n < m_NodeCount; ++n) {
        const u32 base = m_Offset[n];
        f32 deg = 0.0f;
        for (u32 e = 0; e < adj[n].Num(); ++e) {
            m_Neighbor[base + e] = adj[n][e];
            m_Weight[base + e]   = adjW[n][e];           // 対称 (辺の両端で同値)
            deg += adjW[n][e];
        }
        m_Degree[n] = deg;
        if (deg > maxDeg) maxDeg = deg;
    }
    m_Lambda = 0.5f / maxDeg;                            // λ·D ≤ 0.5 < 1 → 無条件安定
    return true;
}

void CVertexScatter::DiffuseChannel(const f32* src, f32* dst, f32* work, u32 iters) const noexcept {
    // ノード空間で iters 回の «対称グラフ・ラプラシアン» 熱拡散:
    //   x'_i = x_i + λ Σ_j w_ij (x_j - x_i)      (w_ij = w_ji)
    // 対称重みゆえ Σ_i Σ_j w_ij(x_j-x_i) = 0 → «総和 (総エネルギー) を保存»。
    // 定数場は x_j-x_i=0 で不動点。λ·D ≤ 0.5 で無条件安定。孤立ノードは隣接 0 で値保持。
    f32* a = work;            // cur
    f32* b = dst;             // next
    for (u32 i = 0; i < m_NodeCount; ++i) a[i] = src[i];
    for (u32 s = 0; s < iters; ++s) {
        for (u32 n = 0; n < m_NodeCount; ++n) {
            const u32 e0 = m_Offset[n], e1 = m_Offset[n + 1];
            f32 flux = 0.0f;
            for (u32 e = e0; e < e1; ++e) flux += m_Weight[e] * (a[m_Neighbor[e]] - a[n]);
            b[n] = a[n] + m_Lambda * flux;
        }
        f32* tmp = a; a = b; b = tmp;                    // ping-pong
    }
    if (a != dst) for (u32 i = 0; i < m_NodeCount; ++i) dst[i] = a[i];
}

void CVertexScatter::Scatter(const FVec3* in, FVec3* out, const FScatterProfile& profile) const noexcept {
    if (m_NodeCount == 0 || in == nullptr || out == nullptr) return;

    // 頂点空間 → ノード空間へ集約 (溶接された頂点群は平均を取る)。
    TArray<f32> nodeR, nodeG, nodeB, cnt;
    nodeR.SetNum(m_NodeCount); nodeG.SetNum(m_NodeCount);
    nodeB.SetNum(m_NodeCount); cnt.SetNum(m_NodeCount);
    for (u32 n = 0; n < m_NodeCount; ++n) { nodeR[n] = nodeG[n] = nodeB[n] = 0.0f; cnt[n] = 0.0f; }
    for (u32 i = 0; i < m_VertexCount; ++i) {
        const u32 n = static_cast<u32>(m_Weld[i]);
        nodeR[n] += in[i].x; nodeG[n] += in[i].y; nodeB[n] += in[i].z; cnt[n] += 1.0f;
    }
    for (u32 n = 0; n < m_NodeCount; ++n) {
        const f32 inv = (cnt[n] > 0.0f) ? (1.0f / cnt[n]) : 0.0f;
        nodeR[n] *= inv; nodeG[n] *= inv; nodeB[n] *= inv;
    }

    // RGB それぞれを別反復回数で拡散 (赤が最も遠くまで散る)。
    TArray<f32> outR, outG, outB, work;
    outR.SetNum(m_NodeCount); outG.SetNum(m_NodeCount); outB.SetNum(m_NodeCount);
    work.SetNum(m_NodeCount);
    DiffuseChannel(nodeR.GetData(), outR.GetData(), work.GetData(), profile.iterR);
    DiffuseChannel(nodeG.GetData(), outG.GetData(), work.GetData(), profile.iterG);
    DiffuseChannel(nodeB.GetData(), outB.GetData(), work.GetData(), profile.iterB);

    // ノード空間 → 頂点空間へ展開 (溶接元の各頂点へ同じ値を配る)。
    for (u32 i = 0; i < m_VertexCount; ++i) {
        const u32 n = static_cast<u32>(m_Weld[i]);
        out[i] = FVec3{ outR[n], outG[n], outB[n] };
    }
}

} // namespace acs
