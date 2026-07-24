// SPDX-License-Identifier: Apache-2.0
// Screen-space subsurface diffusion for opaque HDR lighting.
#pragma once

#include "foundation/Result.h"
#include "math/Mat.h"
#include "math/Vec.h"
#include "memory/UniquePtr.h"
#include "render/IRhiBuffer.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiDevice.h"
#include "render/IRhiPipeline.h"
#include "render/IRhiShader.h"
#include "render/IRhiTexture.h"

namespace acs {

/**
 * Authoring controls for separable screen-space subsurface diffusion.
 */
struct FSubsurfaceScatteringParams {
    /** Master switch. Disabled renders leave OutputTexture untouched. */
    bool enabled = true;

    /**
     * Fallback diffusion radius in scene world units.
     *
     * The PBR MRT writes authored per-channel world radii. This value is used
     * only for legacy/custom material buffers whose RGB profile is empty.
     */
    f32 radius_world = 0.012f;

    /**
     * Fallback relative RGB diffusion radii for an empty material profile.
     */
    FVec3 channel_radius = FVec3{1.0f, 0.55f, 0.25f};

    /** Blend between original and diffused diffuse lighting. */
    f32 strength = 1.0f;

    /** Minimum world-space plane-distance tolerance for the bilateral gate. */
    f32 depth_sigma = 0.001f;

    /** Cosine-lobe exponent for normal discontinuity rejection. */
    f32 normal_power = 24.0f;

    /** Hard screen-space radius bound for stable finite GPU cost. */
    f32 max_radius_pixels = 64.0f;

    /** Replace non-finite values and clamp shader-facing ranges. */
    void Sanitize() noexcept;
};

/**
 * HDR screen-space subsurface scattering using two separable bilateral passes.
 *
 * The input contract deliberately separates diffuse irradiance from complete
 * scene color. The final pass adds only `(blurredDiffuse - originalDiffuse)`
 * to scene color, so specular reflection, emissive lighting and clear-coat
 * remain sharp. A constant diffuse field is reproduced exactly because every
 * channel is independently normalized by its accumulated kernel weight.
 *
 * material_data is an RGBA16F physical profile:
 *   RGB = authored per-channel diffusion radius in scene world units
 *   A   = SSS coverage/strength [0,1]
 *
 * Substrate mean-free-path and thickness, or the legacy subsurface color and
 * scalar, therefore survive the MRT at half-float precision. The blur compares
 * center and neighbour RGB profiles to preserve material boundaries and uses
 * the fallback parameters above only when a custom/legacy producer supplies an
 * empty RGB profile.
 *
 * Integration order:
 *   opaque lighting + SSS buffers -> FSubsurfaceScattering -> transparent /
 *   atmosphere -> scene-linear TAA -> exposure -> bloom -> tone map.
 */
class FSubsurfaceScattering {
public:
    /** Shader handles compiled away from owner-thread PSO/resource creation. */
    struct FCompiledShaders {
        TUniquePtr<IRhiShader> vertex;
        TUniquePtr<IRhiShader> blur_pixel;
        TUniquePtr<IRhiShader> composite_pixel;

        /** Aggregate backend-managed asynchronous compilation without waiting. */
        EShaderStatus Status() const noexcept;
    };

    FSubsurfaceScattering() noexcept = default;
    ~FSubsurfaceScattering() noexcept = default;

    FSubsurfaceScattering(const FSubsurfaceScattering&) = delete;
    FSubsurfaceScattering& operator=(const FSubsurfaceScattering&) = delete;

    /** Create shaders, PSOs, constant buffers and full-resolution HDR targets. */
    TResult<void> Init(IRhiDevice& device, u32 width, u32 height) noexcept;

    /**
     * Compile raw-DX12 bytecode without touching the render device.
     *
     * This is safe to run on the editor's CPU-only shader worker.
     */
    static TResult<FCompiledShaders> CompileShadersCpu() noexcept;

