// SPDX-License-Identifier: Apache-2.0
// HelloColliderViz3D — 3D コライダーを目視確認するデモ。
// 回転するメッシュを wireframe (緑) で、その凸包を wireframe (黄) で重ね描き、
// メッシュコライダーへのレイキャスト (シアン) と命中点 (赤い十字) を表示する。
#pragma once

#include "app/Application.h"
#include "render/DebugDraw.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "collision/MeshCollider.h"
#include "container/Array.h"

namespace viz3d {

class FViz3DApp : public acs::FApplication {
public:
    void OnStart() noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender() noexcept override;
    void OnShutdown() noexcept override;

private:
    acs::FDebugDraw3D m_Dd;
    acs::FSpriteBatch m_Batch;
    acs::FFont         m_Font;
    bool              m_bFontReady = false;

    acs::TArray<acs::FVec3> m_MeshPos;
    acs::TArray<acs::u32>   m_MeshIdx;
    acs::TArray<acs::FVec3> m_HullPos;
    acs::TArray<acs::u32>   m_HullIdx;
    acs::CMeshCollider      m_Collider;

    acs::f32 m_Time = 0.0f;
};

} // namespace viz3d
