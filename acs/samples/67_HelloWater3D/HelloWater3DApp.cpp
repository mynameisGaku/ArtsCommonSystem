// SPDX-License-Identifier: Apache-2.0
#include "HelloWater3DApp.h"

#include "app/Sample.h"
#include "asset/MeshAsset.h"
#include "foundation/Log.h"
#include "foundation/Move.h"
#include "math/CameraRig.h"
#include "math/Math.h"
#include "memory/Allocator.h"
#include "memory/Memory.h"
#include "memory/SharedPtr.h"
#include "platform/Input.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiDevice.h"
#include "render/IRhiSwapchain.h"
#include "render/RhiTypes.h"

#include <cmath>
#include <cstdio>

using namespace acs;

namespace hellowater3d {

namespace {

constexpr f32 kWaterHalfExtent = 7.8f;

TSharedPtr<FMeshAsset> MakeWaterGrid(f32 width, f32 depth,
                                     u32 columns, u32 rows) noexcept {
    if (columns < 2) columns = 2;
    if (rows < 2) rows = 2;
    auto mesh = MakeShared<FMeshAsset>();
    if (!mesh) return TSharedPtr<FMeshAsset>();

    auto& vertices = mesh->Vertices();
    auto& indices = mesh->Indices();
    vertices.Reserve((columns + 1u) * (rows + 1u));
    indices.Reserve(columns * rows * 6u);
    const f32 half_width = width * 0.5f;
    const f32 half_depth = depth * 0.5f;

    for (u32 row = 0; row <= rows; ++row) {
        const f32 v = static_cast<f32>(row) / static_cast<f32>(rows);
        const f32 z = -half_depth + v * depth;
        for (u32 column = 0; column <= columns; ++column) {
            const f32 u =
                static_cast<f32>(column) / static_cast<f32>(columns);
            FMeshVertex vertex{};
            vertex.position = FVec3{-half_width + u * width, 0.0f, z};
            vertex.normal = FVec3{0.0f, 1.0f, 0.0f};
            vertex.u = u;
            vertex.v = 1.0f - v;
            vertices.PushBack(vertex);
        }
    }

    const u32 stride = columns + 1u;
    for (u32 row = 0; row < rows; ++row) {
        for (u32 column = 0; column < columns; ++column) {
            const u32 i0 = row * stride + column;
            const u32 i1 = i0 + 1u;
            const u32 i2 = i0 + stride;
            const u32 i3 = i2 + 1u;
            // Same clockwise top-face convention as Primitive::MakePlane.
            indices.PushBack(i0);
            indices.PushBack(i1);
            indices.PushBack(i3);
            indices.PushBack(i0);
            indices.PushBack(i3);
            indices.PushBack(i2);
        }
    }

    FSubMesh submesh{};
    submesh.first_index = 0;
    submesh.index_count = static_cast<u32>(indices.Size());
    mesh->SubMeshes().PushBack(submesh);
    return mesh;
}

TSharedPtr<FMeshAsset> MakeFloorPlane(f32 width, f32 depth) noexcept {
    auto mesh = MakeShared<FMeshAsset>();
    if (!mesh) return TSharedPtr<FMeshAsset>();
    auto& vertices = mesh->Vertices();
    auto& indices = mesh->Indices();
    const f32 half_width = width * 0.5f;
    const f32 half_depth = depth * 0.5f;

    FMeshVertex a{};
    a.position = FVec3{-half_width, 0, -half_depth};
    a.normal = FVec3{0, 1, 0};
    a.u = 0; a.v = 8;
    FMeshVertex b = a;
    b.position = FVec3{half_width, 0, -half_depth};
    b.u = 8; b.v = 8;
    FMeshVertex c = a;
    c.position = FVec3{half_width, 0, half_depth};
    c.u = 8; c.v = 0;
    FMeshVertex d = a;
    d.position = FVec3{-half_width, 0, half_depth};
    d.u = 0; d.v = 0;
    vertices.PushBack(a);
    vertices.PushBack(b);
    vertices.PushBack(c);
    vertices.PushBack(d);
    indices.PushBack(0); indices.PushBack(1); indices.PushBack(2);
    indices.PushBack(0); indices.PushBack(2); indices.PushBack(3);
    // Keep the refraction reference floor visible on both RHI backends. Their
    // current rasterizer front-face conventions differ, while StandardShader
    // deliberately back-face culls.
    indices.PushBack(2); indices.PushBack(1); indices.PushBack(0);
    indices.PushBack(3); indices.PushBack(2); indices.PushBack(0);
    FSubMesh submesh{};
    submesh.first_index = 0;
    submesh.index_count = 12;
    mesh->SubMeshes().PushBack(submesh);
    return mesh;
}

TResult<TUniquePtr<IRhiTexture>> CreateFloorTexture(IRhiDevice& device) noexcept {
    constexpr u32 kSize = 256;
    constexpr usize kBytes =
        static_cast<usize>(kSize) * static_cast<usize>(kSize) * 4u;
    FAllocator& allocator = DefaultAllocator();
    u8* pixels = static_cast<u8*>(allocator.Alloc(kBytes));
    if (!pixels) {
        return Err<TUniquePtr<IRhiTexture>>(
            ACS_ERR(Render, 720, "HelloWater3D floor texture allocation failed"));
    }

    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            const u32 tile = ((x / 32u) + (y / 32u)) & 1u;
            const f32 sediment =
                0.5f + 0.5f * std::sin(static_cast<f32>(x) * 0.19f
                                       + static_cast<f32>(y) * 0.11f);
            const f32 grain =
                0.5f + 0.5f * std::sin(static_cast<f32>(x * 17u + y * 31u));
            const f32 base = tile ? 0.68f : 0.42f;
            const f32 detail = base + sediment * 0.10f + grain * 0.035f;
            const usize index =
                (static_cast<usize>(y) * kSize + x) * 4u;
            pixels[index + 0] = static_cast<u8>(detail * 134.0f);
            pixels[index + 1] = static_cast<u8>(detail * 115.0f);
            pixels[index + 2] = static_cast<u8>(detail * 76.0f);
            pixels[index + 3] = 255;
        }
    }

