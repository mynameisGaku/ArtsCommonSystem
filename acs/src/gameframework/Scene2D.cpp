// SPDX-License-Identifier: Apache-2.0
#include "gameframework/Scene2D.h"

#include "gameframework/RenderContext.h"
#include "gameframework/SceneServices.h"
#include "gameframework/Camera2D.h"
#include "gameframework/Game.h"
#include "gameframework/PolygonRenderer2D.h"
#include "gameframework/Spawn2DSubsystem.h"
#include "render/Renderer.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiSwapchain.h"
#include "render/IRhiTexture.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

#include <cmath>   // std::cos / std::sin (三角形オクルーダーの頂点算出)
#include <cstring> // std::strcmp

namespace acs::game {

/** World サブシステムを root へ公開し、2D 専用生成先を型付きで接続する。 */
void AScene2D::_OnWorldSubsystemsReady() noexcept
{
    m_Root->_SetSubsystems(_WorldSubsystemsPtr());
    ASpawn2DSubsystem* const Spawner = GetSubsystem<ASpawn2DSubsystem>();
    if (Spawner != nullptr) Spawner->BindTargetRoot(m_Root.Get());
}

/** シーン入場時に root へ services を配線し、派生の初期化フック OnReady を呼ぶ。 */
void AScene2D::OnEnter() noexcept {
    // OnReady より «前» に配線 → OnReady 中に AddComponent された分は SceneServices() が解決でき
    // 即時 OnAttachServices 発火する。OnReady 後の _ActivateServices は配線前に在ったものへの catch-up。
    if (HasServices()) m_Root->_SetSceneServices(_ServicesOrNull());
    OnReady();
    if (HasServices()) m_Root->_ActivateServices(Services());
}

/** シーン退場時に構造変更を解決し、物理・トゥイーン・スプライトバッチを後始末する。 */
void AScene2D::OnExit() noexcept {
    m_Root->ResolveStructuralChanges();
    if (HasServices()) {
        Services().Physics().ClearAll();
        Services().Tweens().CancelAll();
    }
    m_Sprites.Shutdown();
    m_SpritesReady = false;
    m_SceneSprites.Shutdown();
    m_SceneSpritesReady = false;
    m_WaterDepthSprites.Shutdown();
    m_WaterDepthSpritesReady = false;
}

/** 毎フレーム OnTick → root の UpdateTree → 構造変更解決を実行する。 */
void AScene2D::OnUpdate(f32 dt) noexcept {
    OnTick(dt);
    m_Root->UpdateTree(dt);
    m_Root->ResolveStructuralChanges();
}

/** 固定刻みで OnFixedTick → root の FixedUpdateTree → 構造変更解決を実行する。 */
void AScene2D::OnFixedUpdate(f32 fixed_dt) noexcept {
    OnFixedTick(fixed_dt);
    m_Root->FixedUpdateTree(fixed_dt);
    m_Root->ResolveStructuralChanges();
}

/** 共有 FSpriteBatch を遅延初期化する (成功で true)。 */
bool AScene2D::EnsureSpriteBatch(FRenderContext& rc) noexcept {
    if (m_SpritesReady) return true;
    FRenderer& renderer = rc.GetRenderer();
    IRhiDevice* device = renderer.Device();
    if (!device) return false;
    const auto r = m_Sprites.Init(*device, renderer.ColorFormat(), 8192);
    if (r.IsErr()) {
        ACS_LOG_ERROR("FScene2D: FSpriteBatch init failed");
        return false;
    }
    m_SpritesReady = true;
    return true;
}

/** 反射オフスクリーン pass 専用の別 FSpriteBatch を遅延初期化する (成功で true)。 */
bool AScene2D::EnsureSceneSprites(FRenderContext& rc) noexcept {
    // 反射のオフスクリーン pass 専用の SpriteBatch。オフスクリーン pass と
    // 合成 pass で同一バッチを使うと、頂点/定数バッファがフレーム内で上書きし
    // 合い、オフスクリーン pass の遅延 draw が合成 pass のデータを読んでしまう
    // (RT = 反射元が壊れる)。別バッチに分離して両パスが GPU バッファを共有
    // しないようにする。
    if (m_SceneSpritesReady) return true;
    FRenderer& renderer = rc.GetRenderer();
    IRhiDevice* device = renderer.Device();
    if (!device) return false;
    const auto r = m_SceneSprites.Init(*device, renderer.ColorFormat(), 8192);
    if (r.IsErr()) {
        ACS_LOG_ERROR("FScene2D: 反射用 FSpriteBatch init failed");
        return false;
    }
    m_SceneSpritesReady = true;
    return true;
}

/** 水面深度捕捉 pass 専用の別 FSpriteBatch を遅延初期化する (成功で true)。 */
bool AScene2D::EnsureWaterDepthSprites(FRenderContext& rc) noexcept {
    // scene-color pass と同じバッチを使うと、同一フレーム内の VB/CB 更新が先行 draw の
    // 内容を上書きする。depth pass も独立バッチにして GPU 実行まで各データを保持する。
    if (m_WaterDepthSpritesReady) return true;
    FRenderer& renderer = rc.GetRenderer();
    IRhiDevice* device = renderer.Device();
    if (!device) return false;
    const auto r = m_WaterDepthSprites.Init(*device, renderer.ColorFormat(), 8192);
    if (r.IsErr()) {
        ACS_LOG_ERROR("FScene2D: 水面深度用 FSpriteBatch init failed");
        return false;
    }
    m_WaterDepthSpritesReady = true;
    return true;
}

/** 画面ピクセル座標をワールド座標へ変換する (camera 中心・zoom・ppu を逆適用)。 */
FVec2 AScene2D::ScreenToWorld(FVec2 screen_px) noexcept {
    FVec2 vc{0.0f, 0.0f};
    f32   zoom = 1.0f;
    if (HasServices()) {
        CCamera2D& cam = Services().Camera();
        vc   = cam.EffectiveViewCenter();
        zoom = cam.Zoom();
    }
    const f32 scale = m_PixelsPerUnit * zoom;
    const f32 inv   = scale > 1e-6f ? 1.0f / scale : 0.0f;
    return FVec2{ vc.x + (screen_px.x - static_cast<f32>(m_ScreenW) * 0.5f) * inv,
                  vc.y + (screen_px.y - static_cast<f32>(m_ScreenH) * 0.5f) * inv };
}

/** ノードツリーを走査して 2D ライト (ALight2DComponent) と影オクルーダー
 *  (AShadowCaster2DComponent) を収集する。座標は world (runtime は SetView で zoom 処理)。 */
static void CollectLightsAndOccluders(ANode& node,
                                      FSpriteLight* lights, u32& lc,
                                      FSpriteOccluder* occ, u32& oc, bool& has_lit) noexcept {
    if (!node.IsVisible()) return;
    if (node.IsLitMaterial()) has_lit = true;   // lit ノードがあれば SetLights が要る
    node.SetSelfOccluder(-1);                   // 毎フレームリセット (影源でなくなった場合に備える)
    const FTransform2D w = node.World2D();
    // プリミティブ形状を先に拾う (影オクルーダーを «見た目の形» に合わせる)。half_size=m_Size*0.5=base*0.5。
    i32 primShape = -1; FVec2 primHalf{ 24.0f, 24.0f };
    for (u32 pi = 0; pi < node.ComponentCount(); ++pi) {
        const AComponent* pc = node.ComponentAt(pi);
        if (pc != nullptr && pc->QueryPrimitive(primShape, primHalf)) break;
    }
    for (u32 i = 0; i < node.ComponentCount(); ++i) {
        const AComponent* c = node.ComponentAt(i);
        if (c == nullptr) continue;
        FLightDesc2D ld; f32 rscale = 0; i32 oshape = 0; bool self_sh = false;
        if (lc < 16 && c->QueryLight(ld)) {
            FSpriteLight& L = lights[lc++];
            L.pos = w.position;     L.radius = ld.radius;
            L.color = ld.color;     L.intensity = ld.intensity;
            L.type = ld.type;       L.dir = ld.dir;
            L.coneInner = ld.coneInner; L.coneOuter = ld.coneOuter;
        }
        if (oc < 16 && c->QueryShadowCaster(rscale, oshape, self_sh)) {
            // 自分自身のオクルーダー番号を記録。既定 (self_shadow=false) は描画時の自己影
            // スキップ用にそのまま、自己影有効時は -(oc+2) でエンコードして «内部=umbra»
            // 扱いを DrawTree へ伝える (-1 は «オクルーダー無し» のため使えない)。
            node.SetSelfOccluder(self_sh ? -(static_cast<i32>(oc) + 2) : static_cast<i32>(oc));
            FSpriteOccluder& O = occ[oc++];
            const f32 hx = (primShape >= 0 ? primHalf.x : 24.0f) * w.scale.x * rscale;
            const f32 hy = (primShape >= 0 ? primHalf.y : 24.0f) * w.scale.y * rscale;
            O.center = w.position;
            O.radius = (hx + hy) * 0.5f;                  // 外接半径 (world)
            O.halfExtents = FVec2{ hx, hy };
            O.rotation = w.rotation;
            const f32 cph = std::cos(w.rotation), sph = std::sin(w.rotation);
            if (primShape == 2) {                         // 三角形 → 3頂点ポリゴン (DrawShape と同式・world)
                O.shape = 2; O.polyCount = 3;
                O.polyVerts[0] = FVec2{ w.position.x + hy * sph,            w.position.y - hy * cph };
                O.polyVerts[1] = FVec2{ w.position.x - hx * cph - hy * sph, w.position.y - hx * sph + hy * cph };
                O.polyVerts[2] = FVec2{ w.position.x + hx * cph - hy * sph, w.position.y + hx * sph + hy * cph };
            } else if (primShape == 3) {                  // ポリゴン → APolygonRenderer2D の頂点を world 変換
                O.shape = 2;
                // APolygonRenderer2D を見つけてローカル頂点を取得
                u32 vc = 0;
                FVec2 localVerts[APolygonRenderer2D::kMaxVerts];
                for (u32 pi = 0; pi < node.ComponentCount(); ++pi) {
                    AComponent* pc = node.ComponentAt(pi);
                    if (pc != nullptr && pc->ReflectName() != nullptr
                        && std::strcmp(pc->ReflectName(), "APolygonRenderer2D") == 0) {
                        const APolygonRenderer2D* poly = static_cast<const APolygonRenderer2D*>(pc);
                        vc = poly->VertCount();
                        for (u32 k = 0; k < vc; ++k) localVerts[k] = poly->Vert(k);
                        break;
                    }
                }
                if (vc >= 3) {
                    const u32 maxV = (vc > kMaxOccPolyVerts) ? kMaxOccPolyVerts : vc;
                    O.polyCount = static_cast<i32>(maxV);
                    for (u32 k = 0; k < maxV; ++k) {
                        const u32 si = (vc > kMaxOccPolyVerts) ? (k * vc) / maxV : k;
                        const f32 lx = localVerts[si].x * w.scale.x;
                        const f32 ly = localVerts[si].y * w.scale.y;
                        O.polyVerts[k] = FVec2{ w.position.x + lx * cph - ly * sph,
                                                 w.position.y + lx * sph + ly * cph };
                    }
                } else {
                    // フォールバック: 外接箱
                    O.shape = 2; O.polyCount = 4;
                    O.polyVerts[0] = FVec2{ w.position.x - hx*cph + hy*sph, w.position.y - hx*sph - hy*cph };
                    O.polyVerts[1] = FVec2{ w.position.x + hx*cph + hy*sph, w.position.y + hx*sph - hy*cph };
                    O.polyVerts[2] = FVec2{ w.position.x + hx*cph - hy*sph, w.position.y + hx*sph + hy*cph };
                    O.polyVerts[3] = FVec2{ w.position.x - hx*cph - hy*sph, w.position.y - hx*sph + hy*cph };
                }
            } else if (primShape == 0) {                  // 箱 → 4頂点ポリゴン (回転/細長も正確)
                O.shape = 2; O.polyCount = 4;
                O.polyVerts[0] = FVec2{ w.position.x - hx*cph + hy*sph, w.position.y - hx*sph - hy*cph };
                O.polyVerts[1] = FVec2{ w.position.x + hx*cph + hy*sph, w.position.y + hx*sph - hy*cph };
                O.polyVerts[2] = FVec2{ w.position.x + hx*cph - hy*sph, w.position.y + hx*sph + hy*cph };
                O.polyVerts[3] = FVec2{ w.position.x - hx*cph - hy*sph, w.position.y - hx*sph + hy*cph };
            } else {                                      // 円(1) / primitive 無し
                O.shape = (primShape == 1) ? 0 : oshape;
            }
        }
    }
    for (u32 i = 0; i < node.ChildCount(); ++i)
        if (ANode* ch = node.Child(i)) CollectLightsAndOccluders(*ch, lights, lc, occ, oc, has_lit);
}

/** camera view を設定し、ライトを収集してから root を DrawTree、OnDrawWorld を呼ぶ。 */
void AScene2D::DrawWorldPass(FRenderContext& rc) noexcept {
    FSpriteBatch& sb = rc.Sprites();   // 現パスに配線されたバッチ (通常 or 反射 RT 用)
    CCamera2D& cam = Services().Camera();
    const FVec2 center = cam.EffectiveViewCenter();
    sb.SetView(center.x, center.y, m_PixelsPerUnit * cam.Zoom());
    // lit スプライト (PBR マテリアル) 用に 2D ライト + 影オクルーダーを収集する。
    // lit ノードもライトも無いシーンでは SetLights を呼ばない (lit パイプラインを作らない)。
    FSpriteLight   lights[16];
    FSpriteOccluder occ[16];
    u32 lc = 0, oc = 0;
    bool has_lit = false;
    CollectLightsAndOccluders(*m_Root, lights, lc, occ, oc, has_lit);
    if (has_lit || lc > 0)
        sb.SetLights(lights, lc, FVec3{ 0.10f, 0.11f, 0.13f }, 90.0f, occ, oc);
    else
        sb.ClearLights();   // 前フレームのライト残留で既定 Lit が誤発動しないように
    m_Root->DrawTree(rc);
    OnDrawWorld(rc, sb);
}

/** 画面中心の view を設定し OnDrawHud を呼ぶ (カメラ非依存の HUD 描画)。 */
void AScene2D::DrawHudPass(FRenderContext& rc) noexcept {
    FSpriteBatch& sb = rc.Sprites();
    sb.SetView(static_cast<f32>(rc.Width()) * 0.5f,
               static_cast<f32>(rc.Height()) * 0.5f, 1.0f);
    OnDrawHud(rc, sb);
}

/** 反射用に world を焼くオフスクリーン RT を遅延作成・再作成する (成功で true)。 */
bool AScene2D::EnsureSceneRt(FRenderContext& rc) noexcept {
    const u32 w = rc.Width(), h = rc.Height();
    if (w == 0 || h == 0) return false;
    if (m_SceneRt && m_RtW == w && m_RtH == h) return true;
    IRhiDevice* dev = rc.GetRenderer().Device();
    if (dev == nullptr) return false;
    FTextureDesc td{};
    td.width = w; td.height = h;
    td.format = rc.GetRenderer().ColorFormat();
    td.is_render_target = true;
    auto r = CreateRhiTexture(*dev, td);
    if (r.IsErr()) { ACS_LOG_WARN("FScene2D: 反射用 scene RT の作成に失敗 (反射無効)"); return false; }
    m_SceneRt = Move(r.Value());
    m_RtW = w; m_RtH = h;
    return true;
}

/** TopDown 水メッシュの正規化水深を保持するカラー RT を遅延作成する (成功で true)。 */
bool AScene2D::EnsureWaterDepthRt(FRenderContext& rc) noexcept {
    const u32 w = rc.Width(), h = rc.Height();
    if (w == 0 || h == 0) return false;
    if (m_WaterDepthRt && m_WaterDepthW == w && m_WaterDepthH == h) return true;
    IRhiDevice* dev = rc.GetRenderer().Device();
    if (dev == nullptr) return false;
    FTextureDesc td{};
    td.width = w;
    td.height = h;
    // SpriteBatch と同じ PSO を使えるよう scene color と同じ RT format にする。
    // R に 0..1 の正規化岸距離を格納し、main water PS から SRV sample する。
    td.format = rc.GetRenderer().ColorFormat();
    td.is_render_target = true;
    auto r = CreateRhiTexture(*dev, td);
    if (r.IsErr()) {
        ACS_LOG_WARN("FScene2D: 水面深度 RT の作成に失敗 (頂点深度へ fallback)");
        return false;
    }
    m_WaterDepthRt = Move(r.Value());
    m_WaterDepthW = w;
    m_WaterDepthH = h;
    return true;
}

/** マスク用の stencil 付き深度バッファ (D24S8) を遅延作成・再作成する (成功で true)。 */
bool AScene2D::EnsureStencilBuffer(FRenderContext& rc) noexcept {
    const u32 w = rc.Width(), h = rc.Height();
    if (w == 0 || h == 0) return false;
    if (m_StencilBuf && m_StencilW == w && m_StencilH == h) return true;
    IRhiDevice* dev = rc.GetRenderer().Device();
    if (dev == nullptr) return false;
    FTextureDesc td{};
    td.width = w; td.height = h;
    td.format = EFormat::D24_UNorm_S8_UInt;   // 深度 8bit ステンシル付き
    td.is_depth_target = true;
    auto r = CreateRhiTexture(*dev, td);
    if (r.IsErr()) { ACS_LOG_WARN("FScene2D: マスク用 stencil バッファの作成に失敗 (マスク無効)"); return false; }
    m_StencilBuf = Move(r.Value());
    m_StencilW = w; m_StencilH = h;
    return true;
}

/** 反射/ステンシル設定に応じて単一〜複数パスでシーンを描画する。 */
void AScene2D::OnRender(FRenderContext& rc) noexcept {
    if (!EnsureSpriteBatch(rc)) return;
    m_ScreenW = rc.Width();        // picking 用に画面サイズをキャッシュ
    m_ScreenH = rc.Height();
    FSpriteBatch& sb = m_Sprites;
    CCamera2D& cam = Services().Camera();
    const FVec2 center = cam.EffectiveViewCenter();
    const f32 scale = m_PixelsPerUnit * cam.Zoom();
    rc._SetView2D(center, scale);   // 反射等の world→screen 投影用に配線

    // マスク用 stencil バッファ (有効かつ作成成功時のみ)。world パスで DSV として bind。
    IRhiTexture* stencil = (m_StencilMaskEnabled && EnsureStencilBuffer(rc)) ? m_StencilBuf.Get() : nullptr;

    FRenderer& renderer = rc.GetRenderer();
    IRhiCommandList& cl = rc.Cmd();
    IRhiSwapchain* sc = renderer.Swapchain();
    const FClearColor cc = GetGame().GetClearColor();

    const bool reflectionReady =
        m_ReflectionEnabled && EnsureSceneRt(rc) && EnsureSceneSprites(rc);
    const bool waterSceneCapture = reflectionReady && m_WaterSceneSamplingEnabled;
    const bool waterDepthReady =
        waterSceneCapture && EnsureWaterDepthRt(rc) && EnsureWaterDepthSprites(rc);

    if (reflectionReady) {
        // オフスクリーン scene-color pass。通常の planar reflection では従来どおり
        // world 全体を焼く。TopDown の実シーンサンプリング opt-in 時だけ専用フラグを
        // 立て、水自身を除外して「水下/岸/オブジェクト」の実カラーを得る。
        // main pass の描画先とは別 RT なので SRV/RTV の read/write hazard は生じない。
        FSpriteBatch& sbR = m_SceneSprites;
        cl.BeginRenderToTexture(*m_SceneRt, cc);
        sbR.Begin(cl, rc.Width(), rc.Height());
        rc._SetSpriteBatch(&sbR);
        rc._SetReflection(nullptr);
        rc._SetSceneColor(nullptr);
        rc._SetSceneDepth(nullptr);
        rc._SetSceneColorCapturePass(waterSceneCapture);
        rc._SetWaterDepthCapturePass(false);
        rc._SetStencilMaskActive(false);
        DrawWorldPass(rc);
        rc._SetSceneColorCapturePass(false);
        rc._SetSpriteBatch(nullptr);
        sbR.End();
        cl.EndRenderToTexture(*m_SceneRt);

        // 専用 water-depth pass。通常 Draw を抑止し、TopDown 水コンポーネントだけが
        // 実メッシュの岸距離深度を R へ描く。scene-color と別 RT のため、main water PS は
        // 両方を同時に安全に SRV sample できる。
        if (waterDepthReady) {
            FSpriteBatch& sbD = m_WaterDepthSprites;
            cl.BeginRenderToTexture(*m_WaterDepthRt, FClearColor{0, 0, 0, 0});
            sbD.Begin(cl, rc.Width(), rc.Height());
            sbD.SetDrawSuppressed(true);
            rc._SetSpriteBatch(&sbD);
            rc._SetWaterDepthCapturePass(true);
            rc._SetSceneColorCapturePass(false);
            rc._SetStencilMaskActive(false);
            DrawWorldPass(rc);
            rc._SetWaterDepthCapturePass(false);
            rc._SetSpriteBatch(nullptr);
            sbD.SetDrawSuppressed(false);
            sbD.End();
            cl.EndRenderToTexture(*m_WaterDepthRt);
        }

        // main pass: スワップチェーン (必要なら stencil 付き) を再バインド。
        // scene/depth RT は既に書き終えており、ここでは SRV としてのみ参照する。
        if (sc) cl.BeginRenderToSwapchain(*sc, renderer.CurrentBuffer(), cc, stencil);
        sb.Begin(cl, rc.Width(), rc.Height());
        rc._SetSpriteBatch(&sb);
        if (stencil) { rc._SetStencilMaskActive(true); sb.SetStencilMode(EStencilMode::Off); }
        rc._SetReflection(m_SceneRt.Get());   // 水がこの RT を鏡像 UV でサンプル
        rc._SetSceneColor(waterSceneCapture ? m_SceneRt.Get() : nullptr);
        rc._SetSceneDepth(waterDepthReady ? m_WaterDepthRt.Get() : nullptr);
        DrawWorldPass(rc);
        rc._SetReflection(nullptr);
        rc._SetSceneColor(nullptr);
        rc._SetSceneDepth(nullptr);
        if (stencil) { sb.SetStencilMode(EStencilMode::Off); rc._SetStencilMaskActive(false); }
        DrawHudPass(rc);
        rc._SetSpriteBatch(nullptr);
        sb.End();
        // EndRenderToSwapchain は CGame/Renderer の EndFrame が行う。
    } else if (stencil) {
        // stencil のみ: スワップチェーンを stencil 付きで再バインド (バリアはガード済)。
        if (sc) cl.BeginRenderToSwapchain(*sc, renderer.CurrentBuffer(), cc, stencil);
        sb.Begin(cl, rc.Width(), rc.Height());
        rc._SetSpriteBatch(&sb);
        rc._SetStencilMaskActive(true);
        sb.SetStencilMode(EStencilMode::Off);   // DSV 整合の既定 PSO へ切替
        DrawWorldPass(rc);
        sb.SetStencilMode(EStencilMode::Off);   // HUD が誤ってマスクされないよう解除
        rc._SetStencilMaskActive(false);
        DrawHudPass(rc);
        rc._SetSpriteBatch(nullptr);
        sb.End();
    } else {
        sb.Begin(rc.Cmd(), rc.Width(), rc.Height());
        rc._SetSpriteBatch(&sb);
        DrawWorldPass(rc);
        DrawHudPass(rc);
        rc._SetSpriteBatch(nullptr);
        sb.End();
    }
}

} // namespace acs::game
