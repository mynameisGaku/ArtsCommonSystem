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

bool ModelScene::Init(IRhiDevice& dev, f32 aspect) noexcept {
    // === プリミティブをアップロード ===
    auto sphere = Primitive::MakeSphere(0.8f, 48, 24);
    auto plane  = Primitive::MakePlane(20.0f, 20.0f);
    auto cube   = Primitive::MakeCube(0.6f);
    if (!sphere || !plane || !cube) return false;
    if (UploadMesh(dev, *sphere, _gm_sphere).IsErr()) return false;
    if (UploadMesh(dev, *plane,  _gm_plane).IsErr())  return false;
    if (UploadMesh(dev, *cube,   _gm_cube).IsErr())   return false;

    // === カメラ ===
    _camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 200.0f);
    _cam_pos = kCamInitialPos;
    return true;
}

void ModelScene::Shutdown() noexcept {
    _gm_cube   = FGpuMesh{};
    _gm_plane  = FGpuMesh{};
    _gm_sphere = FGpuMesh{};
}

void ModelScene::Update(f32 dt, FAssetFuture& async_mesh, bool& async_loaded) noexcept {
    _angle += dt * kSphereSpinSpeed;

    // === カメラ操作 ===
    const f32 move_speed = kCamMoveSpeed * dt;
    const f32 turn_speed = kCamTurnSpeed * dt;

    if (FInput::IsKeyDown(EKey::Left))  _cam_yaw -= turn_speed;
    if (FInput::IsKeyDown(EKey::Right)) _cam_yaw += turn_speed;
    if (FInput::IsKeyDown(EKey::Up))    _cam_pitch -= turn_speed * 0.8f;
    if (FInput::IsKeyDown(EKey::Down))  _cam_pitch += turn_speed * 0.8f;
    // 真上 / 真下を向くと forward の計算が破綻するため上下 81° 弱で頭打ち。
    const f32 limit = 0.45f * kPi;
    if (_cam_pitch >  limit) _cam_pitch =  limit;
    if (_cam_pitch < -limit) _cam_pitch = -limit;

    FVec3 forward{ Sin(_cam_yaw) * Cos(_cam_pitch),
                 -Sin(_cam_pitch),
                  Cos(_cam_yaw) * Cos(_cam_pitch) };
    FVec3 right{ Cos(_cam_yaw), 0, -Sin(_cam_yaw) };

    if (FInput::IsKeyDown(EKey::W)) _cam_pos += forward * move_speed;
    if (FInput::IsKeyDown(EKey::S)) _cam_pos -= forward * move_speed;
    if (FInput::IsKeyDown(EKey::D)) _cam_pos += right   * move_speed;
    if (FInput::IsKeyDown(EKey::A)) _cam_pos -= right   * move_speed;

    FVec3 target = _cam_pos + forward;
    _camera.SetLookAt(_cam_pos, target);

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

void ModelScene::Render(FStandardShader& shader, IRhiCommandList& cl) noexcept {
    // Frame 共通設定（カメラ + 1 灯方向光 + 環境光）
    shader.SetFrame(_camera.ViewProjection(),
                    _camera.Eye(),
                    kLightDir,
                    kLightColor,
                    kAmbientColor);

    cl.SetPipeline(*shader.Pipeline());
    cl.SetConstantBuffer(0, *shader.PerFrameCB());
    cl.SetConstantBuffer(1, *shader.PerObjectCB());
    cl.SetTexture(0, *shader.DefaultWhiteTexture());
    cl.SetTexture(1, *shader.ShadowTextureOrDefault());

    // ---- 中央の球 ----
    shader.SetObject(FMat4::RotationY(_angle), kSphereColor);
    cl.SetVertexBuffer(*_gm_sphere.vertex_buffer, _gm_sphere.vertex_stride);
    cl.SetIndexBuffer(*_gm_sphere.index_buffer);
    cl.DrawIndexed(_gm_sphere.index_count);

    // ---- 地面プレーン ----
    shader.SetObject(FMat4::Translation(FVec3{0, -0.8f, 0}), kPlaneColor);
    cl.SetVertexBuffer(*_gm_plane.vertex_buffer, _gm_plane.vertex_stride);
    cl.SetIndexBuffer(*_gm_plane.index_buffer);
    cl.DrawIndexed(_gm_plane.index_count);

    // ---- 周囲のキューブ ----
    cl.SetVertexBuffer(*_gm_cube.vertex_buffer, _gm_cube.vertex_stride);
    cl.SetIndexBuffer(*_gm_cube.index_buffer);
    for (u32 i = 0; i < kCubeCount; ++i) {
        const f32 a = i * (kPi * 0.5f) + _angle * 0.3f;
        const f32 r = 2.5f;
        FVec3 pos{ Sin(a) * r, -0.2f, Cos(a) * r };
        FMat4 m = FMat4::RotationY(_angle * 1.2f) * FMat4::Translation(pos);
        shader.SetObject(m, kCubeColors[i]);
        cl.DrawIndexed(_gm_cube.index_count);
    }
}

} // namespace hellomodel