    FTextureDesc description{};
    description.width = kSize;
    description.height = kSize;
    description.format = EFormat::R8G8B8A8_UNorm;
    description.initial_data = pixels;
    description.initial_data_size = kBytes;
    auto result = CreateRhiTexture(device, description);
    allocator.Free(pixels);
    return result;
}

} // namespace

void FHelloWater3DApp::OnStart() noexcept {
    IRhiDevice* device = GetRenderer().Device();
    IRhiSwapchain* swapchain = GetRenderer().Swapchain();
    if (!device || !swapchain) {
        Quit();
        return;
    }

    const u32 width = swapchain->Width();
    const u32 height = swapchain->Height();
    ACS_SAMPLE_INIT(
        m_Post.Init(*device, width, height, GetRenderer().ColorFormat()));
    ACS_SAMPLE_INIT(
        m_Sky.Init(*device, m_Post.HdrFormat(), GetRenderer().DepthFormat()));
    ACS_SAMPLE_INIT(
        m_OpaqueShader.Init(*device, m_Post.HdrFormat(),
                            GetRenderer().DepthFormat()));
    ACS_SAMPLE_INIT(
        m_Water.Init(*device, m_Post.HdrFormat(),
                     GetRenderer().DepthFormat()));
    ACS_SAMPLE_INIT(m_Blit.Init(*device, m_Post.HdrFormat()));
    ACS_SAMPLE_INIT(m_Batch.Init(*device, GetRenderer().ColorFormat()));
    (void)FSample::TryLoadDefaultUIFont(m_Font, *device, 18.0f, 1024, true);

    m_Sky.PresetDay();
    FWaterSurface3DParams water_parameters{};
    water_parameters.shallow_color = FVec3{0.045f, 0.38f, 0.50f};
    water_parameters.deep_color = FVec3{0.004f, 0.040f, 0.14f};
    water_parameters.absorption = FVec3{0.40f, 0.145f, 0.038f};
    water_parameters.scattering = FVec3{0.016f, 0.052f, 0.090f};
    water_parameters.phase_anisotropy = 0.64f;
    water_parameters.roughness = 0.085f;
    water_parameters.normal_strength = 0.86f;
    water_parameters.normal_tiling = 0.082f;
    water_parameters.refraction_strength = 0.78f;
    water_parameters.optical_depth = 1.18f;
    water_parameters.wave_amplitude = 0.11f;
    water_parameters.wave_scale = 0.86f;
    water_parameters.ripple_lifetime = 4.4f;
    water_parameters.ripple_damping = 0.68f;
    water_parameters.foam_intensity = 0.68f;
    m_Water.SetParams(water_parameters);

    auto water_mesh = MakeWaterGrid(kWaterHalfExtent * 2.0f,
                                    kWaterHalfExtent * 2.0f, 128, 128);
    auto floor_mesh = MakeFloorPlane(kWaterHalfExtent * 2.0f,
                                    kWaterHalfExtent * 2.0f);
    if (!water_mesh || !floor_mesh) {
        ACS_LOG_ERROR("HelloWater3D: mesh allocation failed");
        Quit();
        return;
    }
    ACS_SAMPLE_INIT(UploadMesh(*device, *water_mesh, m_WaterMesh));
    ACS_SAMPLE_INIT(UploadMesh(*device, *floor_mesh, m_FloorMesh));

    auto floor_texture_result = CreateFloorTexture(*device);
    if (floor_texture_result.IsErr()) {
        ACS_LOG_ERROR("HelloWater3D: floor texture failed: %s",
                      floor_texture_result.Error().message);
        Quit();
        return;
    }
    m_FloorTexture = Move(floor_texture_result.Value());

    if (!ResizeFrameResources(width, height, false)) {
        Quit();
        return;
    }

    m_PostParams.bloom_threshold = 1.15f;
    m_PostParams.bloom_intensity = 0.28f;
    m_PostParams.bloom_radius = 0.82f;
    m_PostParams.bloom_scatter = 0.62f;
    m_PostParams.exposure = 0.86f;
    m_PostParams.tonemap_kind = 0;
    m_PostParams.vignette_intensity = 0.08f;
    m_PostParams.vignette_radius = 0.82f;
    m_PostParams.grain_intensity = 0.002f;
    m_PostParams.cas_strength = 0.16f;

    // Make persistence visible before the first user interaction.
    (void)m_Water.AddDisturbance(FVec3{-2.2f, 0, 0.8f}, 0.16f, 0.22f);
    (void)m_Water.AddDisturbance(FVec3{ 1.8f, 0, -1.1f}, 0.12f, 0.18f);
    ACS_LOG_INFO("HelloWater3D initialized: %s", device->BackendName());
}

