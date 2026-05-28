// SPDX-License-Identifier: Apache-2.0
// HelloLight2D — 2D 動的ライト + ソフト影 (Core Keeper 風) と簡易ブロブ影のデモ。
//
// ・暗い洞窟 (低 ambient) に複数のカラー点光源。マウスが「松明」になり、
//   柱 (occluder) のシルエットに沿った柔らかい影が落ちる。
// ・[B] でブロブ影のみの軽量モードに切替 (動的影 OFF / フラット照明)。
//
// 影は FLighting2D が occluder mask を ray-march して出す。occluder は
// 「柱スプライトを白で焼いたシルエット」なので矩形でなく丸い影になる。
#pragma once

#include "app/Application.h"
#include "render/SpriteBatch.h"
#include "render/Light2D.h"
#include "render/Font.h"
#include "memory/UniquePtr.h"

namespace hellolight2d {

class FLight2DApp : public acs::FApplication {
public:
    void OnStart() noexcept override;
    bool OnCustomFrame() noexcept override;
    void OnShutdown() noexcept override;

private:
    acs::TResult<void> BuildTextures(acs::IRhiDevice& device) noexcept;

    // scene パス: 床タイル + 柱 + キャラ + (キャラ足元の) ブロブ影。
    void DrawWorld(acs::u32 w, acs::u32 h) noexcept;
    // occluder パス: 影を落とすもの (柱 + キャラ) を白で描く。
    void DrawOccluders() noexcept;
    // ambient + 光源を毎フレーム更新。
    void UpdateLights(acs::u32 w, acs::u32 h) noexcept;

    acs::FSpriteBatch m_SceneBatch;   // パスごとに別 batch (VB エイリアス回避)
    acs::FSpriteBatch m_OccBatch;
    acs::FSpriteBatch m_HudBatch;
    acs::FLighting2D  m_Lighting;
    acs::FBlobShadow  m_Blob;
    acs::Font         m_Font;
    bool              m_bFontReady = false;

    acs::TUniquePtr<acs::IRhiTexture> m_FloorTex;
    acs::TUniquePtr<acs::IRhiTexture> m_PillarTex;
    acs::TUniquePtr<acs::IRhiTexture> m_CharTex;

    acs::u32  m_Width  = 0;
    acs::u32  m_Height = 0;
    acs::f32  m_Time   = 0.0f;
    bool      m_bBlobOnly = false;

    acs::FVec2 m_CharPos{0, 0};       // 自動で歩き回るキャラ
    acs::FVec2 m_TorchPos{0, 0};      // マウス追従の松明
};

} // namespace hellolight2d
