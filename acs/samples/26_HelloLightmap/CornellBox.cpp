// SPDX-License-Identifier: Apache-2.0
// HelloLightmap — Cornell box シーン構築の実装。
#include "CornellBox.h"

#include "asset/MeshPrimitive.h"
#include "asset/MeshAsset.h"
#include "render/RenderAssets.h"
#include "math/Mat.h"
#include "math/Math.h"

using namespace acs;

namespace hellolightmap {

void InitQuad(IRhiDevice& dev, Quad& q, f32 w, f32 h,
              const FMat4& model, FVec3 albedo,
              i32 axis, f32 axis_value,
              f32 u_min, f32 u_max,
              f32 v_min, f32 v_max,
              bool emissive) noexcept {
    auto plane = Primitive::MakePlane(w, h);
    (void)UploadMesh(dev, *plane, q.mesh);
    q.model      = model;
    q.albedo     = albedo;
    q.plane_w    = w;
    q.plane_h    = h;
    q.normal     = Normalize(TransformVector(FVec3{0, 1, 0}, model));
    q.axis       = axis;
    q.axis_value = axis_value;
    q.u_min = u_min; q.u_max = u_max;
    q.v_min = v_min; q.v_max = v_max;
    q.emissive   = emissive;
}

void BuildCornellBox(IRhiDevice& dev, Quad (&quads)[kQuadCount]) noexcept {
    const FVec3 white{0.72f, 0.72f, 0.72f};
    const FVec3 red  {0.65f, 0.10f, 0.10f};
    const FVec3 green{0.10f, 0.55f, 0.12f};

    // model は Rotation * Translation の順 (ACS の row-major では「先に
    // 回転、次に平行移動」= ローカルで回転してから world 位置へ移動)。
    // MakePlane(w,h) は XZ 平面: ローカル u→+X, v→-Z。回転後の world 軸への
    // 写像を考慮して w/h を割り当てる (例: 壁は回転で v→Z, u→Y になる)。

    // 床: y=0、法線 +Y。回転なし。u→X(幅2), v→Z(奥行3)。
    InitQuad(dev, quads[0], 2.0f, 3.0f,
             FMat4::Translation(FVec3{0.0f, kBoxMinY, 0.5f}),
             white, /*axis(y)=*/1, kBoxMinY,
             kBoxMinX, kBoxMaxX, kBoxMinZ, kBoxMaxZ, false);
    // 天井: y=2、法線 -Y。X 軸 π 回転で裏返す。emissive = 光源。u→X, v→Z。
    InitQuad(dev, quads[1], 2.0f, 3.0f,
             FMat4::RotationX(kPi) * FMat4::Translation(FVec3{0.0f, kBoxMaxY, 0.5f}),
             white, /*axis(y)=*/1, kBoxMaxY,
             kBoxMinX, kBoxMaxX, kBoxMinZ, kBoxMaxZ, true);
    // 奥壁: z=2、法線 -Z。X 軸 -π/2 回転。u→X(幅2), v→Y(高さ2)。
    InitQuad(dev, quads[2], 2.0f, 2.0f,
             FMat4::RotationX(-kPi * 0.5f) * FMat4::Translation(FVec3{0.0f, 1.0f, kBoxMaxZ}),
             white, /*axis(z)=*/2, kBoxMaxZ,
             kBoxMinX, kBoxMaxX, kBoxMinY, kBoxMaxY, false);
    // 左壁: x=-1、法線 +X、赤。Z 軸 -π/2 回転。u→Y(高さ2), v→Z(奥行3)。
    InitQuad(dev, quads[3], 2.0f, 3.0f,
             FMat4::RotationZ(-kPi * 0.5f) * FMat4::Translation(FVec3{kBoxMinX, 1.0f, 0.5f}),
             red, /*axis(x)=*/0, kBoxMinX,
             kBoxMinY, kBoxMaxY, kBoxMinZ, kBoxMaxZ, false);
    // 右壁: x=1、法線 -X、緑。Z 軸 +π/2 回転。u→Y(高さ2), v→Z(奥行3)。
    InitQuad(dev, quads[4], 2.0f, 3.0f,
             FMat4::RotationZ(kPi * 0.5f) * FMat4::Translation(FVec3{kBoxMaxX, 1.0f, 0.5f}),
             green, /*axis(x)=*/0, kBoxMaxX,
             kBoxMinY, kBoxMaxY, kBoxMinZ, kBoxMaxZ, false);
}

} // namespace hellolightmap
