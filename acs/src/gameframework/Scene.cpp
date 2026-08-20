// SPDX-License-Identifier: Apache-2.0
#include "gameframework/Scene.h"

#include "gameframework/RenderContext.h"
#include "gameframework/SceneServices.h"
#include "gameframework/Camera2D.h"
#include "gameframework/Draw.h"
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
void AScene::OnWorldSubsystemsReady_Internal() noexcept
{
    Root().ManagementAccess().SetSubsystems(WorldSubsystemsPtr_Internal());
    ASpawn2DSubsystem* const Spawner = GetSubsystem<ASpawn2DSubsystem>();
    if (Spawner != nullptr) Spawner->BindTargetRoot(&Root());
}

/** SwapContents 直後に graph から呼ばれ、owner の AScene へ再配線を依頼する。 */
void AScene::OnGraphRootSwapped_Internal(void* user) noexcept {
    static_cast<AScene*>(user)->RewireGraphRoot_Internal();
}

/** 差し替え後の root へ service/subsystem/spawn 先の配線をやり直す。 */
void AScene::RewireGraphRoot_Internal() noexcept {
    if (!m_Graph.HasRoot()) return;
    ANode& NewRoot = m_Graph.Root();
    // OnWorldSubsystemsReady_Internal / Enter_Internal と同じ配線を、swap 後の root へ適用し直す。
    // 未 attach (pre-enter の load) では services が無いだけで、束の公開は常に安全。
    NewRoot.ManagementAccess().SetSubsystems(WorldSubsystemsPtr_Internal());
    ASpawn2DSubsystem* const Spawner = GetSubsystem<ASpawn2DSubsystem>();
    if (Spawner != nullptr) Spawner->BindTargetRoot(&NewRoot);
    if (HasServices()) {
        NewRoot.ManagementAccess().SetSceneServices(ServicesOrNull_Internal());
        NewRoot.ManagementAccess().ActivateServices(Services());
    }
}

/** rootへservicesを配線し、利用者hook後に既存componentへ通知する。 */
void AScene::Enter_Internal() noexcept {
    // 利用者hookより前に配線し、hook内で追加したcomponentは即時通知する。
    // hook後のActivateServices_Internalは配線前から存在したcomponentを補う。
    if (HasServices()) Root().ManagementAccess().SetSceneServices(ServicesOrNull_Internal());
    OnEnter();
    if (HasServices()) Root().ManagementAccess().ActivateServices(Services());
}

/** 既定の入場hookとしてOnReadyを呼ぶ。 */
void AScene::OnEnter() noexcept {
    OnReady();
}

/** 利用者hook後に構造変更と要求済みserviceを後始末する。 */
void AScene::Exit_Internal() noexcept {
    OnExit();
    // 破棄予定ノードを pool から外してから reap する (順序は graph の Update と同じ)。
    m_Graph.ResolveStructuralChanges();
    // kScene2DServices を要求するシーンだけが Physics2D/Tweens を持つ。既定は ESvc::None なので
    // service ごとに要求済みかを確認する (docs/SceneUnification.md)。
    if (HasServices()) {
        if (Services().Has(ESvc::Physics2D)) Services().Physics().ClearAll();
        if (Services().Has(ESvc::Tweens))    Services().Tweens().CancelAll();
    }
    // CSpriteBatch と RT は CGame が game 寿命で共有するため、シーン退場では解放しない
    // (docs/SceneUnification.md)。シーンを跨いで再 Init しない分、遷移コストも下がる。
}

/** 既定の退場hook。共通後始末はExit_Internalが必ず実行する。 */
void AScene::OnExit() noexcept {}

/** CGame が共有する world/HUD 用 CSpriteBatch を返す (シーンは所有しない)。 */
CSpriteBatch& AScene::SpriteBatch() noexcept {
    return GetGame().SceneRenderResources().SpriteBatch();
}

/** 利用者hook後にrootのUpdateTreeと構造変更解決を実行する。 */
void AScene::Update_Internal(f32 DeltaSeconds) noexcept {
    OnUpdate(DeltaSeconds);
    // UpdateTree → pool purge → 構造変更解決。reap される前に破棄予定ノードを
    // pool から外す (docs/SceneUnification.md)。
    m_Graph.Update(DeltaSeconds);
}

/** 既定の毎frame hookとしてOnTickを呼ぶ。 */
void AScene::OnUpdate(f32 DeltaSeconds) noexcept {
    OnTick(DeltaSeconds);
}

/** 利用者hook後にrootの固定更新と構造変更解決を実行する。 */
void AScene::FixedUpdate_Internal(f32 FixedDeltaSeconds) noexcept {
    OnFixedUpdate(FixedDeltaSeconds);
    m_Graph.FixedUpdate(FixedDeltaSeconds);
}

