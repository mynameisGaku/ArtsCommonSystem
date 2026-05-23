// SPDX-License-Identifier: Apache-2.0
// Image-Based Lighting (Phase 31)
//
// PBR の ambient 項を「環境マップから事前積分した光」で置き換える。
// 構成要素:
//   ・BRDF LUT       — 2D 256x256 RG16F。GGX split-sum approximation の scale+bias
//   ・環境 cubemap   — シーンの背景 (Sky 等から captured)。256x256x6、R11G11B10_Float
//   ・拡散 irradiance cubemap — 32x32x6、半球積分された diffuse 反射
//   ・specular prefilter cubemap — 128x128x6 (5 mip)、roughness 段階別 GGX 反射
//
// 使い方 (HelloIbl):
//   ImageBasedLighting ibl;
//   ibl.EnsureBrdfLut(*dev, *cl);              // 初回のみ LUT 生成 (256x256)
//   ibl.EnsureEnvCubemap(*dev, *cl, sky);       // 初回のみ env cubemap キャプチャ
//   // (Phase 31 Step 4 以降で:)
//   // ibl.EnsureIrradiance(*cl);                // 環境 → irradiance
//   // ibl.EnsurePrefilter(*cl);                 // 環境 → prefilter (mip)
//   // pbr.SetIbl(*ibl.IrradianceMap(), *ibl.PrefilterMap(), *ibl.BrdfLut());
//
// 注意: Diligent backend 専用 (BeginRenderToTextureSlice / cubemap 依存)。
//       Dx12 raw backend では Init は成功するが LUT 生成は no-op になる。
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "math/Mat.h"
#include "math/Vec.h"
#include "render/IRhiDevice.h"
#include "render/IRhiShader.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiBuffer.h"
#include "render/IRhiTexture.h"
#include "render/IRhiCommandList.h"
#include "render/RhiTypes.h"

namespace acs {

class Sky;

class ImageBasedLighting {
public:
    ImageBasedLighting() noexcept = default;
    ~ImageBasedLighting() noexcept = default;

    ImageBasedLighting(const ImageBasedLighting&)            = delete;
    ImageBasedLighting& operator=(const ImageBasedLighting&) = delete;

    // 初回呼び出しで BRDF LUT を 1 回だけ生成する。以降の呼び出しは no-op。
    // cl は frame 内 (BeginFrame と EndFrame の間) で記録中である必要あり。
    Result<void> EnsureBrdfLut(IRhiDevice& device, IRhiCommandList& cl) noexcept;

    // 初回呼び出しで env cubemap (256x256x6, R11G11B10_Float) を Sky 手続き式から
    // キャプチャする。各 face を 6 回の per-slice draw で塗る。
    // sky の現在のパラメータ (sun_dir / colors / 等) がスナップショットされる。
    // 再キャプチャしたいときは Shutdown() で全 reset するか、別途 Recapture API
    // を追加する (Step 4+ で必要なら)。
    Result<void> EnsureEnvCubemap(IRhiDevice& device, IRhiCommandList& cl,
                                  const Sky& sky) noexcept;

    // 既存 env cubemap を破棄して equirectangular HDR 画像 (e.g. .hdr ファイル) から
    // 新しい env cubemap を生成する。`rgba_float` は width × height × 4 個の float、
    // memory layout は row-major / top-down (v=0 が天頂 +Y、v=1 が天底 -Y)。
    // 内部で R32G32B32A32_Float の Texture2D に upload → 6 face をピクセル shader で
    // 塗り直す。irradiance / prefilter は呼び出し側で再 Ensure する必要あり。
    Result<void> LoadEquirectHdrFromMemory(IRhiDevice& device, IRhiCommandList& cl,
                                            const f32* rgba_float, u32 width, u32 height) noexcept;

    // SH 9 light probe (Phase 32c)。equirect 画像から Ramamoorthi-Hanrahan の
    // L_l,m 球面調和係数 9 個を CPU 側で計算し、`out_sh_rgb[9]` に書き出す。
    //   `out_sh_rgb[i].xyz` = RGB 係数、`.w` 不使用 (CB layout の便宜上 Vec4)
    // 後段 PbrShader 側で SH 9 ambient mode に切替できる (irradiance cubemap の
    // 圧縮版とみなせる、メモリ 144B のみで diffuse irradiance を近似)。
    //
    // 規約: equirect の v=0 が +Y、v=1 が -Y、u=0 が phi=-π。
    static void ComputeSh9FromEquirect(const f32* rgba_float,
                                        u32 width, u32 height,
                                        Vec4 out_sh_rgb[9]) noexcept;

    // 初回呼び出しで diffuse irradiance cubemap (32x32x6, R11G11B10_Float) を
    // env cubemap の半球積分から作る。EnsureEnvCubemap が完了していないと失敗。
    // 出力 = (1/π) ∫_Ω L_env(ω) (N·ω) dω → Lambert diffuse の ambient 項として
    // `diffuse = albedo * irradiance_cube.Sample(N)` で使う。
    Result<void> EnsureIrradiance(IRhiDevice& device, IRhiCommandList& cl) noexcept;

