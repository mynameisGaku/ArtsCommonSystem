// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Render — 集約ヘッダ
// -----------------------------------------------------------------------------
// 描画まわりでよく使うヘッダをまとめて include する便利ヘッダ。
// 個別に render/IRhiBuffer.h などを並べる代わりに、これ 1 行で済みます:
//
//     #include "render/Render.h"
//
// 含まれるもの:
//   - 低レベル RHI インターフェイス (IRhiDevice / IRhiBuffer / IRhiShader / ...)
//   - 高レベルヘルパ (CRenderer / CStandardShader / CSpriteBatch / FFont /
//     メッシュ・テクスチャのアップロード)
//
// PBR・スカイ・シャドウ・IBL・スキンメッシュなど特定機能のヘッダ
// (CPbrShader.h / CSky.h / CShadowMap.h / FIbl.h / CSkinnedShader.h ...) は、
// 使うサンプル側で必要なときに個別に include してください。
// =============================================================================
#pragma once

// ---- 低レベル RHI（バックエンド非依存の描画インターフェイス）----------------
#include "render/RhiTypes.h"
#include "render/IRhiDevice.h"
#include "render/IRhiSwapchain.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiBuffer.h"
#include "render/IRhiShader.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiTexture.h"
#include "render/IRhiSampler.h"

// ---- 高レベルヘルパ ---------------------------------------------------------
#include "render/Renderer.h"        // スワップチェイン + フレーム管理
#include "render/RenderAssets.h"    // UploadMesh / UploadTexture / FGpuMesh
#include "render/StandardShader.h"  // Lambert + 環境光の標準シェーダ（HLSL 不要）
#include "render/SpriteBatch.h"     // 2D スプライト / 矩形 / テキスト
#include "render/Font.h"            // TTF フォント
#include "render/WaterSurface3D.h"  // 3D 対話水面 (法線・屈折・Fresnel・永続波紋)
