// SPDX-License-Identifier: Apache-2.0
// HelloIbl — FHelloIblApp の実装 (薄い orchestration レイヤ)。
//
// 各 pass は helper file (ShadowPass / GBufferPass / ScreenSpaceEffects /
// RefractionPass / DynamicOrbs / SceneDraw / PbrLightingBindings /
// IblPresetBuilder / TaaJitter / ExposureControl / HudOverlay) に分割。
// ここでは OnStart で資源を確保し、OnCustomFrame でその helper 群を順に呼ぶ
// だけのフレーム駆動ロジックを記述する。
//
// 1 フレーム = OnCustomFrame() の中で:
//   ・preset 切替 → env / irradiance / prefilter 再生成 (必要なら)
//   ・shadow pass (CSM 3 cascade)
//   ・HDR RT: skybox → IBL sphere grid → 動的オーブ → ガラス球 (refraction)
//   ・geometry G-buffer pass: motion vector + world normal を MRT に焼く
//   ・SSR / SSAO / SSGI を計算 (次フレーム用に 1-frame latency)
//   ・FPostProcess: Bloom + ACES tonemap + CAS sharpen + auto-expo + grading
//   ・FSpriteBatch HUD (LDR backbuffer に sub-window + テキスト)
#include "HelloIblApp.h"
#include "IblEnvBuilder.h"
#include "IblLightmapBaker.h"

#include "app/Sample.h"
#include "platform/Input.h"
#include "asset/MeshPrimitive.h"
#include "asset/MeshAsset.h"
#include "math/Mat.h"
#include "math/Math.h"
#include "foundation/Log.h"

using namespace acs;