    // 初回呼び出しで specular prefilter cubemap (128x128x6, R11G11B10_Float, 5 mips)
    // を env cubemap の GGX importance sampling 積分から作る。各 mip は roughness 段階に
    // 対応 (mip 0 = roughness 0 / 鏡面、mip 4 = roughness 1)。
    // 実行時 PBR specular:
    //   F = F0 * lut.r + lut.g
    //   specular = prefilter.SampleLevel(R, roughness * (mips-1)).rgb * F
    Result<void> EnsurePrefilter(IRhiDevice& device, IRhiCommandList& cl) noexcept;

    // デバッグ用: 任意の cubemap を fullscreen quad の skybox として現在の RT へ描画する。
    // ・rt_format / depth_format は現在 bind 中の swapchain と一致させること
    // ・depth_test / write は無効 (背景描画)
    // ・初回呼び出しで PSO/CB を lazy init する (env_cube / irradiance / prefilter
    //   で同じ pipeline を共有)
    // ・mip_level: prefilter で roughness 段階を選ぶときに 0..(prefilter_mips-1) を渡す。
    //   env / irradiance では 0 を渡す。
    void DrawSkybox(IRhiDevice& device, IRhiCommandList& cl,
                    IRhiTexture& cube,
                    const Mat4& view_proj, Vec3 eye,
                    EFormat rt_format, EFormat depth_format,
                    f32 mip_level = 0.0f) noexcept;

    // 利便用 (env_cube を skybox 描画)
    void DrawEnvSkybox(IRhiDevice& device, IRhiCommandList& cl,
                       const Mat4& view_proj, Vec3 eye,
                       EFormat rt_format, EFormat depth_format) noexcept;

    // 環境 cubemap (+ それに依存する irradiance / 将来の prefilter) だけを reset。
    // Sky preset 切替などで env を作り直したいとき、BRDF LUT (sky 非依存) は
    // 残せる。**呼び出し前にデバイスの `WaitIdle()` を呼ぶこと**: 前フレームの
    // GPU 描画がまだこのテクスチャを参照中だと UB になる。
    void ResetEnvCubemap() noexcept;

    void Shutdown() noexcept;

    // BRDF LUT (256x256 RG16F)。EnsureBrdfLut 完了後に有効。
    IRhiTexture* BrdfLut()    const noexcept { return _brdf_lut.Get(); }
    bool         HasBrdfLut() const noexcept { return static_cast<bool>(_brdf_lut); }

    // 環境 cubemap (256x256x6 R11G11B10_Float)。EnsureEnvCubemap 完了後に有効。
    IRhiTexture* EnvCubemap()    const noexcept { return _env_cube.Get(); }
    bool         HasEnvCubemap() const noexcept { return static_cast<bool>(_env_cube); }

    // Diffuse irradiance cubemap (32x32x6 R11G11B10_Float)。EnsureIrradiance 完了後に有効。
    IRhiTexture* IrradianceMap()    const noexcept { return _irradiance_cube.Get(); }
    bool         HasIrradianceMap() const noexcept { return static_cast<bool>(_irradiance_cube); }

    // Specular prefilter cubemap (128x128x6 R11G11B10_Float, 5 mips)。EnsurePrefilter 完了後に有効。
    IRhiTexture* PrefilterMap()    const noexcept { return _prefilter_cube.Get(); }
    bool         HasPrefilterMap() const noexcept { return static_cast<bool>(_prefilter_cube); }
    u32          PrefilterMips()   const noexcept { return _prefilter_mips; }

private:
    Result<void> BuildBrdfLut(IRhiDevice& device, IRhiCommandList& cl) noexcept;
    Result<void> BuildEnvCubemap(IRhiDevice& device, IRhiCommandList& cl,
                                 const Sky& sky) noexcept;
    Result<void> BuildIrradiance(IRhiDevice& device, IRhiCommandList& cl) noexcept;
    Result<void> BuildPrefilter(IRhiDevice& device, IRhiCommandList& cl) noexcept;
    Result<void> EnsureSkyboxPipeline(IRhiDevice& device,
                                      EFormat rt_format, EFormat depth_format) noexcept;

    UniquePtr<IRhiTexture>  _brdf_lut;
    UniquePtr<IRhiTexture>  _env_cube;
    UniquePtr<IRhiTexture>  _irradiance_cube;
    UniquePtr<IRhiTexture>  _prefilter_cube;
    u32                     _prefilter_mips = 0;

    // Skybox preview パイプライン (lazy init)
    UniquePtr<IRhiShader>   _sky_vs;
    UniquePtr<IRhiShader>   _sky_ps;
    UniquePtr<IRhiPipeline> _sky_pipeline;
    UniquePtr<IRhiBuffer>   _sky_cb;
    EFormat                  _sky_rt_format    = EFormat::Unknown;
    EFormat                  _sky_depth_format = EFormat::Unknown;

    bool _brdf_built       = false;
    bool _env_built        = false;
    bool _irradiance_built = false;
    bool _prefilter_built  = false;
};

} // namespace acs
