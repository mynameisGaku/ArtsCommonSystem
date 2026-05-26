// SPDX-License-Identifier: Apache-2.0
// HelloRaycast3D — RaycastScene 実装。
#include "RaycastScene.h"

#include "asset/MeshPrimitive.h"
#include "asset/MeshAsset.h"
#include "platform/Input.h"
#include "math/Mat.h"
#include "math/Math.h"
#include "math/Collision3D.h"
#include "foundation/Log.h"

#include <cstdio>

using namespace acs;

namespace helloraycast3d {

bool RaycastScene::Init(IRhiDevice& dev, f32 aspect) noexcept {
    auto sphere = Primitive::MakeSphere(1.0f, 32, 16);  // 半径 1 で作っておき、SetObject で Scale
    auto cube   = Primitive::MakeCube(1.0f);             // 半サイズ 0.5 のキューブ
    auto plane  = Primitive::MakePlane(40.0f, 40.0f);
    if (!sphere || !cube || !plane) return false;
    if (UploadMesh(dev, *sphere, _gm_sphere).IsErr()) return false;
    if (UploadMesh(dev, *cube,   _gm_cube).IsErr())   return false;
    if (UploadMesh(dev, *plane,  _gm_plane).IsErr())  return false;

    // === オブジェクト配置（円状）===
    for (u32 i = 0; i < kNumObjects; ++i) {
        const f32 a = (kPi * 2.0f) * static_cast<f32>(i) / kNumObjects;
        Object& o = _objects[i];
        o.kind  = (i & 1) ? ShapeKind::Sphere : ShapeKind::Cube;
        o.position = { Sin(a) * kObjectArenaR,
                       1.0f + (i & 2 ? 1.5f : 0.0f),
                       Cos(a) * kObjectArenaR };
        o.radius_or_half = kObjectRadius;
        // 虹色配置
        const f32 hue = static_cast<f32>(i) / kNumObjects;
        o.base_color = HsvToRgb(hue, 0.6f, 0.95f);
    }

    _camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 200.0f);
    _cam_pos = kCamInitialPos;
    return true;
}

void RaycastScene::Shutdown() noexcept {
    _gm_plane  = GpuMesh{};
    _gm_cube   = GpuMesh{};
    _gm_sphere = GpuMesh{};
}

void RaycastScene::Update(f32 dt) noexcept {
    _time += dt;

    // カメラ操作
    const f32 move_speed = kCamMoveSpeed * dt;
    const f32 turn_speed = kCamTurnSpeed * dt;
    if (Input::IsKeyDown(EKey::Left))  _cam_yaw -= turn_speed;
    if (Input::IsKeyDown(EKey::Right)) _cam_yaw += turn_speed;
    if (Input::IsKeyDown(EKey::Up))    _cam_pitch -= turn_speed * 0.8f;
    if (Input::IsKeyDown(EKey::Down))  _cam_pitch += turn_speed * 0.8f;
    const f32 limit = 0.45f * kPi;
    if (_cam_pitch >  limit) _cam_pitch =  limit;
    if (_cam_pitch < -limit) _cam_pitch = -limit;

    Vec3 forward{ Sin(_cam_yaw) * Cos(_cam_pitch),
                 -Sin(_cam_pitch),
                  Cos(_cam_yaw) * Cos(_cam_pitch) };
    Vec3 right{ Cos(_cam_yaw), 0, -Sin(_cam_yaw) };
    if (Input::IsKeyDown(EKey::W)) _cam_pos += forward * move_speed;
    if (Input::IsKeyDown(EKey::S)) _cam_pos -= forward * move_speed;
    if (Input::IsKeyDown(EKey::D)) _cam_pos += right   * move_speed;
    if (Input::IsKeyDown(EKey::A)) _cam_pos -= right   * move_speed;

    _cam_forward = forward;
    _camera.SetLookAt(_cam_pos, _cam_pos + forward);

    // === 前方レイキャストで最近傍ヒットを探す ===
    Ray3 ray{ _cam_pos, forward };
    _hit_index = -1;
    f32 best_t = 1000.0f;
    for (u32 i = 0; i < kNumObjects; ++i) {
        const Object& o = _objects[i];
        RayHit3 h{};
        if (o.kind == ShapeKind::Sphere) {
            Sphere s{ o.position, o.radius_or_half };
            h = RaycastSphere(ray, s, best_t);
        } else {
            Aabb3 box = Aabb3::FromCenterExtents(
                o.position,
                Vec3{o.radius_or_half, o.radius_or_half, o.radius_or_half});
            h = RaycastAabb(ray, box, best_t);
        }
        if (h.hit && h.t < best_t) {
            best_t = h.t;
            _hit_index = static_cast<i32>(i);
            _hit_point = h.point;
        }
    }
}

