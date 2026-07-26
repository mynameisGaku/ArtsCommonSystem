// SPDX-License-Identifier: Apache-2.0
// HelloModel — ModelScene 実装。
#include "ModelScene.h"

#include "asset/MeshPrimitive.h"
#include "asset/MeshAsset.h"
#include "platform/Input.h"
#include "math/Mat.h"
#include "math/Math.h"
#include "foundation/Log.h"

using namespace acs;

namespace hellomodel {

bool FModelScene::Init(IRhiDevice& dev, f32 aspect) noexcept {
    // === プリミティブをアップロード ===
    auto sphere = Primitive::MakeSphere(0.8f, 48, 24);
    auto plane  = Primitive::MakePlane(20.0f, 20.0f);
    auto cube   = Primitive::MakeCube(0.6f);
    if (!sphere || !plane || !cube) return false;
    if (UploadMesh(dev, *sphere, m_GmSphere).IsErr()) return false;
    if (UploadMesh(dev, *plane,  m_GmPlane).IsErr())  return false;
    if (UploadMesh(dev, *cube,   m_GmCube).IsErr())   return false;

    // === カメラ ===
    m_Camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 200.0f);
    m_CamPos = kCamInitialPos;
    return true;
}

void FModelScene::Shutdown() noexcept {
    m_GmCube   = FGpuMesh{};
    m_GmPlane  = FGpuMesh{};
    m_GmSphere = FGpuMesh{};
}

void FModelScene::Update(f32 dt, FAssetFuture& async_mesh, bool& async_loaded) noexcept {
    m_Angle += dt * kSphereSpinSpeed;

    // === カメラ操作 ===
    const f32 move_speed = kCamMoveSpeed * dt;
    const f32 turn_speed = kCamTurnSpeed * dt;

    if (FInput::IsKeyDown(EKey::Left))  m_CamYaw -= turn_speed;
    if (FInput::IsKeyDown(EKey::Right)) m_CamYaw += turn_speed;
    if (FInput::IsKeyDown(EKey::Up))    m_CamPitch -= turn_speed * 0.8f;
    if (FInput::IsKeyDown(EKey::Down))  m_CamPitch += turn_speed * 0.8f;
    // 真上 / 真下を向くと forward の計算が破綻するため上下 81° 弱で頭打ち。
    const f32 limit = 0.45f * kPi;
    if (m_CamPitch >  limit) m_CamPitch =  limit;
    if (m_CamPitch < -limit) m_CamPitch = -limit;

    FVec3 forward{ Sin(m_CamYaw) * Cos(m_CamPitch),
                 -Sin(m_CamPitch),
                  Cos(m_CamYaw) * Cos(m_CamPitch) };
    FVec3 right{ Cos(m_CamYaw), 0, -Sin(m_CamYaw) };

    if (FInput::IsKeyDown(EKey::W)) m_CamPos += forward * move_speed;
    if (FInput::IsKeyDown(EKey::S)) m_CamPos -= forward * move_speed;
    if (FInput::IsKeyDown(EKey::D)) m_CamPos += right   * move_speed;
    if (FInput::IsKeyDown(EKey::A)) m_CamPos -= right   * move_speed;

    FVec3 target = m_CamPos + forward;
    m_Camera.SetLookAt(m_CamPos, target);

    // === 非同期ロード結果の確認 ===
    if (async_mesh.Valid() && !async_loaded && async_mesh.IsReady()) {
        auto r = async_mesh.Get();
        if (r.IsOk()) {
            ACS_LOG_INFO("Optional mesh loaded async OK");
        } else {
            ACS_LOG_INFO("Optional mesh not present (expected): %s",
                         r.Error().message);
        }
        async_loaded = true;
    }
}

void FModelScene::Render(FStandardShader& shader, IRhiCommandList& cl) noexcept {
    if (!shader.BeginFrame(2u + kCubeCount)) return;

    // Frame 共通設定（カメラ + 1 灯方向光 + 環境光）
    shader.SetFrame(m_Camera.ViewProjection(),
                    m_Camera.Eye(),
                    kLightDir,
                    kLightColor,
                    kAmbientColor);

    cl.SetPipeline(*shader.Pipeline());
    cl.SetConstantBuffer(0, *shader.PerFrameCB());
    cl.SetTexture(0, *shader.DefaultWhiteTexture());
    cl.SetTexture(1, *shader.ShadowTextureOrDefault());

    // ---- 中央の球 ----
    if (!shader.SetObject(FMat4::RotationY(m_Angle), kSphereColor)) return;
    cl.SetConstantBuffer(1, *shader.PerObjectCB());
    cl.SetVertexBuffer(*m_GmSphere.vertex_buffer, m_GmSphere.vertex_stride);
    cl.SetIndexBuffer(*m_GmSphere.index_buffer);
    cl.DrawIndexed(m_GmSphere.index_count);

    // ---- 地面プレーン ----
    if (!shader.SetObject(FMat4::Translation(FVec3{0, -0.8f, 0}), kPlaneColor)) return;
    cl.SetConstantBuffer(1, *shader.PerObjectCB());
    cl.SetVertexBuffer(*m_GmPlane.vertex_buffer, m_GmPlane.vertex_stride);
    cl.SetIndexBuffer(*m_GmPlane.index_buffer);
    cl.DrawIndexed(m_GmPlane.index_count);

    // ---- 周囲のキューブ ----
    cl.SetVertexBuffer(*m_GmCube.vertex_buffer, m_GmCube.vertex_stride);
    cl.SetIndexBuffer(*m_GmCube.index_buffer);
    for (u32 i = 0; i < kCubeCount; ++i) {
        const f32 a = i * (kPi * 0.5f) + m_Angle * 0.3f;
        const f32 r = 2.5f;
        FVec3 pos{ Sin(a) * r, -0.2f, Cos(a) * r };
        FMat4 m = FMat4::RotationY(m_Angle * 1.2f) * FMat4::Translation(pos);
        if (!shader.SetObject(m, kCubeColors[i])) continue;
        cl.SetConstantBuffer(1, *shader.PerObjectCB());
        cl.DrawIndexed(m_GmCube.index_count);
    }
}

} // namespace hellomodel