/** 既定の固定更新hookとしてOnFixedTickを呼ぶ。 */
void AScene::OnFixedUpdate(f32 FixedDeltaSeconds) noexcept {
    OnFixedTick(FixedDeltaSeconds);
}

/** camera service が無いシーンでも使える view 中心を返す (無ければ原点)。 */
FVec2 AScene::ViewCenter() noexcept {
    if (!HasServices() || !Services().Has(ESvc::Camera2D)) return FVec2{0.0f, 0.0f};
    return Services().Camera().EffectiveViewCenter();
}

/** camera service が無いシーンでも使える zoom を返す (無ければ等倍)。 */
f32 AScene::ViewZoom() noexcept {
    if (!HasServices() || !Services().Has(ESvc::Camera2D)) return 1.0f;
    return Services().Camera().Zoom();
}

/** 画面ピクセル座標をワールド座標へ変換する (camera 中心・zoom・ppu を逆適用)。 */
FVec2 AScene::ScreenToWorld(FVec2 screen_px) noexcept {
    const FVec2 vc   = ViewCenter();
    const f32   zoom = ViewZoom();
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
void AScene::DrawWorldPass(FRenderContext& rc) noexcept {
    CSpriteBatch& sb = rc.Sprites();   // 現パスに配線されたバッチ (通常 or 反射 RT 用)
    // Camera2D を要求しないシーン (AScene の既定 ESvc::None) でも描けるよう、
    // ScreenToWorld と同じフォールバック (原点中心・等倍) を使う。
    const FVec2 center = ViewCenter();
    sb.SetView(center.x, center.y, m_PixelsPerUnit * ViewZoom());
    // lit スプライト (PBR マテリアル) 用に 2D ライト + 影オクルーダーを収集する。
    // lit ノードもライトも無いシーンでは SetLights を呼ばない (lit パイプラインを作らない)。
    FSpriteLight   lights[16];
    FSpriteOccluder occ[16];
    u32 lc = 0, oc = 0;
    bool has_lit = false;
    CollectLightsAndOccluders(Root(), lights, lc, occ, oc, has_lit);
    if (has_lit || lc > 0)
        sb.SetLights(lights, lc, FVec3{ 0.10f, 0.11f, 0.13f }, 90.0f, occ, oc);
    else
        sb.ClearLights();   // 前フレームのライト残留で既定 Lit が誤発動しないように
    Root().DrawTree(rc);
    OnDrawWorld(rc, sb);
    OnDrawWorld();
}

/** 画面中心の view を設定し OnDrawHud を呼ぶ (カメラ非依存の HUD 描画)。 */
void AScene::DrawHudPass(FRenderContext& rc) noexcept {
    CSpriteBatch& sb = rc.Sprites();
    sb.SetView(static_cast<f32>(rc.Width()) * 0.5f,
               static_cast<f32>(rc.Height()) * 0.5f, 1.0f);
    OnDrawHud(rc, sb);
    OnDrawHud();
}

/** 反射/ステンシル設定に応じて単一〜複数パスでシーンを描画する。 */
void AScene::OnRender(FRenderContext& rc) noexcept {
    // 描画リソースは CGame が game 寿命で共有する (docs/SceneUnification.md)。
    CSceneRenderResources& res = GetGame().SceneRenderResources();
    if (!res.EnsureSpriteBatch(rc)) return;
    // 以降このパスの間だけ「今の描画先」が publish され、batch を持ち回らずに
    // Draw* 関数から描けるようになる (gameframework/Draw.h)。
    SetDrawContext_Internal(&rc);
    m_ScreenW = rc.Width();        // picking 用に画面サイズをキャッシュ
    m_ScreenH = rc.Height();
    CSpriteBatch& sb = res.SpriteBatch();
    const FVec2 center = ViewCenter();
    const f32 scale = m_PixelsPerUnit * ViewZoom();
    rc.SetView2D_Internal(center, scale);   // 反射等の world→screen 投影用に配線

    // マスク用 stencil バッファ (有効かつ作成成功時のみ)。world パスで DSV として bind。
    IRhiTexture* stencil = (m_StencilMaskEnabled && res.EnsureStencilBuffer(rc)) ? res.StencilBuffer() : nullptr;

    CRenderer& renderer = rc.GetRenderer();
    IRhiCommandList& cl = rc.Cmd();
    IRhiSwapchain* sc = renderer.Swapchain();
    const FClearColor cc = GetGame().GetClearColor();

    const bool reflectionReady =
        m_ReflectionEnabled && res.EnsureSceneRt(rc) && res.EnsureSceneSprites(rc);
    const bool waterSceneCapture = reflectionReady && m_WaterSceneSamplingEnabled;
    const bool waterDepthReady =
        waterSceneCapture && res.EnsureWaterDepthRt(rc) && res.EnsureWaterDepthSprites(rc);

    if (reflectionReady) {
        // オフスクリーン scene-color pass。通常の planar reflection では従来どおり
        // world 全体を焼く。TopDown の実シーンサンプリング opt-in 時だけ専用フラグを
        // 立て、水自身を除外して「水下/岸/オブジェクト」の実カラーを得る。
        // main pass の描画先とは別 RT なので SRV/RTV の read/write hazard は生じない。
        CSpriteBatch& sbR = res.SceneSprites();
        cl.BeginRenderToTexture(*res.SceneRt(), cc);
        sbR.Begin(cl, rc.Width(), rc.Height());
        rc.SetSpriteBatch_Internal(&sbR);
        rc.SetReflection_Internal(nullptr);
        rc.SetSceneColor_Internal(nullptr);
        rc.SetSceneDepth_Internal(nullptr);
        rc.SetSceneColorCapturePass_Internal(waterSceneCapture);
        rc.SetWaterDepthCapturePass_Internal(false);
        rc.SetStencilMaskActive_Internal(false);
        DrawWorldPass(rc);
        rc.SetSceneColorCapturePass_Internal(false);
        rc.SetSpriteBatch_Internal(nullptr);
        sbR.End();
        cl.EndRenderToTexture(*res.SceneRt());

        // 専用 water-depth pass。通常 Draw を抑止し、TopDown 水コンポーネントだけが
        // 実メッシュの岸距離深度を R へ描く。scene-color と別 RT のため、main water PS は
        // 両方を同時に安全に SRV sample できる。
        if (waterDepthReady) {
            CSpriteBatch& sbD = res.WaterDepthSprites();
            cl.BeginRenderToTexture(*res.WaterDepthRt(), FClearColor{0, 0, 0, 0});
            sbD.Begin(cl, rc.Width(), rc.Height());
            sbD.SetDrawSuppressed(true);
            rc.SetSpriteBatch_Internal(&sbD);
            rc.SetWaterDepthCapturePass_Internal(true);
            rc.SetSceneColorCapturePass_Internal(false);
            rc.SetStencilMaskActive_Internal(false);
            DrawWorldPass(rc);
            rc.SetWaterDepthCapturePass_Internal(false);
            rc.SetSpriteBatch_Internal(nullptr);
            sbD.SetDrawSuppressed(false);
            sbD.End();
            cl.EndRenderToTexture(*res.WaterDepthRt());
        }

        // main pass: スワップチェーン (必要なら stencil 付き) を再バインド。
        // scene/depth RT は既に書き終えており、ここでは SRV としてのみ参照する。
        if (sc) cl.BeginRenderToSwapchain(*sc, renderer.CurrentBuffer(), cc, stencil);
        sb.Begin(cl, rc.Width(), rc.Height());
        rc.SetSpriteBatch_Internal(&sb);
        if (stencil) { rc.SetStencilMaskActive_Internal(true); sb.SetStencilMode(EStencilMode::Off); }
        rc.SetReflection_Internal(res.SceneRt());   // 水がこの RT を鏡像 UV でサンプル
        rc.SetSceneColor_Internal(waterSceneCapture ? res.SceneRt() : nullptr);
        rc.SetSceneDepth_Internal(waterDepthReady ? res.WaterDepthRt() : nullptr);
        DrawWorldPass(rc);
        rc.SetReflection_Internal(nullptr);
        rc.SetSceneColor_Internal(nullptr);
        rc.SetSceneDepth_Internal(nullptr);
        if (stencil) { sb.SetStencilMode(EStencilMode::Off); rc.SetStencilMaskActive_Internal(false); }
        DrawHudPass(rc);
        rc.SetSpriteBatch_Internal(nullptr);
        sb.End();
        // EndRenderToSwapchain は CGame/Renderer の EndFrame が行う。
    } else if (stencil) {
        // stencil のみ: スワップチェーンを stencil 付きで再バインド (バリアはガード済)。
        if (sc) cl.BeginRenderToSwapchain(*sc, renderer.CurrentBuffer(), cc, stencil);
        sb.Begin(cl, rc.Width(), rc.Height());
        rc.SetSpriteBatch_Internal(&sb);
        rc.SetStencilMaskActive_Internal(true);
        sb.SetStencilMode(EStencilMode::Off);   // DSV 整合の既定 PSO へ切替
        DrawWorldPass(rc);
        sb.SetStencilMode(EStencilMode::Off);   // HUD が誤ってマスクされないよう解除
        rc.SetStencilMaskActive_Internal(false);
        DrawHudPass(rc);
        rc.SetSpriteBatch_Internal(nullptr);
        sb.End();
    } else {
        sb.Begin(rc.Cmd(), rc.Width(), rc.Height());
        rc.SetSpriteBatch_Internal(&sb);
        DrawWorldPass(rc);
        DrawHudPass(rc);
        rc.SetSpriteBatch_Internal(nullptr);
        sb.End();
    }
    // パスを出たら Draw* が何も描かないよう publish を解除する。
    SetDrawContext_Internal(nullptr);
}

} // namespace acs::game
