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

class FDebugDraw3D {
public:
    FDebugDraw3D() noexcept = default;
    ~FDebugDraw3D() noexcept = default;
    FDebugDraw3D(const FDebugDraw3D&)            = delete;
    FDebugDraw3D& operator=(const FDebugDraw3D&) = delete;

    TResult<void> Init(IRhiDevice& device, EFormat rt_format, u32 max_lines = 16384) noexcept;
    void Shutdown() noexcept;

    void Begin() noexcept;
    void Line(FVec3 a, FVec3 b, FVec4 color) noexcept;
    void Aabb(const Aabb3& box, FVec4 color) noexcept;
    // 三角形インデックス列の全エッジを線で描く (メッシュ/凸包の wireframe)。
    void Wireframe(const FVec3* positions, u32 vertex_count,
                   const u32* indices, u32 index_count, FVec4 color) noexcept;
    // 現在 bind 中のターゲットに描画 (view_proj に model を畳んだ MVP を渡してもよい)。
    void End(IRhiCommandList& cl, const FMat4& view_proj) noexcept;

    u32 LineCount() const noexcept { return static_cast<u32>(m_Verts.Size() / 2); }

private:
    struct LineVtx { f32 px, py, pz, r, g, b, a; };

    TUniquePtr<IRhiShader>   m_Vs;
    TUniquePtr<IRhiShader>   m_Ps;
    TUniquePtr<IRhiPipeline> m_Pipeline;
    TUniquePtr<IRhiBuffer>   m_Vb;
    TUniquePtr<IRhiBuffer>   m_Cb;
    TArray<LineVtx>          m_Verts;
    u32                      m_MaxVerts = 0;
};

} // namespace acs