bool FHelloWater3DApp::ResizeFrameResources(
    u32 width, u32 height, bool resize_post_process) noexcept {
    if (width == 0 || height == 0) return true;

    IRhiDevice* device = GetRenderer().Device();
    if (!device) {
        ACS_LOG_ERROR(
            "HelloWater3D: no RHI device while resizing to %ux%u",
            width, height);
        return false;
    }

    FTextureDesc scene_copy_description{};
    scene_copy_description.width = width;
    scene_copy_description.height = height;
    scene_copy_description.format = m_Post.HdrFormat();
    scene_copy_description.is_render_target = true;
    auto scene_copy_result =
        CreateRhiTexture(*device, scene_copy_description);
    if (scene_copy_result.IsErr()) {
        ACS_LOG_ERROR("HelloWater3D: scene capture failed: %s",
                      scene_copy_result.Error().message);
        return false;
    }

    if (resize_post_process) {
        auto post_resize_result = m_Post.Resize(width, height);
        if (post_resize_result.IsErr()) {
            ACS_LOG_ERROR(
                "HelloWater3D: post-process resize failed at %ux%u: %s",
                width, height, post_resize_result.Error().message);
            return false;
        }
    }

    m_SceneCopy = Move(scene_copy_result.Value());
    const f32 aspect = static_cast<f32>(width)
        / static_cast<f32>(height);
    m_Camera.SetPerspective(52.0f * kDeg2Rad, aspect, 0.1f, 120.0f);
    UpdateCamera();
    return true;
}

void FHelloWater3DApp::OnEvent(const FEvent& event) noexcept {
    if (event.type != EEventType::WindowResize
        || event.resize.width == 0 || event.resize.height == 0) {
        return;
    }

    if (!ResizeFrameResources(
            event.resize.width, event.resize.height, true)) {
        Quit();
    }
}

void FHelloWater3DApp::UpdateCamera() noexcept {
    const f32 cosine_pitch = Cos(m_CameraPitch);
    m_CameraPosition = FVec3{
        m_CameraTarget.x
            + Sin(m_CameraYaw) * cosine_pitch * m_CameraDistance,
        m_CameraTarget.y + Sin(m_CameraPitch) * m_CameraDistance,
        m_CameraTarget.z
            - Cos(m_CameraYaw) * cosine_pitch * m_CameraDistance,
    };
    m_Camera.SetLookAt(m_CameraPosition, m_CameraTarget, FVec3::Up());
}

