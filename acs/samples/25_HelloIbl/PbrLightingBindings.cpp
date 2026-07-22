// SPDX-License-Identifier: Apache-2.0
// HelloIbl — FPbrShader の lighting / 補助テクスチャ bind 集約。
//
// HDR 描画パスで sphere grid / floor を描く前に呼ぶ。IBL / 太陽 / SSAO / SSGI /
// SSR / Shadow / Fog / probe grid / area light を一括で bind する。
//
// 1-frame latency パターン: SSAO / SSGI / SSR は color pass の「後」で焼かれる
// ので、ここで bind するのは前フレームに焼いたテクスチャ。m_XxxWarm フラグで
// frame 0 の garbage read を回避する (warm=false の間は null bind してフォール
// バック値を使わせる)。
#include "HelloIblApp.h"

using namespace acs;

namespace helloibl {

// 各 bind helper は FHelloIblApp の private (m_Pbr / m_Ibl / m_Shadow…) を直接触るため
// friend 宣言済み (FHelloIblApp.h)。匿名名前空間に入れると friend と一致しなくなるので
// namespace 直下に置く。
void BindIbl(FHelloIblApp& app) noexcept {
    app.m_Pbr.SetIbl(app.m_Ibl.IrradianceMap(), app.m_Ibl.PrefilterMap(), app.m_Ibl.BrdfLut(),
                    app.m_Ibl.PrefilterMips());
    app.m_Pbr.SetSh9(app.m_bUseSh9 ? app.m_Sh9 : nullptr);
}

void BindSun(FHelloIblApp& app, const FMat4& vp_for_render, const FDirLight& sun) noexcept {
    app.m_Pbr.SetLights(vp_for_render, app.m_Camera.Eye(), &sun, 1, FVec3{0, 0, 0});
    app.m_Pbr.SetPointLights(nullptr, 0);
}

void BindSsao(FHelloIblApp& app) noexcept {
    IRhiTexture* hdr = app.m_Post.HdrRenderTarget();
    if (!hdr) return;
    // 注: SSAO 無効時も viewport サイズは渡す。SSGI / SSR が screen UV を
    // ssao_params.zw から得るため、ここを 0 にすると参照が壊れる。
    if (app.m_bUseSsao && app.m_bSsaoWarm) {
        app.m_Pbr.SetSsao(app.m_Ssao.OutputTexture(), /*intensity=*/1.0f,
                         hdr->Width(), hdr->Height());
    } else {
        app.m_Pbr.SetSsao(nullptr, 0.0f, hdr->Width(), hdr->Height());
    }
}

void BindSsgi(FHelloIblApp& app) noexcept {
    if (app.m_bUseSsgi && app.m_bSsgiWarm) {
        app.m_Pbr.SetSsgi(app.m_Ssgi.OutputTexture(), /*intensity=*/0.6f);
    } else {
        app.m_Pbr.SetSsgi(nullptr, 0.0f);
    }
}

void BindSsr(FHelloIblApp& app) noexcept {
    // FPbrShader 側で roughness 依存合成 (rough 面ほど反射が弱まる)。
    if (app.m_ShowSsr && app.m_SsrWarm) {
        app.m_Pbr.SetSsr(app.m_Ssr.OutputTexture(), /*intensity=*/1.0f);
    } else {
        app.m_Pbr.SetSsr(nullptr, 0.0f);
    }
}

void BindShadow(FHelloIblApp& app) noexcept {
    if (app.m_bUseShadows) {
        FMat4 vps   [FShadowMap::kMaxCascades] = {};
        f32  splits[FShadowMap::kMaxCascades] = {};
        for (u32 c = 0; c < app.m_Shadow.CascadeCount(); ++c) {
            vps[c]    = app.m_Shadow.LightViewProjection(c);
            splits[c] = app.m_Shadow.CascadeSplit(c);
        }
        app.m_Pbr.SetShadowMapCascades(app.m_Shadow.DepthTexture(), vps, splits,
                                      app.m_Shadow.CascadeCount(),
                                      /*bias=*/0.002f,
                                      /*texel_size=*/1.0f / static_cast<f32>(app.m_Shadow.Size()));
    } else {
        app.m_Pbr.SetShadowMap(nullptr, FMat4{}, 0, 0);
    }
}

void BindFog(FHelloIblApp& app) noexcept {
    // 青灰色の解析積分 height fog。HG 前方散乱で太陽方向に自然な光だまりを作る。
    if (app.m_bUseFog) {
        app.m_Pbr.SetFog(FVec3{0.62f, 0.70f, 0.82f},
                         /*density=*/0.065f, /*height_falloff=*/0.16f,
                         /*height_base=*/0.0f, /*anisotropy=*/0.45f,
                         /*sun_scatter=*/0.20f);
    } else {
        app.m_Pbr.SetFog(FVec3{0, 0, 0}, 0.0f);
    }
}

void BindProbeGrid(FHelloIblApp& app) noexcept {
    if (app.m_bUseProbeGrid) {
        // 計算済 m_Sh9 (現 env の SH9) をベースに、左右の probe を赤/青に着色
        FPbrShader::FLightProbe p[2];
        p[0].position = FVec3{-4.0f, 1.5f, 3.0f};   // 左 probe (赤光)
        for (u32 k = 0; k < 9; ++k) p[0].sh9[k] = app.m_Sh9[k];
        p[0].sh9[0] = p[0].sh9[0] + FVec4{2.5f, 0.4f, 0.4f, 0};   // l=0 (DC) に赤を強める

        p[1].position = FVec3{+4.0f, 1.5f, 3.0f};   // 右 probe (青光)
        for (u32 k = 0; k < 9; ++k) p[1].sh9[k] = app.m_Sh9[k];
        p[1].sh9[0] = p[1].sh9[0] + FVec4{0.4f, 0.4f, 2.5f, 0};

        app.m_Pbr.SetProbeGrid(p, 2);
        app.m_bNeedSh9Rebuild = true;       // SH9 base が古ければ次フレームで再計算
    } else {
        app.m_Pbr.SetProbeGrid(nullptr, 0);
    }
}

void BindAreaLight(FHelloIblApp& app) noexcept {
    // 球グリッドの前方上空に置いた 2x1 矩形パネル
    if (app.m_bUseAreaLight) {
        FPbrShader::FAreaLight rect;
        rect.center = FVec3{0.0f, 4.0f, 1.0f};      // 上空、camera 側
        rect.axis_x = FVec3{1.0f, 0.0f, 0.0f};      // 横半幅 = 1
        rect.axis_y = FVec3{0.0f, 0.0f, 0.5f};      // 奥行半高 = 0.5
        rect.color  = FVec3{5.0f, 4.5f, 3.5f};      // 暖色 HDR
        app.m_Pbr.SetAreaLights(&rect, 1);
    } else {
        app.m_Pbr.SetAreaLights(nullptr, 0);
    }
}

void BindPbrLighting(FHelloIblApp& app, const FMat4& vp_for_render, const FDirLight& sun) noexcept {
    BindIbl(app);
    BindSun(app, vp_for_render, sun);
    BindSsao(app);
    BindSsgi(app);
    BindSsr(app);
    BindShadow(app);
    BindFog(app);
    BindProbeGrid(app);
    BindAreaLight(app);
}

} // namespace helloibl
