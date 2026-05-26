// SPDX-License-Identifier: Apache-2.0
// HelloIbl — FPbrShader の lighting / 補助テクスチャ bind 集約。
//
// HDR 描画パスで sphere grid / floor を描く前に呼ぶ。IBL / 太陽 / SSAO / SSGI /
// SSR / Shadow / Fog / probe grid / area light を一括で bind する。
//
// 1-frame latency パターン: SSAO / SSGI / SSR は color pass の「後」で焼かれる
// ので、ここで bind するのは前フレームに焼いたテクスチャ。_xxx_warm フラグで
// frame 0 の garbage read を回避する (warm=false の間は null bind してフォール
// バック値を使わせる)。
#include "HelloIblApp.h"

using namespace acs;

namespace helloibl {

namespace {

void BindIbl(HelloIblApp& app) noexcept {
    app._pbr.SetIbl(app._ibl.IrradianceMap(), app._ibl.PrefilterMap(), app._ibl.BrdfLut(),
                    app._ibl.PrefilterMips());
    app._pbr.SetSh9(app._use_sh9 ? app._sh9 : nullptr);
}

void BindSun(HelloIblApp& app, const FMat4& vp_for_render, const FDirLight& sun) noexcept {
    app._pbr.SetLights(vp_for_render, app._camera.Eye(), &sun, 1, FVec3{0, 0, 0});
    app._pbr.SetPointLights(nullptr, 0);
}

void BindSsao(HelloIblApp& app) noexcept {
    IRhiTexture* hdr = app._post.HdrRenderTarget();
    if (!hdr) return;
    // 注: SSAO 無効時も viewport サイズは渡す。SSGI / SSR が screen UV を
    // ssao_params.zw から得るため、ここを 0 にすると参照が壊れる。
    if (app._use_ssao && app._ssao_warm) {
        app._pbr.SetSsao(app._ssao.OutputTexture(), /*intensity=*/1.0f,
                         hdr->Width(), hdr->Height());
    } else {
        app._pbr.SetSsao(nullptr, 0.0f, hdr->Width(), hdr->Height());
    }
}

void BindSsgi(HelloIblApp& app) noexcept {
    if (app._use_ssgi && app._ssgi_warm) {
        app._pbr.SetSsgi(app._ssgi.OutputTexture(), /*intensity=*/0.6f);
    } else {
        app._pbr.SetSsgi(nullptr, 0.0f);
    }
}

void BindSsr(HelloIblApp& app) noexcept {
    // FPbrShader 側で roughness 依存合成 (rough 面ほど反射が弱まる)。
    if (app._show_ssr && app._ssr_warm) {
        app._pbr.SetSsr(app._ssr.OutputTexture(), /*intensity=*/1.0f);
    } else {
        app._pbr.SetSsr(nullptr, 0.0f);
    }
}

void BindShadow(HelloIblApp& app) noexcept {
    if (app._use_shadows) {
        FMat4 vps   [FShadowMap::kMaxCascades] = {};
        f32  splits[FShadowMap::kMaxCascades] = {};
        for (u32 c = 0; c < app._shadow.CascadeCount(); ++c) {
            vps[c]    = app._shadow.LightViewProjection(c);
            splits[c] = app._shadow.CascadeSplit(c);
        }
        app._pbr.SetShadowMapCascades(app._shadow.DepthTexture(), vps, splits,
                                      app._shadow.CascadeCount(),
                                      /*bias=*/0.002f,
                                      /*texel_size=*/1.0f / static_cast<f32>(app._shadow.Size()));
    } else {
        app._pbr.SetShadowMap(nullptr, FMat4{}, 0, 0);
    }
}

void BindFog(HelloIblApp& app) noexcept {
    // 灰色 fog、密度 0.12 / 高さ減衰 0.2
    if (app._use_fog) {
        app._pbr.SetFog(FVec3{0.65f, 0.7f, 0.8f}, 0.12f, 0.2f, 0.0f);
    } else {
        app._pbr.SetFog(FVec3{0, 0, 0}, 0.0f);
    }
}

void BindProbeGrid(HelloIblApp& app) noexcept {
    if (app._use_probe_grid) {
        // 計算済 _sh9 (現 env の SH9) をベースに、左右の probe を赤/青に着色
        FPbrShader::FLightProbe p[2];
        p[0].position = FVec3{-4.0f, 1.5f, 3.0f};   // 左 probe (赤光)
        for (u32 k = 0; k < 9; ++k) p[0].sh9[k] = app._sh9[k];
        p[0].sh9[0] = p[0].sh9[0] + FVec4{2.5f, 0.4f, 0.4f, 0};   // l=0 (DC) に赤を強める

        p[1].position = FVec3{+4.0f, 1.5f, 3.0f};   // 右 probe (青光)
        for (u32 k = 0; k < 9; ++k) p[1].sh9[k] = app._sh9[k];
        p[1].sh9[0] = p[1].sh9[0] + FVec4{0.4f, 0.4f, 2.5f, 0};

        app._pbr.SetProbeGrid(p, 2);
        app._need_sh9_rebuild = true;       // SH9 base が古ければ次フレームで再計算
    } else {
        app._pbr.SetProbeGrid(nullptr, 0);
    }
}

void BindAreaLight(HelloIblApp& app) noexcept {
    // 球グリッドの前方上空に置いた 2x1 矩形パネル
    if (app._use_area_light) {
        FPbrShader::FAreaLight rect;
        rect.center = FVec3{0.0f, 4.0f, 1.0f};      // 上空、camera 側
        rect.axis_x = FVec3{1.0f, 0.0f, 0.0f};      // 横半幅 = 1
        rect.axis_y = FVec3{0.0f, 0.0f, 0.5f};      // 奥行半高 = 0.5
        rect.color  = FVec3{5.0f, 4.5f, 3.5f};      // 暖色 HDR
        app._pbr.SetAreaLights(&rect, 1);
    } else {
        app._pbr.SetAreaLights(nullptr, 0);
    }
}

} // anonymous namespace

void BindPbrLighting(HelloIblApp& app, const FMat4& vp_for_render, const FDirLight& sun) noexcept {
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