namespace helloibl {

void FHelloIblApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }
    IRhiSwapchain* sc = GetRenderer().Swapchain();
    if (!sc) { Quit(); return; }

    const u32 sw = sc->Width();
    const u32 sh = sc->Height();

    // HDR FPostProcess (Bloom + ACES Tonemap) — HDR RT は R16G16B16A16_Float
    ACS_SAMPLE_INIT(m_Post.Init(*dev, sw, sh, GetRenderer().ColorFormat()));

    // シーン側は HDR RT format に揃える
    ACS_SAMPLE_INIT(m_Sky.Init(*dev, m_Post.HdrFormat(), GetRenderer().DepthFormat()));
    m_Sky.PresetDay();
    ACS_SAMPLE_INIT(m_Pbr.Init(*dev, m_Post.HdrFormat(), GetRenderer().DepthFormat()));

    auto sphere = Primitive::MakeSphere(0.55f, 48, 24);
    ACS_SAMPLE_INIT(UploadMesh(*dev, *sphere, m_GmSphere));
    auto plane  = Primitive::MakePlane(40.0f, 40.0f);
    ACS_SAMPLE_INIT(UploadMesh(*dev, *plane, m_GmPlane));

    // FShadowMap: 2048 px × 3 cascade atlas (CSM)
    ACS_SAMPLE_INIT(m_Shadow.Init(*dev, 2048, /*cascade_count=*/3));
    // SSR: HDR と同フォーマット / 同サイズで scratch を確保
    ACS_SAMPLE_INIT(m_Ssr.Init(*dev, m_Post.HdrFormat(), sw, sh));
    // SSAO: depth から visibility を計算、frame size と同じ解像度
    ACS_SAMPLE_INIT(m_Ssao.Init(*dev, sw, sh));
    // SSGI: scene_color + depth → 1 bounce indirect light
    ACS_SAMPLE_INIT(m_Ssgi.Init(*dev, sw, sh));
    // Motion vector: 動的 mesh の screen-space motion を焼く
    ACS_SAMPLE_INIT(m_Motion.Init(*dev, sw, sh));

    // Refraction: screen-space 屈折 + 背景キャプチャ用 RT。
    // m_BgRt は opaque pass の HDR をコピーする先 (同一 RT の read+write 不可 回避)。
    // HDR と同じフォーマット・解像度で確保。
    ACS_SAMPLE_INIT(m_Refr.Init(*dev, m_Post.HdrFormat(), GetRenderer().DepthFormat()));
    ACS_SAMPLE_INIT(m_Blit.Init(*dev, m_Post.HdrFormat()));
    {
        FTextureDesc bg_td{};
        bg_td.width            = sw;
        bg_td.height           = sh;
        bg_td.format           = m_Post.HdrFormat();
        bg_td.is_render_target = true;
        auto bg_r = CreateRhiTexture(*dev, bg_td);
        if (bg_r.IsErr()) {
            ACS_LOG_ERROR("HelloIbl: refraction background RT 作成に失敗: %s",
                          bg_r.Error().message);
            Quit();
            return;
        }
        m_BgRt = Move(bg_r.Value());
    }

    // 床用 lightmap を CPU 焼き。FSphere-FRay analytical hit で覆い、各 plane texel
    // で hemisphere sampling して visibility を求める。実装は IblLightmapBaker.cpp。
    {
        auto lm_r = BakeFloorLightmap(*dev);
        if (lm_r.IsErr()) {
            ACS_LOG_ERROR("HelloIbl: floor lightmap bake failed");
            Quit();
            return;
        }
        m_Lightmap = Move(lm_r.Value());
    }

    // FSpriteBatch は LDR backbuffer (tonemap 後)
    ACS_SAMPLE_INIT(m_Batch.Init(*dev, GetRenderer().ColorFormat()));
    (void)FSample::TryLoadDefaultUIFont(m_Font, *dev, 18.0f, 1024, true);

    const f32 aspect = static_cast<f32>(sw) / static_cast<f32>(sh);
    m_Camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 100.0f);
    m_CamPos = FVec3{0, 1.0f, -5.0f};

    // Bloom 強度はデフォルトより少し弱めに (Day sky は十分明るいので)
    m_PostParams.bloom_threshold = 1.5f;
    m_PostParams.bloom_intensity = 0.4f;
    // 手動モード用の初期露出。preset Day = 0.7、adapted も同値に。
    m_ExposureTarget   = 0.7f;
    m_AdaptedExposure  = 0.7f;
    // 既定で GPU auto-exposure を有効化。シーン輝度を GPU で実測して露出を
    // 自動算出 → preset 切替 (Day↔Night) でも自動で再露出され、eye adaptation
    // が演出として効く。'U' で手動モードへ切替可。
    m_bUseAutoExposure = true;
    m_PostParams.auto_exposure_enabled = true;
    m_PostParams.auto_exposure_key     = m_AutoKey;
    m_PostParams.auto_exposure_speed   = 1.5f;   // τ≈0.7s のシネマ的順応速度
    m_PostParams.exposure              = 1.0f;   // 露出は GPU 側が決める

    // FColor grading cinematic look: 軽い暖色 + 彩度ブースト + コントラスト
    m_PostParams.cg_saturation   = 1.10f;
    m_PostParams.cg_contrast     = 1.08f;
    m_PostParams.cg_temperature  = 0.08f;     // 暖色寄り
    m_PostParams.cg_tint         = -0.02f;    // 軽く緑寄り (映画 teal-orange)
    m_PostParams.cg_lift         = FVec3{0.005f, 0.0f, 0.01f};      // 影に微かな青冷感
    m_PostParams.cg_gain         = FVec3{1.04f, 1.02f, 0.98f};      // ハイライトを暖色に

    // CAS sharpening: subtle clarity boost、UE5 デフォルト相当 0.4
    m_PostParams.cas_strength    = 0.4f;
    // SSR は FPbrShader 側で合成するので tonemap 側の SSR は無効化。intensity 0 に
    // しないと fallback bloom mip が誤加算される。
    m_PostParams.ssr_intensity   = 0.0f;

    // 動的球の初期 transform。frame 0 で prev==curr になるよう curr と同値で
    // prev も初期化する。
    for (u32 i = 0; i < kDynCount; ++i) {
        m_DynCurr[i] = ComputeDynTransform(i, 0.0f);
        m_DynPrev[i] = m_DynCurr[i];
    }
}

FMat4 FHelloIblApp::ComputeDynTransform(u32 i, f32 t) const noexcept {
    const f32 a = t * 1.8f + static_cast<f32>(i) * (2.0f * kPi / 3.0f);
    const f32 r = 2.2f;
    return FMat4::Translation(FVec3{ Cos(a) * r, 3.0f + Sin(a) * r, 1.0f });
}