void RaycastScene::Render(StandardShader& shader,
                          IRhiCommandList& cl,
                          SpriteBatch& batch,
                          Font& font,
                          u32 screen_w,
                          u32 screen_h) noexcept {
    // === マルチライト設定 ===
    DirLight lights[2];
    lights[0].direction = Vec3{ 0.5f, 0.8f, 0.3f };  // 暖色キーライト
    lights[0].color     = Vec3{ 1.0f, 0.9f, 0.7f };
    lights[1].direction = Vec3{-0.4f, 0.5f,-0.7f };  // 寒色フィル
    lights[1].color     = Vec3{ 0.3f, 0.4f, 0.6f };
    shader.SetLights(_camera.ViewProjection(), _camera.Eye(),
                     lights, 2, kAmbient);

    cl.SetPipeline(*shader.Pipeline());
    cl.SetConstantBuffer(0, *shader.PerFrameCB());
    cl.SetConstantBuffer(1, *shader.PerObjectCB());
    cl.SetTexture(0, *shader.DefaultWhiteTexture());
    cl.SetTexture(1, *shader.ShadowTextureOrDefault());

    // === 地面 ===
    shader.SetObject(Mat4::Translation(Vec3{0, 0, 0}),
                     kPlaneColor, 0.0f, 1.0f);
    cl.SetVertexBuffer(*_gm_plane.vertex_buffer, _gm_plane.vertex_stride);
    cl.SetIndexBuffer(*_gm_plane.index_buffer);
    cl.DrawIndexed(_gm_plane.index_count);

    // === 各オブジェクト ===
    for (u32 i = 0; i < kNumObjects; ++i) {
        const Object& o = _objects[i];
        const bool selected = (static_cast<i32>(i) == _hit_index);
        const Vec3 col = selected
            ? kSelectedColor          // ヒット中は白で目立たせる
            : o.base_color;
        const f32 spec  = selected ? 0.9f : 0.3f;
        const f32 shine = selected ? 64.0f : 16.0f;

        const Mat4 m = Mat4::Scale(Vec3{o.radius_or_half, o.radius_or_half, o.radius_or_half}) *
                       Mat4::Translation(o.position);
        shader.SetObject(m, col, spec, shine);

        if (o.kind == ShapeKind::Sphere) {
            cl.SetVertexBuffer(*_gm_sphere.vertex_buffer, _gm_sphere.vertex_stride);
            cl.SetIndexBuffer(*_gm_sphere.index_buffer);
            cl.DrawIndexed(_gm_sphere.index_count);
        } else {
            cl.SetVertexBuffer(*_gm_cube.vertex_buffer, _gm_cube.vertex_stride);
            cl.SetIndexBuffer(*_gm_cube.index_buffer);
            cl.DrawIndexed(_gm_cube.index_count);
        }
    }

    // === 2D HUD: 中央のクロスヘア + 当たり情報 ===
    if (font.AtlasTexture()) {
        batch.Begin(cl, screen_w, screen_h);

        // クロスヘア（中央 12x2 + 2x12）
        const f32 cx = screen_w * 0.5f, cy = screen_h * 0.5f;
        batch.DrawRect(cx - 8, cy - 1, 16, 2, Vec4{1, 1, 1, 0.8f});
        batch.DrawRect(cx - 1, cy - 8, 2, 16, Vec4{1, 1, 1, 0.8f});

        // ヒット情報
        char buf[160];
        if (_hit_index >= 0) {
            std::snprintf(buf, sizeof(buf),
                          "HIT: object[%d] at (%.2f, %.2f, %.2f)",
                          _hit_index, _hit_point.x, _hit_point.y, _hit_point.z);
            batch.DrawString(font, buf, 20, 20, Vec4{1.0f, 0.9f, 0.4f, 1});
        } else {
            batch.DrawString(font, "(no hit)", 20, 20, Vec4{0.7f, 0.7f, 0.7f, 1});
        }
        batch.DrawString(font,
                        "WASD: 移動  矢印: 視点  Esc: 終了",
                        20, 44, Vec4{0.7f, 0.8f, 0.9f, 1});
        batch.End();
    }
}

Vec3 RaycastScene::HsvToRgb(f32 h, f32 s, f32 v) noexcept {
    h = h - static_cast<i32>(h);
    if (h < 0) h += 1.0f;
    const f32 c = v * s;
    const f32 hp = h * 6.0f;
    const f32 x = c * (1.0f - Abs(hp - 2.0f * static_cast<i32>(hp / 2.0f) - 1.0f));
    f32 r = 0, g = 0, b = 0;
    const i32 i = static_cast<i32>(hp);
    switch (i % 6) {
        case 0: r = c; g = x; b = 0; break;
        case 1: r = x; g = c; b = 0; break;
        case 2: r = 0; g = c; b = x; break;
        case 3: r = 0; g = x; b = c; break;
        case 4: r = x; g = 0; b = c; break;
        case 5: r = c; g = 0; b = x; break;
    }
    const f32 m = v - c;
    return { r + m, g + m, b + m };
}

} // namespace helloraycast3d