    /** Submit all shader stages to a supporting asynchronous backend. */
    static TResult<FCompiledShaders> BeginCompileShadersAsync(
        IRhiDevice& device) noexcept;

    /**
     * Create PSOs, constant buffers and targets from ready shader handles.
     *
     * This is the bounded render-owner-thread commit paired with either
     * CompileShadersCpu or BeginCompileShadersAsync.
     */
    TResult<void> InitWithCompiledShaders(
        IRhiDevice& device,
        FCompiledShaders&& shaders,
        u32 width,
        u32 height) noexcept;

    /**
     * Publish only shaders, PSOs and constant buffers.
     *
     * Full-resolution targets remain unallocated until Resize is called. This
     * lets runtime startup stage pipeline creation and the two internal HDR
     * allocations across separate bounded GPU commits.
     */
    TResult<void> InitPipelineResourcesWithCompiledShaders(
        IRhiDevice& device,
        FCompiledShaders&& shaders) noexcept;

    /**
     * Build an unpublished raw-DX12 pipeline candidate on a worker thread.
     *
     * The target pair is intentionally excluded and remains an owner-thread,
     * later-frame Resize commit.
     */
    TResult<void> BuildPipelineCandidateForRawDx12(
        IRhiDevice& device,
        FCompiledShaders&& shaders) noexcept;

    /** Release all owned GPU resources. */
    void Shutdown() noexcept;

    /** Atomically recreate the two full-resolution HDR targets. */
    TResult<void> Resize(u32 width, u32 height) noexcept;

    /**
     * Record horizontal diffusion and vertical diffusion/composition.
     *
     * @return true when both passes were recorded. False means callers must
     * keep using scene_color rather than OutputTexture().
     */
    bool Render(IRhiCommandList& command_list,
                IRhiTexture& scene_color,
                IRhiTexture& diffuse_lighting,
                IRhiTexture& scene_depth,
                IRhiTexture& normal_gbuffer,
                IRhiTexture& material_data,
                const FMat4& inverse_view_projection,
                const FSubsurfaceScatteringParams& params) noexcept;

    /** Complete HDR scene color after diffusion/composition. */
    IRhiTexture* OutputTexture() const noexcept { return m_Output.Get(); }

    /** Intermediate horizontally diffused diffuse lighting, for diagnostics. */
    IRhiTexture* HorizontalTexture() const noexcept {
        return m_Horizontal.Get();
    }

    bool IsReady() const noexcept {
        return HasPipelineResources() && m_Horizontal && m_Output;
    }

    bool HasPipelineResources() const noexcept {
        return m_Device && m_BlurPipeline && m_CompositePipeline &&
               m_HorizontalCb && m_VerticalCb;
    }

    u32 Width() const noexcept { return m_Width; }
    u32 Height() const noexcept { return m_Height; }

private:
    TResult<void> CreateTargets(IRhiDevice& device,
                                u32 width,
                                u32 height) noexcept;
    TResult<void> CreatePipelines(
        IRhiDevice& device,
        FCompiledShaders& shaders) noexcept;
    TResult<void> CreateConstantBuffers(IRhiDevice& device) noexcept;

    IRhiDevice* m_Device = nullptr;
    u32 m_Width = 0;
    u32 m_Height = 0;

    TUniquePtr<IRhiTexture> m_Horizontal;
    TUniquePtr<IRhiTexture> m_Output;

    TUniquePtr<IRhiShader> m_VertexShader;
    TUniquePtr<IRhiShader> m_BlurPixelShader;
    TUniquePtr<IRhiShader> m_CompositePixelShader;
    TUniquePtr<IRhiPipeline> m_BlurPipeline;
    TUniquePtr<IRhiPipeline> m_CompositePipeline;

    // A unique CB per draw is required by the raw DX12 upload contract: two
    // Update calls to one resource in a command recording would alias.
    TUniquePtr<IRhiBuffer> m_HorizontalCb;
    TUniquePtr<IRhiBuffer> m_VerticalCb;
};

} // namespace acs