void FHelloIblApp::OnUpdate(f32 dt) noexcept {
    if (FInput::IsKeyPressed(EKey::Escape)) Quit();

    // 1/2/3/4/5 で env preset 切替。SH9 mode が有効中は SH 9 係数も再計算が必要。
    // preset ごとに露出目標を変える → m_AdaptedExposure がじわっと追従して
    // eye adaptation (目が明暗に慣れる) 演出になる。
    if (FInput::IsKeyPressed(EKey::Num1)) {
        m_Sky.PresetDay();    m_CurrentPreset = 0;
        m_bNeedRecapture = true; m_bNeedSh9Rebuild = m_bUseSh9;
        m_ExposureTarget = 0.7f;     // Day: 明るいので露出を絞る
    }
    if (FInput::IsKeyPressed(EKey::Num2)) {
        m_Sky.PresetSunset(); m_CurrentPreset = 1;
        m_bNeedRecapture = true; m_bNeedSh9Rebuild = m_bUseSh9;
        m_ExposureTarget = 1.0f;
    }
    if (FInput::IsKeyPressed(EKey::Num3)) {
        m_Sky.PresetNight();  m_CurrentPreset = 2;
        m_bNeedRecapture = true; m_bNeedSh9Rebuild = m_bUseSh9;
        m_ExposureTarget = 1.8f;     // Night: 暗いので露出を上げる
    }
    if (FInput::IsKeyPressed(EKey::Num4)) {
        m_CurrentPreset = 3;
        m_bNeedStudioHdr = true; m_bNeedSh9Rebuild = m_bUseSh9;
        m_ExposureTarget = 1.0f;
    }
    if (FInput::IsKeyPressed(EKey::Num5)) {
        m_CurrentPreset = 4;
        m_bNeedAtmosphere = true; m_bNeedSh9Rebuild = m_bUseSh9;
        m_ExposureTarget = 0.85f;
    }

    if (FInput::IsKeyPressed(EKey::I)) m_DisplayMode = (m_DisplayMode + 1) % 7;
    // SH9 toggle: 現在の irradiance cubemap から計算した SH 9 で diffuse を再構築
    if (FInput::IsKeyPressed(EKey::S)) {
        m_bUseSh9 = !m_bUseSh9;
        m_bNeedSh9Rebuild = m_bUseSh9;     // 必要なときに再計算
    }
    if (FInput::IsKeyPressed(EKey::C)) m_bUseClearcoat = !m_bUseClearcoat;
    if (FInput::IsKeyPressed(EKey::Z)) m_bUseAnisotropy = !m_bUseAnisotropy;
    if (FInput::IsKeyPressed(EKey::L)) m_bUseAreaLight = !m_bUseAreaLight;
    if (FInput::IsKeyPressed(EKey::G)) m_bUseProbeGrid = !m_bUseProbeGrid;
    if (FInput::IsKeyPressed(EKey::F)) m_bUseFog = !m_bUseFog;
    if (FInput::IsKeyPressed(EKey::H)) m_bUseShadows = !m_bUseShadows;
    if (FInput::IsKeyPressed(EKey::R)) m_ShowSsr = !m_ShowSsr;
    if (FInput::IsKeyPressed(EKey::X)) m_ShowRefraction = !m_ShowRefraction;
    if (FInput::IsKeyPressed(EKey::O)) m_bUseSsao = !m_bUseSsao;
    if (FInput::IsKeyPressed(EKey::T)) m_bUseTaa  = !m_bUseTaa;
    if (FInput::IsKeyPressed(EKey::J)) m_bUseSsgi = !m_bUseSsgi;
    if (FInput::IsKeyPressed(EKey::K)) m_bUseLightmap = !m_bUseLightmap;
    if (FInput::IsKeyPressed(EKey::M)) m_bUseMotionVec = !m_bUseMotionVec;
    if (FInput::IsKeyPressed(EKey::B)) m_PostParams.bloom_enabled = !m_PostParams.bloom_enabled;

    // film grain アニメ用に時間累積 + 動的球の公転時刻
    m_PostParams.grain_time += dt;
    m_AnimTime += dt;

    UpdateExposureControls(*this, dt);

    // 視点を矢印 (回転) + WASD (移動) で操作
    const f32 mv = 4.0f * dt, tr = 1.5f * dt;
    if (FInput::IsKeyDown(EKey::Left))  m_CamYaw -= tr;
    if (FInput::IsKeyDown(EKey::Right)) m_CamYaw += tr;
    if (FInput::IsKeyDown(EKey::Up))    m_CamPitch -= tr * 0.8f;
    if (FInput::IsKeyDown(EKey::Down))  m_CamPitch += tr * 0.8f;
    const f32 limit = 0.45f * kPi;
    if (m_CamPitch >  limit) m_CamPitch =  limit;
    if (m_CamPitch < -limit) m_CamPitch = -limit;

    FVec3 forward{ Sin(m_CamYaw) * Cos(m_CamPitch),
                 -Sin(m_CamPitch),
                  Cos(m_CamYaw) * Cos(m_CamPitch) };
    FVec3 right{ Cos(m_CamYaw), 0, -Sin(m_CamYaw) };
    if (FInput::IsKeyDown(EKey::W)) m_CamPos += forward * mv;
    if (FInput::IsKeyDown(EKey::S)) m_CamPos -= forward * mv;
    if (FInput::IsKeyDown(EKey::D)) m_CamPos += right   * mv;
    if (FInput::IsKeyDown(EKey::A)) m_CamPos -= right   * mv;
    m_Camera.SetLookAt(m_CamPos, m_CamPos + forward);
}

