// SPDX-License-Identifier: Apache-2.0
// 2D 動的ライティング + ソフト影 (Core Keeper 風) と簡易ブロブ影。
//
// FLighting2D:
//   トップダウン/横スクロール 2D 向けの動的ライト。複数のカラー点光源を
//   持ち、occluder (影を落とすもの) のシルエットからソフト影を落とす。
//   ambient を低く (暗い洞窟) しておき、光源の周りだけが照らされる Core Keeper
//   のような表現を狙う。
//
//   描画フロー (FApplication::OnCustomFrame 内):
//     li.BeginScene(cl);                       // 内部 scene RT を bind + clear
//       sb.Begin(cl,w,h); ...world を描画...; sb.End();
//     li.EndScene(cl);
//     li.BeginOccluders(cl);                   // occluder RT を黒 clear
//       sb.Begin(cl,w,h); ...影を落とすスプライトを白 tint で描画...; sb.End();
//     li.EndOccluders(cl);                     // → シルエットが occluder mask に
//     li.SetAmbient({0.06,0.06,0.09});
//     li.ClearLights();
//     li.AddLight({ {px,py}, radius, {r,g,b}, intensity, softness });
//     cl.BeginRenderToSwapchain(sc, idx, clear);
//       li.Composite(cl, w, h);                // scene × (ambient + Σ light·影) を backbuffer へ
//       sb.Begin(...); ...HUD...; sb.End();
//     cl.EndRenderToSwapchain(sc, idx);
//
//   occluder は「スプライトのシルエット」をそのまま使う (白 tint + alpha blend で
//   黒地に焼くと、不透明部分が occluder になる) ので、矩形でなくスプライト形状に
//   沿った影が落ちる。影は occluder mask を linear sample + 複数レイ (面光源近似)
//   で柔らかくする。
//
// FBlobShadow:
//   光源計算を伴わない激軽の「足元の影」。柔らかい楕円テクスチャを 1 枚持ち、
//   SpriteBatch で暗く落とすだけ。動的ライトを使わない/負荷を抑えたい時の fallback。
//
// ACS 規約: STL/<string> 不使用、全 noexcept、TResult、非コピー。
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "math/Vec.h"
#include "render/IRhiDevice.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiTexture.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiShader.h"
#include "render/IRhiBuffer.h"
#include "render/RhiTypes.h"

namespace acs {

class FSpriteBatch;

// 1 個の 2D 点光源。座標はスプライトと同じピクセル空間 (左上原点)。
struct FLight2D {
    FVec2 pos{0.0f, 0.0f};     // 光源位置 (px)
    f32   radius   = 256.0f;   // 届く半径 (px)
    FVec3 color{1.0f, 1.0f, 1.0f};
    f32   intensity = 1.0f;    // 明るさ倍率
    f32   softness  = 0.5f;    // 0=くっきり影, 1=とても柔らかい penumbra
};

class FLighting2D {
public:
    static constexpr u32 kMaxLights = 16;

    FLighting2D() noexcept = default;
    ~FLighting2D() noexcept = default;
    FLighting2D(const FLighting2D&)            = delete;
    FLighting2D& operator=(const FLighting2D&) = delete;

    // color_format = backbuffer の色フォーマット (scene/occluder RT と composite
    // 出力をこれに合わせる)。width/height は初期サイズ。
    TResult<void> Init(IRhiDevice& device, EFormat color_format,
                       u32 width, u32 height) noexcept;
    void Shutdown() noexcept;
    TResult<void> Resize(u32 width, u32 height) noexcept;

    void SetAmbient(FVec3 ambient) noexcept { m_Ambient = ambient; }
    void ClearLights() noexcept { m_LightCount = 0; }
    // 追加成功で true、上限 (kMaxLights) 到達で false。
    bool AddLight(const FLight2D& light) noexcept;
    u32  LightCount() const noexcept { return m_LightCount; }

    // 影の品質。march_steps = 1 レイあたりの行進数、ray_count = 面光源近似の
    // レイ本数 (多いほど penumbra が滑らか/重い)。
    void SetShadowQuality(u32 march_steps, u32 ray_count) noexcept;

    // scene (世界の albedo) 描画 bracket。
    void BeginScene(IRhiCommandList& cl, FVec4 clear = FVec4{0, 0, 0, 1}) noexcept;
    void EndScene(IRhiCommandList& cl) noexcept;

    // occluder (影を落とすスプライト) 描画 bracket。黒 clear → 白 tint で描く。
    void BeginOccluders(IRhiCommandList& cl) noexcept;
    void EndOccluders(IRhiCommandList& cl) noexcept;

    // 現在 bind 中のターゲット (通常 backbuffer) に scene × light を合成する。
    // 呼ぶ前に BeginRenderToSwapchain 等でターゲットを bind しておくこと。
    void Composite(IRhiCommandList& cl, u32 screen_w, u32 screen_h) noexcept;

    IRhiTexture* SceneTexture() const noexcept { return m_SceneRt.Get(); }
    IRhiTexture* OccluderTexture() const noexcept { return m_OccluderRt.Get(); }

private:
    TResult<void> CreateTargets(IRhiDevice& device, u32 w, u32 h) noexcept;
    TResult<void> CreatePipeline(IRhiDevice& device) noexcept;

    IRhiDevice* m_Device      = nullptr;
    EFormat     m_ColorFormat = EFormat::B8G8R8A8_UNorm;
    u32         m_Width       = 0;
    u32         m_Height      = 0;

    TUniquePtr<IRhiTexture>  m_SceneRt;       // 世界の albedo
    TUniquePtr<IRhiTexture>  m_OccluderRt;   // 影を落とすシルエット (白=遮蔽)
    TUniquePtr<IRhiShader>   m_Vs;
    TUniquePtr<IRhiShader>   m_Ps;
    TUniquePtr<IRhiPipeline> m_Pipeline;
    TUniquePtr<IRhiBuffer>   m_Cb;

    FVec3     m_Ambient{0.08f, 0.08f, 0.10f};
    FLight2D  m_Lights[kMaxLights]{};
    u32       m_LightCount  = 0;
    u32       m_MarchSteps  = 24;
    u32       m_RayCount    = 6;
};

// 光源計算なしの簡易ブロブ影 (足元の楕円)。動的ライトの fallback / 補助。
class FBlobShadow {
public:
    FBlobShadow() noexcept = default;
    ~FBlobShadow() noexcept = default;
    FBlobShadow(const FBlobShadow&)            = delete;
    FBlobShadow& operator=(const FBlobShadow&) = delete;

    // 柔らかい放射状グラデーションのテクスチャ (resolution²) を 1 枚作る。
    TResult<void> Init(IRhiDevice& device, u32 resolution = 64) noexcept;
    void Shutdown() noexcept;

    // (cx,cy) 中心に w×h の柔らかい影を sb 経由で描く (要 sb.Begin 済み)。
    // alpha は影の濃さ。color で色付きも可 (既定は黒)。
    void Draw(FSpriteBatch& sb, f32 cx, f32 cy, f32 w, f32 h,
              f32 alpha = 0.5f, FVec3 color = FVec3{0, 0, 0}) noexcept;

    IRhiTexture* Texture() const noexcept { return m_Tex.Get(); }

private:
    TUniquePtr<IRhiTexture> m_Tex;
};

} // namespace acs
