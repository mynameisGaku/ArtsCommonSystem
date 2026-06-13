// SPDX-License-Identifier: Apache-2.0
// HelloVertexSSS — 頂点空間サブサーフェススキャタリング (Vertex-Space SSS) のデモ。
// 同じ肌色の球を 2 つ並べ、左 = 生 Lambert (無散乱) / 右 = 頂点空間 SSS で照明する。
// 右側は terminator (明暗境界) を赤い光が回り込んで «にじむ» = 皮膚らしい内部散乱。
// 散乱は FVertexScatter (メッシュ隣接グラフ上の RGB 別熱拡散) で CPU 計算し、
// per-vertex の散乱後放射照度を動的頂点バッファに毎フレーム書き込んで描画する。
#pragma once

#include "app/Application.h"

#include "render/VertexScatter.h"
#include "render/RenderAssets.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "render/IRhiShader.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiBuffer.h"

#include "asset/MeshAsset.h"
#include "memory/SharedPtr.h"
#include "math/Camera.h"

namespace hellovertexsss {

// シェーダ頂点 (位置 + 法線 + 散乱後放射照度 RGB)。プレーン float で密パック (36 bytes)。
// 注意: FVec3 は alignas(16) で 16 bytes あるため頂点には使えない (stride/offset がずれる)。
struct VtxSSS {
    acs::f32 px, py, pz;
    acs::f32 nx, ny, nz;
    acs::f32 sr, sg, sb;
};

class HelloVertexSssApp : public acs::FApplication {
public:
    void OnStart()    noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;

private:
    // light 方向から per-vertex の散乱後放射照度を計算し dst (動的 VB) を更新する。
    // scatter=true で FVertexScatter を通し (右球)、false で生 Lambert (左球)。
    void UpdateSphere(acs::IRhiBuffer* vb, acs::FVec3 lightDir, bool scatter) noexcept;

    acs::TSharedPtr<acs::FMeshAsset> m_Sphere;   // CPU メッシュ (位置/法線/インデックス)
    acs::FVertexScatter              m_Scatter;  // 頂点空間拡散演算子
    acs::TArray<VtxSSS>              m_CpuVtx;   // 毎フレーム書き換える頂点ステージ
    acs::TArray<acs::FVec3>          m_Irr;      // per-vertex 入力放射照度 (work)
    acs::TArray<acs::FVec3>          m_IrrOut;   // 散乱後 (work)

    acs::TUniquePtr<acs::IRhiShader>   m_Vs, m_Ps;
    acs::TUniquePtr<acs::IRhiPipeline> m_Pipe;
    acs::TUniquePtr<acs::IRhiBuffer>   m_Ib;
    acs::TUniquePtr<acs::IRhiBuffer>   m_VbLeft;   // 生 Lambert 球
    acs::TUniquePtr<acs::IRhiBuffer>   m_VbRight;  // 頂点空間 SSS 球
    acs::TUniquePtr<acs::IRhiBuffer>   m_FrameCb;  // b0: view_proj + light + tint
    acs::TUniquePtr<acs::IRhiBuffer>   m_ObjCbL;   // b1: model (左)
    acs::TUniquePtr<acs::IRhiBuffer>   m_ObjCbR;   // b1: model (右)
    acs::u32                          m_IndexCount = 0;

    acs::FSpriteBatch m_Batch;
    acs::Font        m_Font;
    acs::FCamera      m_Camera;
    acs::f32         m_Time = 0.0f;
};

} // namespace hellovertexsss