bool FHelloWater3DApp::MouseToWater(FVec3& world_point) noexcept {
    IRhiSwapchain* swapchain = GetRenderer().Swapchain();
    if (!swapchain || swapchain->Width() == 0 || swapchain->Height() == 0) {
        return false;
    }

    const FVec2 mouse = FInput::MousePos();
    const FRay3 ray = ScreenPointToRay(
        m_Camera, mouse.x, mouse.y,
        static_cast<f32>(swapchain->Width()),
        static_cast<f32>(swapchain->Height()));
    if (Abs(ray.direction.y) < 1e-6f) return false;
    const f32 distance = -ray.origin.y / ray.direction.y;
    if (distance <= 0.0f) return false;
    const FVec3 point{
        ray.origin.x + ray.direction.x * distance,
        0.0f,
        ray.origin.z + ray.direction.z * distance,
    };
    if (Abs(point.x) > kWaterHalfExtent
        || Abs(point.z) > kWaterHalfExtent) {
        return false;
    }
    world_point = point;
    return true;
}

void FHelloWater3DApp::OnUpdate(f32 dt) noexcept {
    if (FInput::IsKeyPressed(EKey::Escape)) {
        Quit();
        return;
    }
    if (!std::isfinite(dt) || dt < 0.0f) dt = 0.0f;
    m_FrameDt = dt;
    m_PostParams.delta_time = dt;
    m_PostParams.grain_time += dt;

    const FVec2 mouse_delta = FInput::MouseDelta();
    if (FInput::IsMouseButtonDown(EMouseButton::Right)) {
        m_CameraYaw += mouse_delta.x * 0.006f;
        m_CameraPitch += mouse_delta.y * 0.005f;
    }
    const f32 keyboard_turn = 1.2f * dt;
    if (FInput::IsKeyDown(EKey::Left)) m_CameraYaw -= keyboard_turn;
    if (FInput::IsKeyDown(EKey::Right)) m_CameraYaw += keyboard_turn;
    if (FInput::IsKeyDown(EKey::Up)) m_CameraPitch += keyboard_turn * 0.72f;
    if (FInput::IsKeyDown(EKey::Down)) m_CameraPitch -= keyboard_turn * 0.72f;
    if (m_CameraPitch < 0.14f) m_CameraPitch = 0.14f;
    if (m_CameraPitch > 1.28f) m_CameraPitch = 1.28f;
    m_CameraDistance -= FInput::MouseWheel() * 0.85f;
    if (m_CameraDistance < 4.5f) m_CameraDistance = 4.5f;
    if (m_CameraDistance > 20.0f) m_CameraDistance = 20.0f;
    UpdateCamera();

    m_Water.Update(dt);
    if (FInput::IsKeyPressed(EKey::C)) m_Water.ClearDisturbances();
    if (FInput::IsKeyPressed(EKey::Space)) {
        (void)m_Water.AddDisturbance(FVec3{0, 0, 0}, 0.25f, 0.30f);
    }

    FVec3 water_point{};
    m_MouseHitsWater = MouseToWater(water_point);
    const bool pressed =
        FInput::IsMouseButtonPressed(EMouseButton::Left);
    const bool dragging =
        FInput::IsMouseButtonDown(EMouseButton::Left);

    if (pressed && m_MouseHitsWater) {
        (void)m_Water.AddDisturbance(water_point, 0.17f, 0.24f);
        m_LastDragPoint = water_point;
        m_DragTravel = 0.0f;
        m_HasLastDragPoint = true;
    } else if (dragging && m_MouseHitsWater && m_HasLastDragPoint
               && dt > 1e-5f) {
        const FVec3 delta{
            water_point.x - m_LastDragPoint.x,
            0.0f,
            water_point.z - m_LastDragPoint.z,
        };
        const f32 travel =
            std::sqrt(delta.x * delta.x + delta.z * delta.z);
        m_DragTravel += travel;
        constexpr f32 kWakeSpacing = 0.28f;
        if (travel > 1e-5f && m_DragTravel >= kWakeSpacing) {
            const FVec3 velocity{
                delta.x / dt, 0.0f, delta.z / dt,
            };
            (void)m_Water.AddWake(water_point, velocity, 0.19f, 0.13f);
            m_DragTravel -= kWakeSpacing;
        }
        m_LastDragPoint = water_point;
    }

    if (!dragging || !m_MouseHitsWater) {
        m_HasLastDragPoint = false;
        m_DragTravel = 0.0f;
    }
}

