// SPDX-License-Identifier: Apache-2.0
// HelloAnimation — FApplication 派生クラス。
// 4 ボーンの「ヘビ風」スキンメッシュを GPU スキニングで描画するデモ。
//
// リソース所有 (sky / shader / sprite / font / camera / mesh / player) と
// 入力ハンドリングを担当し、毎フレームの「FSky → 地面 → スキンメッシュ」
// 描画は AnimationScene に委譲する。
#pragma once

#include "AnimationScene.h"

#include "app/Application.h"
#include "asset/SkinnedMesh.h"
#include "math/Camera.h"
#include "memory/SharedPtr.h"
#include "render/Font.h"
#include "render/RenderAssets.h"
#include "render/SkinnedShader.h"
#include "render/Sky.h"
#include "render/SpriteBatch.h"
#include "render/StandardShader.h"

namespace helloanim {

class FHelloAnimationApp : public acs::FApplication {
public:
    void OnStart()    noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;

private:
    FAnimationScene                m_Scene;
    acs::FSky                      m_Sky;
    acs::FSkinnedShader            m_Shader;       // スキンメッシュ用
    acs::FStandardShader           m_StdShader;   // 地面用
    acs::FSpriteBatch              m_Batch;
    acs::FFont                     m_Font;

    acs::TSharedPtr<acs::FSkinnedMeshAsset> m_Snake;
    acs::FSkinnedGpuMesh            m_GmSnake;
    acs::FGpuMesh                   m_GmPlane;
    acs::FAnimationPlayer           m_Player;

    acs::CCamera                   m_Camera;
    acs::FVec3                     m_CamPos;
    acs::f32                      m_CamYaw = 0.6f;
};

} // namespace helloanim
