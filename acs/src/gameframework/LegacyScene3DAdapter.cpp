// SPDX-License-Identifier: Apache-2.0
#include "gameframework/LegacyScene3DAdapter.h"

#include "asset/MeshAsset.h"
#include "asset/MeshPrimitive.h"
#include "foundation/Log.h"
#include "foundation/Move.h"
#include "gameframework/AssetPack.h"
#include "gameframework/Game.h"
#include "gameframework/MeshComponent3D.h"
#include "gameframework/RenderContext.h"
#include "math/Mat.h"
#include "math/Math.h"
#include "platform/Input.h"
#include "platform/InputCodes.h"
#include "render/Renderer.h"

#include <cfloat>

namespace acs::game {

namespace {

AMeshComponent3D* FindMesh(ANode& node) noexcept {
    const void* kind = ComponentKindOf<AMeshComponent3D>();
    for (u32 index = 0u; index < node.ComponentCount(); ++index) {
        AComponent* component = node.ComponentAt(index);
        if (component != nullptr && component->Kind() == kind)
            return static_cast<AMeshComponent3D*>(component);
    }
    return nullptr;
}

const AMeshComponent3D* FindMesh(const ANode& node) noexcept {
    const void* kind = ComponentKindOf<AMeshComponent3D>();
    for (u32 index = 0u; index < node.ComponentCount(); ++index) {
        const AComponent* component = node.ComponentAt(index);
        if (component != nullptr && component->Kind() == kind)
            return static_cast<const AMeshComponent3D*>(component);
    }
    return nullptr;
}

void LocalBounds(
    const AMeshComponent3D& component,
    FVec3& minimum,
    FVec3& maximum) noexcept {
    minimum = FVec3{-0.5f, -0.5f, -0.5f};
    maximum = FVec3{0.5f, 0.5f, 0.5f};
    if (component.Primitive() == EMeshPrimitive3D::Plane) {
        minimum.y = 0.0f;
        maximum.y = 0.0f;
        return;
    }
    const FMeshAsset* mesh = component.Mesh();
    if (component.Primitive() != EMeshPrimitive3D::Mesh
        || mesh == nullptr || mesh->Vertices().IsEmpty()) {
        return;
    }
    minimum = FVec3{FLT_MAX, FLT_MAX, FLT_MAX};
    maximum = FVec3{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (u32 index = 0u; index < mesh->Vertices().Size(); ++index) {
        const FVec3 value = mesh->Vertices()[index].position;
        if (value.x < minimum.x) minimum.x = value.x;
        if (value.y < minimum.y) minimum.y = value.y;
        if (value.z < minimum.z) minimum.z = value.z;
        if (value.x > maximum.x) maximum.x = value.x;
        if (value.y > maximum.y) maximum.y = value.y;
        if (value.z > maximum.z) maximum.z = value.z;
    }
}

void ExpandBounds(
    FVec3 value,
    FVec3& minimum,
    FVec3& maximum) noexcept {
    if (value.x < minimum.x) minimum.x = value.x;
    if (value.y < minimum.y) minimum.y = value.y;
    if (value.z < minimum.z) minimum.z = value.z;
    if (value.x > maximum.x) maximum.x = value.x;
    if (value.y > maximum.y) maximum.y = value.y;
    if (value.z > maximum.z) maximum.z = value.z;
}

} // namespace

FScene3DLoadResult FLegacyScene3DAdapter::LoadFile(const char* path) noexcept {
    if (m_GpuReady || m_GpuAttempted) ReleaseGpu();
    m_LoadResult = TryLoadScene3DFile(m_Graph, path);
    if (m_LoadResult.Succeeded()) FrameScene();
    return m_LoadResult;
}

FScene3DLoadResult FLegacyScene3DAdapter::LoadAssetPack(
    IAssetPackReader& pack,
    const char* virtual_path) noexcept {
    if (m_GpuReady || m_GpuAttempted) ReleaseGpu();
    m_LoadResult = TryLoadScene3DAssetPack(m_Graph, pack, virtual_path);
    if (m_LoadResult.Succeeded()) FrameScene();
    return m_LoadResult;
}

void FLegacyScene3DAdapter::FrameScene() noexcept {
    FVec3 minimum{FLT_MAX, FLT_MAX, FLT_MAX};
    FVec3 maximum{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    bool found = false;
    TArray<const ANode*> stack;
    if (!stack.TryPushBack(&m_Graph.Root())) return;
    while (!stack.IsEmpty()) {
        const ANode* node = stack.Back();
        stack.PopBack();
        if (node == nullptr) continue;
        if (const AMeshComponent3D* component = FindMesh(*node)) {
            FVec3 local_minimum, local_maximum;
            LocalBounds(*component, local_minimum, local_maximum);
            const FMat4 world = node->World().ToMat4();
            for (u32 corner = 0u; corner < 8u; ++corner) {
                const FVec3 local{
                    (corner & 1u) != 0u ? local_maximum.x : local_minimum.x,
                    (corner & 2u) != 0u ? local_maximum.y : local_minimum.y,
                    (corner & 4u) != 0u ? local_maximum.z : local_minimum.z,
                };
                ExpandBounds(
                    TransformPoint(local, world), minimum, maximum);
            }
            found = true;
        }
        for (u32 index = 0u; index < node->ChildCount(); ++index)
            if (!stack.TryPushBack(node->Child(index))) return;
    }
    if (!found) {
        m_Target = FVec3{0.0f, 0.0f, 0.0f};
        m_Distance = 8.0f;
    } else {
        m_Target = (minimum + maximum) * 0.5f;
        const f32 radius = Length((maximum - minimum) * 0.5f);
        m_Distance = radius > 0.25f ? radius * 2.8f : 3.0f;
        if (m_Distance < 3.0f) m_Distance = 3.0f;
        if (m_Distance > 10000.0f) m_Distance = 10000.0f;
    }
    UpdateCameraView();
}

void FLegacyScene3DAdapter::OnEnter() noexcept {
    GetGame().SetClearColor(0.025f, 0.035f, 0.055f, 1.0f);
    FrameScene();
}

void FLegacyScene3DAdapter::OnExit() noexcept {
    if (IRhiDevice* device = GetGame().GetRenderer().Device())
        device->WaitIdle();
    ReleaseGpu();
}

void FLegacyScene3DAdapter::OnUpdate(f32 dt) noexcept {
    m_Time += dt;
    m_Graph.Update(dt);
    if (FInput::IsKeyPressed(EKey::Escape)) {
        GetGame().Quit();
        return;
    }

    const f32 turn = 1.45f * dt;
    if (FInput::IsKeyDown(EKey::Left)) m_Yaw -= turn;
    if (FInput::IsKeyDown(EKey::Right)) m_Yaw += turn;
    if (FInput::IsKeyDown(EKey::Up)) m_Pitch += turn * 0.75f;
    if (FInput::IsKeyDown(EKey::Down)) m_Pitch -= turn * 0.75f;
    const f32 pitch_limit = 0.475f * kPi;
    if (m_Pitch > pitch_limit) m_Pitch = pitch_limit;
    if (m_Pitch < -pitch_limit) m_Pitch = -pitch_limit;

    const f32 move = (m_Distance > 1.0f ? m_Distance : 1.0f) * 0.55f * dt;
    const FVec3 horizontal_forward{Sin(m_Yaw), 0.0f, Cos(m_Yaw)};
    const FVec3 right{Cos(m_Yaw), 0.0f, -Sin(m_Yaw)};
    if (FInput::IsKeyDown(EKey::W)) m_Target += horizontal_forward * move;
    if (FInput::IsKeyDown(EKey::S)) m_Target -= horizontal_forward * move;
    if (FInput::IsKeyDown(EKey::D)) m_Target += right * move;
    if (FInput::IsKeyDown(EKey::A)) m_Target -= right * move;
    if (FInput::IsKeyDown(EKey::E)) m_Target.y += move;
    if (FInput::IsKeyDown(EKey::Q)) m_Target.y -= move;
    UpdateCameraView();
}

void FLegacyScene3DAdapter::OnFixedUpdate(f32 fixed_dt) noexcept {
    m_Graph.FixedUpdate(fixed_dt);
}

void FLegacyScene3DAdapter::OnRender(FRenderContext& context) noexcept {
    if (!EnsureGpu(context)) return;
    UpdateCameraProjection(context.Width(), context.Height());
    UpdateCameraView();

    FDirLight lights[1];
    lights[0].direction = Normalize(FVec3{0.45f, 0.82f, -0.38f});
    lights[0].color = FVec3{1.55f, 1.48f, 1.36f};
    m_Shader.SetLights(
        m_Camera.ViewProjection(),
        m_Camera.Eye(),
        lights,
        1u,
        FVec3{0.055f, 0.075f, 0.11f});
    m_Shader.SetFog(
        FVec3{0.08f, 0.11f, 0.16f},
        0.0035f,
        0.12f,
        m_Target.y - m_Distance * 0.25f);

    struct FRenderEntry {
        const ANode* Node = nullptr;
        bool ParentVisible = true;
        bool ParentEnabled = true;
    };
    TArray<FRenderEntry> stack;
    if (!stack.TryPushBack(FRenderEntry{&m_Graph.Root(), true, true}))
        return;

    u32 draw_count = 0u;
    while (!stack.IsEmpty()) {
        const FRenderEntry entry = stack.Back();
        stack.PopBack();
        const ANode* node = entry.Node;
        if (node == nullptr) continue;
        const bool enabled = entry.ParentEnabled && node->IsEnabled();
        const bool visible = entry.ParentVisible && node->IsVisible();
        for (u32 index = node->ChildCount(); index > 0u; --index) {
            if (!stack.TryPushBack(
                    FRenderEntry{node->Child(index - 1u), visible, enabled})) {
                return;
            }
        }
        if (!enabled || !visible || draw_count >= 256u) continue;
        const AMeshComponent3D* component = FindMesh(*node);
        if (component == nullptr) continue;
        const FGpuMesh* gpu = GpuMeshFor(*component);
        if (gpu == nullptr || !gpu->vertex_buffer || !gpu->index_buffer)
            continue;

        FVec4 tint = component->Color();
        FVec3 base{tint.x, tint.y, tint.z};
        f32 metallic = 0.0f;
        f32 roughness = 0.5f;
        f32 ao = 1.0f;
        m_Shader.ClearSubstrateSurface();
        m_Shader.SetExtParams(0.0f, 0.1f, 0.0f);
        m_Shader.SetEmissive(FVec3{0.0f, 0.0f, 0.0f}, 0.0f);
        m_Shader.SetSheen(FVec3{1.0f, 1.0f, 1.0f}, 0.0f);
        m_Shader.SetSubsurface(FVec3{1.0f, 0.3f, 0.2f}, 0.0f);
        m_Shader.SetNormalMap(nullptr, 0.0f);

        if (component->MaterialLoaded()) {
            const FMaterial2D& material = component->Material();
            const FPbrParams2D& pbr = material.pbr;
            base = FVec3{
                pbr.baseColor.x * tint.x,
                pbr.baseColor.y * tint.y,
                pbr.baseColor.z * tint.z,
            };
            metallic = pbr.metallic;
            roughness = pbr.roughness;
            ao = pbr.ao;
            m_Shader.SetExtParams(
                pbr.clearcoat, pbr.clearcoatRoughness, pbr.anisotropy);
            m_Shader.SetEmissive(pbr.emissive, pbr.emissiveStrength);
            m_Shader.SetSheen(
                pbr.sheenColor, pbr.sheen, pbr.sheenRoughness);
            m_Shader.SetSubsurface(
                pbr.subsurfaceColor, pbr.subsurface);
            if (material.substrate.enabled)
                (void)m_Shader.SetSubstrateMaterial(material.substrate, m_Time);
        }
        m_Shader.DrawMesh(
            context.Cmd(),
            *gpu,
            node->World().ToMat4(),
            base,
            metallic,
            roughness,
            ao);
        ++draw_count;
    }
}

bool FLegacyScene3DAdapter::EnsureGpu(FRenderContext& context) noexcept {
    if (m_GpuReady) return true;
    if (m_GpuAttempted) return false;
    m_GpuAttempted = true;
    IRhiDevice* device = context.GetRenderer().Device();
    if (device == nullptr) return false;
    const auto shader = m_Shader.Init(
        *device,
        context.GetRenderer().ColorFormat(),
        context.GetRenderer().DepthFormat());
    if (shader.IsErr()) {
        ACS_LOG_ERROR(
            "LegacyScene3DAdapter: PBR initialization failed: %s",
            shader.Error().message);
        return false;
    }

    const TSharedPtr<FMeshAsset> cube = Primitive::MakeCube();
    const TSharedPtr<FMeshAsset> sphere =
        Primitive::MakeSphere(0.5f, 48u, 24u);
    const TSharedPtr<FMeshAsset> plane = Primitive::MakePlane();
    if (!cube || !sphere || !plane
        || UploadMesh(*device, *cube, m_Cube).IsErr()
        || UploadMesh(*device, *sphere, m_Sphere).IsErr()
        || UploadMesh(*device, *plane, m_Plane).IsErr()
        || !UploadGraphMeshes(*device)) {
        ACS_LOG_ERROR(
            "LegacyScene3DAdapter: GPU mesh initialization failed");
        ReleaseGpu();
        m_GpuAttempted = true;
        return false;
    }
    m_GpuReady = true;
    return true;
}

bool FLegacyScene3DAdapter::UploadGraphMeshes(IRhiDevice& device) noexcept {
    m_CustomMeshes.Clear();
    if (!m_CustomMeshes.TryReserve(m_Graph.NodeCount()))
        return false;
    TArray<ANode*> stack;
    if (!stack.TryPushBack(&m_Graph.Root())) return false;
    while (!stack.IsEmpty()) {
        ANode* node = stack.Back();
        stack.PopBack();
        if (node == nullptr) continue;
        AMeshComponent3D* component = FindMesh(*node);
        if (component != nullptr
            && component->Primitive() == EMeshPrimitive3D::Mesh) {
            FMeshAsset* mesh = component->Mesh();
            if (mesh == nullptr) return false;
            FCustomGpuMesh uploaded;
            uploaded.Component = component;
            if (UploadMesh(device, *mesh, uploaded.Mesh).IsErr()
                || !m_CustomMeshes.TryPushBack(Move(uploaded))) {
                return false;
            }
        }
        for (u32 index = 0u; index < node->ChildCount(); ++index)
            if (!stack.TryPushBack(node->Child(index))) return false;
    }
    return true;
}

void FLegacyScene3DAdapter::ReleaseGpu() noexcept {
    m_CustomMeshes.Clear();
    m_Cube = FGpuMesh{};
    m_Sphere = FGpuMesh{};
    m_Plane = FGpuMesh{};
    m_Shader.Shutdown();
    m_GpuReady = false;
    m_GpuAttempted = false;
}

void FLegacyScene3DAdapter::UpdateCameraProjection(
    u32 width,
    u32 height) noexcept {
    const f32 safe_width = width > 0u ? static_cast<f32>(width) : 1.0f;
    const f32 safe_height = height > 0u ? static_cast<f32>(height) : 1.0f;
    const f32 aspect = safe_width / safe_height;
    const f32 far_plane = m_Distance * 200.0f + 1000.0f;
    if (m_Projection == ESceneProjectionMode::Orthographic) {
        const f32 view_height = m_Distance * 1.25f;
        m_Camera.SetOrthographic(
            view_height * aspect, view_height, 0.01f, far_plane);
    } else {
        m_Camera.SetPerspective(
            55.0f * kDeg2Rad, aspect, 0.05f, far_plane);
    }
}

void FLegacyScene3DAdapter::UpdateCameraView() noexcept {
    const FVec3 forward{
        Sin(m_Yaw) * Cos(m_Pitch),
        -Sin(m_Pitch),
        Cos(m_Yaw) * Cos(m_Pitch),
    };
    m_Camera.SetLookAt(m_Target - forward * m_Distance, m_Target);
}

const FGpuMesh* FLegacyScene3DAdapter::GpuMeshFor(
    const AMeshComponent3D& component) const noexcept {
    switch (component.Primitive()) {
    case EMeshPrimitive3D::Cube: return &m_Cube;
    case EMeshPrimitive3D::Sphere: return &m_Sphere;
    case EMeshPrimitive3D::Plane: return &m_Plane;
    case EMeshPrimitive3D::Mesh:
        for (u32 index = 0u; index < m_CustomMeshes.Size(); ++index) {
            if (m_CustomMeshes[index].Component == &component)
                return &m_CustomMeshes[index].Mesh;
        }
        return nullptr;
    }
    return nullptr;
}

} // namespace acs::game
