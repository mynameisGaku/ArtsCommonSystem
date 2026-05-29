// SPDX-License-Identifier: Apache-2.0
// HelloColliderViz3D 実装。
#include "Viz3DApp.h"

#include "app/Sample.h"
#include "platform/Input.h"
#include "render/IRhiSwapchain.h"
#include "collision/ConvexHull3.h"
#include "asset/MeshPrimitive.h"
#include "asset/MeshAsset.h"
#include "memory/Rc.h"
#include "math/Mat.h"

#include <cstdio>

using namespace acs;

namespace viz3d {

void FViz3DApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }

    ACS_SAMPLE_INIT(m_Dd.Init(*dev, GetRenderer().ColorFormat()));
    ACS_SAMPLE_INIT(m_Batch.Init(*dev, GetRenderer().ColorFormat()));
    m_bFontReady = FSample::TryLoadDefaultUIFont(m_Font, *dev, 18.0f, 1024, false).IsOk();

    // メッシュ (球) を生成 → 位置 / インデックスを取り出す。
    TRc<FMeshAsset> mesh = Primitive::MakeSphere(1.0f, 20, 10);
    for (usize i = 0; i < mesh->Vertices().Size(); ++i) m_MeshPos.PushBack(mesh->Vertices()[i].position);
    for (usize i = 0; i < mesh->Indices().Size(); ++i)  m_MeshIdx.PushBack(mesh->Indices()[i]);

    ACS_SAMPLE_INIT(m_Collider.BuildFromMesh(*mesh));
    ACS_SAMPLE_INIT(BuildConvexHull3(m_MeshPos.Data(), static_cast<u32>(m_MeshPos.Size()),
                                     m_HullPos, m_HullIdx));
}

void FViz3DApp::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) { Quit(); return; }
    m_Time += (dt > 0.0f && dt < 0.1f) ? dt : (1.0f / 60.0f);
}

void FViz3DApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    IRhiSwapchain*   sc = GetRenderer().Swapchain();
    if (!cl || !sc) return;
    const u32 sw = sc->Width(), sh = sc->Height();
    const f32 aspect = sh > 0 ? static_cast<f32>(sw) / static_cast<f32>(sh) : 1.0f;

    const FMat4 view  = FMat4::LookAtLH(FVec3{ 3.0f, 2.2f, 3.4f }, FVec3{ 0, 0, 0 }, FVec3{ 0, 1, 0 });
    const FMat4 proj  = FMat4::PerspectiveFovLH(1.0472f, aspect, 0.1f, 100.0f);   // 60deg
    const FMat4 model = FMat4::RotationY(m_Time * 0.6f);
    const FMat4 mvp   = model * view * proj;   // 全描画を local 空間 → mvp で一括変換

    // local 空間でレイキャスト (上から下) → 命中点 (球の上端 y≈1)
    const Ray3 ray{ FVec3{ 0, 2.0f, 0 }, FVec3{ 0, -1, 0 } };
    const RayHit3 hit = m_Collider.Raycast(ray);

    m_Dd.Begin();
    m_Dd.Wireframe(m_HullPos.Data(), static_cast<u32>(m_HullPos.Size()),
                   m_HullIdx.Data(), static_cast<u32>(m_HullIdx.Size()),
                   FVec4{ 1.0f, 0.85f, 0.2f, 0.5f });                     // 凸包 (黄、半透明)
    m_Dd.Wireframe(m_MeshPos.Data(), static_cast<u32>(m_MeshPos.Size()),
                   m_MeshIdx.Data(), static_cast<u32>(m_MeshIdx.Size()),
                   FVec4{ 0.4f, 1.0f, 0.5f, 0.9f });                      // メッシュ (緑)
    m_Dd.Line(ray.origin, FVec3{ 0, -2.0f, 0 }, FVec4{ 0.3f, 0.9f, 1.0f, 1.0f });  // レイ (シアン)
    if (hit.hit) {
        const FVec3 p = hit.point;
        const f32 s = 0.12f;
        const FVec4 red{ 1.0f, 0.25f, 0.25f, 1.0f };
        m_Dd.Line(FVec3{ p.x - s, p.y, p.z }, FVec3{ p.x + s, p.y, p.z }, red);
        m_Dd.Line(FVec3{ p.x, p.y - s, p.z }, FVec3{ p.x, p.y + s, p.z }, red);
        m_Dd.Line(FVec3{ p.x, p.y, p.z - s }, FVec3{ p.x, p.y, p.z + s }, red);
    }
    m_Dd.End(*cl, mvp);

    if (m_bFontReady) {
        m_Batch.Begin(*cl, sw, sh);
        m_Batch.DrawString(m_Font,
            "ColliderViz3D  green=mesh wireframe  yellow=convex hull  cyan=ray  red=hit",
            24.0f, 22.0f, FVec4{ 0.95f, 0.96f, 1.0f, 1.0f });
        char info[96];
        std::snprintf(info, sizeof(info), "mesh tris=%u  hull tris=%u  raycast hit y=%.3f",
                      m_Collider.TriangleCount(),
                      static_cast<unsigned>(m_HullIdx.Size() / 3),
                      hit.hit ? hit.point.y : 0.0f);
        m_Batch.DrawString(m_Font, info, 24.0f, 46.0f, FVec4{ 0.7f, 0.8f, 0.95f, 1.0f });
        m_Batch.DrawString(m_Font, "Esc: quit", 24.0f, static_cast<f32>(sh) - 34.0f,
                           FVec4{ 0.6f, 0.62f, 0.7f, 1.0f });
        m_Batch.End();
    }
}

void FViz3DApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    if (m_bFontReady) m_Font.Shutdown();
    m_Batch.Shutdown();
    m_Dd.Shutdown();
}

} // namespace viz3d