bool FHelloWater3DApp::OnCustomFrame() noexcept {
    IRhiCommandList* command_list = GetRenderer().CommandList();
    IRhiSwapchain* swapchain = GetRenderer().Swapchain();
    IRhiTexture* hdr = m_Post.HdrRenderTarget();
    IRhiTexture* depth = GetRenderer().DepthBuffer();
    if (!command_list || !swapchain || !hdr || !depth
        || !m_SceneCopy || !m_FloorTexture) {
        return false;
    }

    const u32 buffer_index = swapchain->AcquireNextImage();
    command_list->Begin();
    command_list->BeginRenderToTexture(
        *hdr, FClearColor{0.035f, 0.065f, 0.10f, 1.0f}, depth, 1.0f);
    FViewport viewport{};
    viewport.width = static_cast<f32>(hdr->Width());
    viewport.height = static_cast<f32>(hdr->Height());
    command_list->SetViewport(viewport);
    FScissorRect scissor{};
    scissor.right = static_cast<i32>(hdr->Width());
    scissor.bottom = static_cast<i32>(hdr->Height());
    command_list->SetScissor(scissor);

    m_Sky.Render(*command_list, m_Camera);
    const FVec3 sun_direction = m_Sky.SunDirection();
    const FVec3 sun_color = m_Sky.SunColor();
    m_OpaqueShader.SetFrame(
        m_Camera.ViewProjection(), m_CameraPosition,
        sun_direction, sun_color, FVec3{0.10f, 0.14f, 0.17f});
    const FMat4 floor_model =
        FMat4::Translation(FVec3{0.0f, -1.28f, 0.0f});
    m_OpaqueShader.DrawMesh(
        *command_list, m_FloorMesh, floor_model,
        FVec3{0.82f, 0.78f, 0.64f}, 0.10f, 24.0f,
        m_FloorTexture.Get());
    command_list->EndRenderToTexture(*hdr);

    // The water samples a stable copy, never the render target it writes.
    m_Blit.Copy(*command_list, *hdr, *m_SceneCopy);
    // Depth must be an SRV for physically reconstructed water thickness. Bind
    // no DSV here; FWaterSurface3D selects its manual-depth-test pipeline.
    command_list->BeginRenderToTextureLoad(*hdr, nullptr);
    command_list->SetViewport(viewport);
    command_list->SetScissor(scissor);
    m_Water.SetFrame(
        m_Camera.ViewProjection(), m_CameraPosition,
        hdr->Width(), hdr->Height(), sun_direction,
        FVec3{4.8f, 4.25f, 3.7f});
    m_Water.DrawMesh(
        *command_list, m_WaterMesh, FMat4::Identity(),
        m_SceneCopy.Get(), depth);
    command_list->EndRenderToTexture(*hdr);

    m_Post.Render(*command_list, *swapchain, buffer_index, m_PostParams);

    m_Batch.Begin(*command_list, swapchain->Width(), swapchain->Height());
    if (m_Font.AtlasTexture()) {
        m_Batch.DrawRect(
            10, 10, 990, 78, FVec4{0.0f, 0.0f, 0.0f, 0.52f});
        m_Batch.DrawString(
            m_Font,
            "3D WATER  layered normal map + geometry waves + Fresnel + refraction + foam",
            20, 18, FVec4{0.88f, 0.96f, 1.0f, 1.0f});
        char status[192]{};
        std::snprintf(
            status, sizeof(status),
            "persistent ripples: %u / %u    FPS: %.1f",
            m_Water.ActiveRippleCount(), FWaterSurface3D::kMaxRipples,
            static_cast<double>(FPS()));
        m_Batch.DrawString(
            m_Font, status, 20, 42,
            FVec4{0.58f, 1.0f, 0.75f, 1.0f});
        m_Batch.DrawString(
            m_Font,
            "LMB click: impact  LMB drag: wake  RMB drag/arrows: orbit  wheel: zoom  C: clear",
            20, 64, FVec4{0.74f, 0.86f, 1.0f, 1.0f});
    }
    const FVec2 mouse = FInput::MousePos();
    const FVec4 cursor_color = m_MouseHitsWater
        ? FVec4{0.62f, 1.0f, 0.78f, 0.92f}
        : FVec4{1.0f, 0.45f, 0.32f, 0.82f};
    m_Batch.DrawRect(mouse.x - 7.0f, mouse.y - 1.0f,
                     14.0f, 2.0f, cursor_color);
    m_Batch.DrawRect(mouse.x - 1.0f, mouse.y - 7.0f,
                     2.0f, 14.0f, cursor_color);
    m_Batch.End();

    command_list->EndRenderToSwapchain(*swapchain, buffer_index);
    command_list->End();
    command_list->Submit();
    swapchain->Present();
    return true;
}

void FHelloWater3DApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    m_Font.Shutdown();
    m_Batch.Shutdown();
    m_SceneCopy.Reset();
    m_FloorTexture.Reset();
    m_WaterMesh = FGpuMesh{};
    m_FloorMesh = FGpuMesh{};
    m_Blit.Shutdown();
    m_Water.Shutdown();
    m_OpaqueShader.Shutdown();
    m_Sky.Shutdown();
    m_Post.Shutdown();
}

} // namespace hellowater3d