// OnCustomFrame() で HDR 経路に切替えてデフォルトフローを置き換える。
// 1) IBL build (必要なら、RT を一時的に切替)
// 2) HDR RT にシーン (skybox + sphere grid) を描画
// 3) FPostProcess.Render で Bloom + Tonemap → LDR backbuffer
// 4) FSpriteBatch HUD を LDR backbuffer に
bool FHelloIblApp::OnCustomFrame() noexcept {
    IRhiDevice*      dev   = GetRenderer().Device();
    IRhiCommandList* cl    = GetRenderer().CommandList();
    IRhiSwapchain*   sc    = GetRenderer().Swapchain();
    IRhiTexture*     hdr   = m_Post.HdrRenderTarget();
    IRhiTexture*     depth = GetRenderer().DepthBuffer();
    if (!dev || !cl || !sc || !hdr) return false;

    const u64 required_object_draws =
        1u +
        static_cast<u64>(kPbrGridSize) *
            static_cast<u64>(kPbrGridSize) +
        static_cast<u64>(kDynCount);
    const u32 object_draw_hint =
        required_object_draws > static_cast<u64>(~u32{0})
            ? ~u32{0}
            : static_cast<u32>(required_object_draws);
    if (!m_Pbr.BeginFrame(object_draw_hint)) return false;

    UpdateDynamicOrbs(*this);

    // TAA Halton(2,3) sub-pixel jitter を skybox / FPbrShader / SSR / SSAO の
    // VP に適用する。複数フレームの累積でエッジが滑らかになる。
    const FMat4 vp_no_jitter = m_Camera.ViewProjection();
    const FMat4 vp_for_render = BuildJitteredViewProjection(*this, vp_no_jitter,
                                                           hdr->Width(), hdr->Height());

    const u32 buf_idx = sc->AcquireNextImage();
    cl->Begin();

    ApplyPresetRebuilds(*this);

    // ===== Shadow pass (CSM、'H' で有効) =====
    const FVec3 sun_dir = ResolveSunDirection(*this);
    RenderShadowPass(*this, sun_dir);

    // ===== 1) HDR RT にシーン描画 =====
    cl->BeginRenderToTexture(*hdr, FClearColor{0, 0, 0, 1}, depth, 1.0f);

    FViewport vp{}; vp.width  = static_cast<f32>(hdr->Width());
                   vp.height = static_cast<f32>(hdr->Height());
    cl->SetViewport(vp);
    FScissorRect svr{}; svr.right  = static_cast<i32>(hdr->Width());
                       svr.bottom = static_cast<i32>(hdr->Height());
    cl->SetScissor(svr);

    // 背景 skybox。表示モードに応じて env / irradiance / prefilter を切替える。
    IRhiTexture* display_cube = nullptr;
    f32          mip_level    = 0.0f;
    if (m_DisplayMode == 0) {
        display_cube = m_Ibl.EnvCubemap();
    } else if (m_DisplayMode == 1) {
        display_cube = m_Ibl.IrradianceMap();
    } else {
        display_cube = m_Ibl.PrefilterMap();
        mip_level    = static_cast<f32>(m_DisplayMode - 2);
    }
    if (display_cube) {
        m_Ibl.DrawSkybox(*dev, *cl, *display_cube,
                        vp_for_render, m_Camera.Eye(),
                        m_Post.HdrFormat(), GetRenderer().DepthFormat(),
                        mip_level);
    }

    // SH9 mode: 現在の env cubemap (sky or studio HDR) から SH 9 を計算
    if (m_bNeedSh9Rebuild) {
        // Studio HDR は別 builder で既に焼かれている。それ以外は FSky 評価から焼く。
        if (m_CurrentPreset != 3) {
            BuildEquirectFromSky(m_Sky, m_EquirectRgba);
        }
        FImageBasedLighting::ComputeSh9FromEquirect(
            m_EquirectRgba.Data(), kEquirectWidth, kEquirectHeight, m_Sh9);
        m_bNeedSh9Rebuild = false;
    }

    // 太陽の direct light を 1 灯追加 (Studio HDR では中央パネルを sun に見立てる)。
    // これで clear-coat / anisotropic の direct specular が見える。
    FDirLight sun;
    if (m_CurrentPreset == 3) {
        sun.direction = FVec3{0, 0.4f, 1.0f};
        sun.color     = FVec3{0.7f, 0.7f, 0.7f};
    } else {
        sun.direction = m_Sky.SunDirection();
        sun.color     = m_Sky.SunColor() * 0.9f;
    }

    BindPbrLighting(*this, vp_for_render, sun);
    DrawFloor(*this);
    DrawSphereGrid(*this);
    DrawDynamicOrbs(*this);

    cl->EndRenderToTexture(*hdr);

    // ===== Refraction pass (ガラス球) =====
    RenderRefractionPass(*this, vp_for_render, vp, svr);

    // ===== Geometry G-buffer pass (motion + normal) =====
    RenderMotionAndNormalGBuffer(*this, vp_no_jitter);

    // motion texture を TAA / SSGI へ渡す。frame 0 は prev VP 未確定なので渡さず、
    // depth reprojection 側の cold-start ガードに委ねる。
    IRhiTexture* motion_tex =
        (m_MotionGBufferValid && m_bUseMotionVec && m_TaaPrevVpValid)
            ? m_Motion.OutputTexture()
            : nullptr;
    m_PostParams.taa_motion_texture = motion_tex;

    // ===== Screen-space effects (1-frame latency) =====
    const FMat4 inv_vp = Inverse(vp_for_render);
    RenderSsrPass(*this, vp_for_render, inv_vp, vp_no_jitter);
    RenderSsaoPass(*this, vp_for_render, inv_vp, sun.direction);
    RenderSsgiPass(*this, vp_for_render, inv_vp, vp_no_jitter);

    // TAA を毎フレーム params に反映。
    // 注意 (frame 0 garbage 回避): まだ m_PrevVpNoJitter が default (identity) のときに
    // reproject すると world 座標を clip 座標と誤解して prev_ndc が破綻する。
    // m_TaaPrevVpValid フラグで「前フレーム VP を本物で書いた」状態を保証してから
    // depth_texture を渡す。最初の 1 フレームは depth=null で reproject 無効化。
    m_PostParams.taa_enabled                  = m_bUseTaa;
    m_PostParams.taa_blend_factor             = 0.1f;     // current 10% + history 90%
    m_PostParams.taa_depth_texture            = (m_bUseTaa && m_TaaPrevVpValid) ? depth : nullptr;
    m_PostParams.taa_view_proj_no_jitter      = vp_no_jitter;
    m_PostParams.taa_prev_view_proj_no_jitter = m_PrevVpNoJitter;

    // ===== 2) Bloom + ACES Tonemap → LDR backbuffer (SSR も additive mix) =====
    m_Post.Render(*cl, *sc, buf_idx, m_PostParams);

    // 次フレーム用に保存 (jitter なしの true VP)
    m_PrevVpNoJitter = vp_no_jitter;
    m_TaaPrevVpValid = true;

    // ===== 3) FSpriteBatch HUD (LDR backbuffer) =====
    DrawHud(*this, sc->Width(), sc->Height());

    cl->EndRenderToSwapchain(*sc, buf_idx);
    cl->End();
    cl->Submit();
    sc->Present();
    return true;
}

void FHelloIblApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    m_Font.Shutdown();
    m_Batch.Shutdown();
    m_BgRt.Reset();
    m_Blit.Shutdown();
    m_Refr.Shutdown();
    m_Motion.Shutdown();
    m_Ssgi.Shutdown();
    m_Ssao.Shutdown();
    m_Ssr.Shutdown();
    m_Shadow.Shutdown();
    m_GmPlane = FGpuMesh{};
    m_GmSphere = FGpuMesh{};
    m_Pbr.Shutdown();
    m_Ibl.Shutdown();
    m_Sky.Shutdown();
    m_Post.Shutdown();
}

} // namespace helloibl
