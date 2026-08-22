// SPDX-License-Identifier: Apache-2.0
#include "gameframework/LegacyScene3DAdapter.h"
#include "gameframework/CameraComponent3D.h"

#include "asset/MeshAsset.h"
#include "asset/MeshPrimitive.h"
#include "foundation/Log.h"
#include "foundation/Move.h"
#include "gameframework/AssetPack.h"
#include "gameframework/Game.h"
#include "gameframework/MeshComponent3D.h"
#include "gameframework/RenderContext.h"
#include "gameframework/Sprite3DComponent.h"
#include "gameframework/WaterSurface3DComponent.h"
#include "math/Mat.h"
#include "math/Math.h"
#include "platform/Input.h"
#include "platform/InputCodes.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiDevice.h"
#include "render/IRhiSwapchain.h"
#include "render/IRhiTexture.h"
#include "render/Renderer.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace acs::game {

namespace {

// 太陽の既定値。シーンに ALightComponent3D が 1 灯も無いときに使う。
// **向きは PBR パス・水面・空で必ず同じものを使う。** 別々に書くと、物の陰り・水面の
// きらめき・空の太陽の位置がばらばらを向き、原因の分かりにくい違和感になる。
constexpr FVec3 kDefaultSunDirection{0.45f, 0.82f, -0.38f};
constexpr FVec3 kDefaultSunColor{1.55f, 1.48f, 1.36f};
constexpr FVec3 kDefaultSkySunColor{1.0f, 0.95f, 0.85f};
constexpr FVec3 kDefaultWaterSunColor{4.8f, 4.35f, 3.9f};
constexpr FVec3 kDefaultAmbient{0.055f, 0.075f, 0.11f};

// 水面は同じ太陽をより強く受ける。シーンの光を使うときはこの倍率を掛ける
// (既定値どうしの比がおよそこの値)。
constexpr f32 kWaterSunBoost = 3.1f;

// 影の解像度。上げると輪郭が締まるが、その 2 乗でメモリと描画費用が増える。
constexpr u32 kShadowMapSize = 2048u;

// 影の判定をずらす量。小さすぎると自分の影で縞が出て、大きすぎると影が浮く。
constexpr f32 kShadowDepthBias = 0.0025f;

// 環境光を焼き直す太陽の移動量 (2 乗)。小さすぎると毎フレーム焼いて重くなり、
// 大きすぎると夕暮れの色が付いてこない。約 1.7 度ぶん。
constexpr f32 kIblRebakeThresholdSquared = 0.0009f;

// 大気を焼く解像度。1024 の cubemap に対して 2:1 の元が要る (足りないと天頂に同心円が出る)。
constexpr u32 kAtmosphereEquirectWidth = 2048u;
constexpr u32 kAtmosphereEquirectHeight = 1024u;

// GPU で焼けないときの逃げ道。同期で回すので小さくする。
constexpr u32 kAtmosphereCpuEquirectWidth = 512u;
constexpr u32 kAtmosphereCpuEquirectHeight = 256u;

// 太陽の «設定した強さ» から大気の放射輝度への換算 (editor_abi と同じ)。
constexpr f32 kAtmosphereDefaultIntensity = 2.35f;
constexpr f32 kAtmosphereRadianceAtDefault = 22.0f;

// 太陽ディスク。実際の太陽の角半径 (0.2666 度) と、ディスクを空より強く出す倍率。
constexpr f32 kSunAngularRadius = 0.004653f;
constexpr f32 kSunDiscRadianceScale = 30.0f;

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

const ACameraComponent3D* FindCamera(const ANode& node) noexcept {
    const void* kind = ComponentKindOf<ACameraComponent3D>();
    for (u32 index = 0u; index < node.ComponentCount(); ++index) {
        const AComponent* component = node.ComponentAt(index);
        if (component != nullptr && component->Kind() == kind)
            return static_cast<const ACameraComponent3D*>(component);
    }
    return nullptr;
}

/** constノードから3Dスプライトを探す。 */
const ASprite3DComponent* FindSprite(const ANode& node) noexcept
{
    const void* kind = ComponentKindOf<ASprite3DComponent>();
    for (u32 index = 0u; index < node.ComponentCount(); ++index) {
        const AComponent* component = node.ComponentAt(index);
        if (component != nullptr && component->Kind() == kind)
            return static_cast<const ASprite3DComponent*>(component);
    }
    return nullptr;
}

const AWaterSurface3DComponent* FindWater(const ANode& node) noexcept {
    const void* kind = ComponentKindOf<AWaterSurface3DComponent>();
    for (u32 index = 0u; index < node.ComponentCount(); ++index) {
        const AComponent* component = node.ComponentAt(index);
        if (component != nullptr && component->Kind() == kind)
            return static_cast<const AWaterSurface3DComponent*>(component);
    }
    return nullptr;
}

AWaterSurface3DComponent* FindWater(ANode& node) noexcept {
    const void* kind = ComponentKindOf<AWaterSurface3DComponent>();
    for (u32 index = 0u; index < node.ComponentCount(); ++index) {
        AComponent* component = node.ComponentAt(index);
        if (component != nullptr && component->Kind() == kind)
            return static_cast<AWaterSurface3DComponent*>(component);
    }
    return nullptr;
}

// 境界は component 自身が持つようになった (AMeshComponent3D::LocalBounds)。
// ここに写しを置くと、片方だけ直したときに «影は付くのに掴めない» が起きる。
void LocalBounds(
    const AMeshComponent3D& component,
    FVec3& minimum,
    FVec3& maximum) noexcept {
    component.LocalBounds(minimum, maximum);
}

/** ノードで実際に表示するスプライト優先のローカル境界を返す。 */
bool TryLocalVisualBounds(const ANode& node, FVec3& minimum, FVec3& maximum) noexcept
{
    if (const ASprite3DComponent* sprite = FindSprite(node)) {
        sprite->LocalBounds(minimum, maximum);
        return true;
    }
    const AMeshComponent3D* component = FindMesh(node);
    if (component == nullptr) return false;
    LocalBounds(*component, minimum, maximum);
    return true;
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

bool Finite(FVec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y)
        && std::isfinite(value.z);
}

bool IsEffectivelyActive(const ANode& node) noexcept {
    const ANode* current = &node;
    u32 depth = 0u;
    while (current != nullptr) {
        if (current->IsPendingDestroy()
            || !current->IsEnabled() || !current->IsVisible()) {
            return false;
        }
        current = current->Parent();
        if (++depth > kNodeMaxTreeDepth) return false;
    }
    return true;
}

bool IsEffectivelyEnabled(const ANode& node) noexcept {
    const ANode* current = &node;
    u32 depth = 0u;
    while (current != nullptr) {
        if (current->IsPendingDestroy() || !current->IsEnabled()) return false;
        current = current->Parent();
        if (++depth > kNodeMaxTreeDepth) return false;
    }
    return true;
}

bool NormalizeCameraBasisVector(FVec3 value, FVec3& output) noexcept {
    const f32 length_squared = LengthSq(value);
    if (!std::isfinite(length_squared) || length_squared <= 1.0e-12f)
        return false;
    output = value * (1.0f / std::sqrt(length_squared));
    return Finite(output);
}

bool BuildLiveCameraState(
    const ANode& node,
    const ACameraComponent3D& component,
    FScene3DCameraState& state) noexcept {
    if (!IsEffectivelyEnabled(node)) return false;
    const FTransform3D world = node.World();
    FVec3 forward;
    FVec3 authored_up;
    FVec3 right;
    FVec3 up;
    if (!Finite(world.position)
        || !NormalizeCameraBasisVector(
            Rotate(world.rotation, FVec3{0.0f, 0.0f, 1.0f}), forward)
        || !NormalizeCameraBasisVector(
            Rotate(world.rotation, FVec3{0.0f, 1.0f, 0.0f}), authored_up)
        || !NormalizeCameraBasisVector(Cross(authored_up, forward), right)
        || !NormalizeCameraBasisVector(Cross(forward, right), up)) {
        return false;
    }
    state = FScene3DCameraState{};
    state.IsAuthored = true;
    state.IsActivePreferred = component.IsActivePreferred();
    state.NodeId = node.SerialId();
    state.Priority = component.Priority();
    state.Projection = component.Projection();
    state.FovYDegrees = component.FovYDegrees();
    state.OrthographicHeight = component.OrthographicHeight();
    state.NearPlane = component.NearPlane();
    state.FarPlane = component.FarPlane();
    state.Position = world.position;
    state.Forward = forward;
    state.Up = up;
    std::snprintf(
        state.StableId, sizeof(state.StableId), "%s",
        component.StableId());
    return true;
}

bool LiveCameraPrecedes(
    const FScene3DCameraState& left,
    const FScene3DCameraState& right) noexcept {
    if (left.IsActivePreferred != right.IsActivePreferred)
        return left.IsActivePreferred;
    if (left.Priority != right.Priority) return left.Priority > right.Priority;
    const int identity_order = std::strcmp(left.StableId, right.StableId);
    if (identity_order != 0) return identity_order < 0;
    return left.NodeId < right.NodeId;
}

void ResolveDeterministicCameraRecursive(
    const ANode& node,
    FScene3DCameraState& best,
    bool& found,
    u32 depth = 0u) noexcept {
    if (depth > kNodeMaxTreeDepth) return;
    if (const ACameraComponent3D* component = FindCamera(node)) {
        FScene3DCameraState candidate;
        if (BuildLiveCameraState(node, *component, candidate)
            && (!found || LiveCameraPrecedes(candidate, best))) {
            best = candidate;
            found = true;
        }
    }
    for (u32 index = 0u; index < node.ChildCount(); ++index) {
        const ANode* child = node.Child(index);
        if (child != nullptr)
            ResolveDeterministicCameraRecursive(*child, best, found, depth + 1u);
    }
}

u32 CountCamerasRecursive(const ANode& node, u32 depth = 0u) noexcept {
    if (depth > kNodeMaxTreeDepth) return 0u;
    u32 count = FindCamera(node) != nullptr ? 1u : 0u;
    for (u32 index = 0u; index < node.ChildCount(); ++index) {
        const ANode* child = node.Child(index);
        if (child != nullptr)
            count += CountCamerasRecursive(*child, depth + 1u);
    }
    return count;
}

const ANode* FindCameraByStableIdRecursive(
    const ANode& node, const char* stable_id, u32 depth = 0u) noexcept {
    if (depth > kNodeMaxTreeDepth) return nullptr;
    if (const ACameraComponent3D* component = FindCamera(node)) {
        if (std::strcmp(component->StableId(), stable_id) == 0) return &node;
    }
    for (u32 index = 0u; index < node.ChildCount(); ++index) {
        const ANode* child = node.Child(index);
        if (child == nullptr) continue;
        if (const ANode* found =
                FindCameraByStableIdRecursive(
                    *child, stable_id, depth + 1u)) {
            return found;
        }
    }
    return nullptr;
}

bool IsPlanarWaterMesh(const AMeshComponent3D& component) noexcept {
    if (component.Primitive() == EMeshPrimitive3D::Plane) return true;
    if (component.Primitive() != EMeshPrimitive3D::Mesh) return false;
    const AMeshAsset* mesh = component.Mesh();
    return mesh != nullptr
        && CWaterSurface3D::IsLocalXzSurfaceMesh(*mesh);
}

FRayHit3 RaycastMeshWorld(
    const ANode& node,
    const AMeshComponent3D& component,
    const FRay3& ray,
    f32 max_distance) noexcept {
    const FMat4 model = node.World().ToMat4();
    const FMat4 inverse_model = Inverse(model);
    const FRay3 local_ray{
        TransformPoint(ray.origin, inverse_model),
        TransformVector(ray.direction, inverse_model),
    };
    if (!Finite(local_ray.origin) || !Finite(local_ray.direction)
        || LengthSq(local_ray.direction) < 1.0e-12f) {
        return FRayHit3{};
    }
    FRayHit3 hit = component.RaycastLocalGeometry(local_ray, max_distance);
    if (!hit.hit) return hit;
    hit.point = ray.origin + ray.direction * hit.t;
    hit.normal = Normalize(TransformVector(
        hit.normal, Transpose(inverse_model)));
    if (!Finite(hit.normal) || LengthSq(hit.normal) < 1.0e-10f)
        hit.normal = FVec3{0.0f, 1.0f, 0.0f};
    if (Dot(hit.normal, ray.direction) > 0.0f)
        hit.normal = -hit.normal;
    return hit;
}

constexpr f32 kSsssEpsilon = 1.0e-4f;

bool SubstrateNeedsSubsurfaceMrt(
    const FMaterial2D& material) noexcept {
    if (!material.substrate.enabled) return false;

    FSubstrateResolvedSurface surface{};
    if (!ResolveMaterialSubstrate(material, surface)) return false;
    bool has_mfp =
        surface.mean_free_path_cm.x > kSsssEpsilon
        || surface.mean_free_path_cm.y > kSsssEpsilon
        || surface.mean_free_path_cm.z > kSsssEpsilon;
    bool has_thickness = surface.thickness_cm > kSsssEpsilon;

    // A literal may be zero while a per-pixel expression supplies the
    // physical profile. Compile only the lightweight link metadata here; GPU
    // shader/resource work remains demand-driven and asynchronous.
    const FSubstrateExpressionLinkResult links =
        CompileSubstrateExpressionLinks(material.substrate);
    if (links.Succeeded()) {
        for (u32 index = 0u; index < links.binding_count; ++index) {
            const u32 target =
                SubstrateExpressionBindingTarget(links.bindings[index]);
            if (target >= 16u && target <= 18u) has_mfp = true;
            if (target == 26u) has_thickness = true;
        }
    }
    return has_mfp && has_thickness;
}

struct FSceneRenderFeatures {
    bool has_water = false;
    bool needs_subsurface_mrt = false;
    bool has_sprites = false;
};

FSceneRenderFeatures ScanSceneRenderFeatures(
    const ANode& root) noexcept {
    FSceneRenderFeatures features{};
    TArray<const ANode*> stack;
    if (!stack.TryAdd(&root)) return features;
    while (!stack.IsEmpty()) {
        const ANode* node = stack.Last();
        stack.Pop();
        if (node == nullptr || node->IsPendingDestroy()) continue;
        // Visibility/enabled state is inherited, so an inactive node lets the
        // complete subtree be rejected before any child or material work.
        if (!IsEffectivelyActive(*node)) continue;
        for (u32 index = 0u; index < node->ChildCount(); ++index) {
            if (!stack.TryAdd(node->Child(index))) return features;
        }
        if (FindSprite(*node) != nullptr) {
            features.has_sprites = true;
            continue;
        }
        const AMeshComponent3D* mesh = FindMesh(*node);
        if (mesh == nullptr) continue;
        if (FindWater(*node) != nullptr
            && IsPlanarWaterMesh(*mesh)) {
            features.has_water = true;
        }
        if (!mesh->MaterialLoaded()) {
            if (features.has_water && features.has_sprites && features.needs_subsurface_mrt) {
                break;
            }
            continue;
        }
        const FMaterial2D& material = mesh->Material();
        if (material.kind != EMaterialKind::Lit
            || material.pbr.transmission > kSsssEpsilon) {
            continue;
        }
        if (material.pbr.subsurface > kSsssEpsilon
            || SubstrateNeedsSubsurfaceMrt(material)) {
            features.needs_subsurface_mrt = true;
        }
        if (features.has_water && features.has_sprites && features.needs_subsurface_mrt) break;
    }
    return features;
}

} // namespace

ALegacyScene3DAdapter::~ALegacyScene3DAdapter() noexcept {
    JoinCpuCompileWorkers();
}

FScene3DLoadResult ALegacyScene3DAdapter::LoadFile(const char* path) noexcept {
    if (m_GpuReady || m_GpuAttempted) DrainAndReleaseGpu();
    m_LoadResult = TryLoadScene3DFile(Graph(), path);
    if (m_LoadResult.Succeeded()) {
        AdoptLoadedCamera();
        if (!m_UseAuthoredCamera) FrameScene();
    }
    return m_LoadResult;
}

FScene3DLoadResult ALegacyScene3DAdapter::LoadText(
    const char* text, u32 size) noexcept {
    if (m_GpuReady || m_GpuAttempted) DrainAndReleaseGpu();
    m_LoadResult = TryLoadScene3DText(Graph(), text, size);
    if (m_LoadResult.Succeeded()) {
        AdoptLoadedCamera();
        if (!m_UseAuthoredCamera) FrameScene();
    }
    return m_LoadResult;
}

FScene3DLoadResult ALegacyScene3DAdapter::LoadAssetPack(
    IAssetPackReader& pack,
    const char* virtual_path) noexcept {
    if (m_GpuReady || m_GpuAttempted) DrainAndReleaseGpu();
    m_LoadResult = TryLoadScene3DAssetPack(Graph(), pack, virtual_path);
    if (m_LoadResult.Succeeded()) {
        AdoptLoadedCamera();
        if (!m_UseAuthoredCamera) FrameScene();
    }
    return m_LoadResult;
}

void ALegacyScene3DAdapter::AdoptLoadedCamera() noexcept {
    m_HasExplicitCameraOverride = false;
    // The loaded graph supersedes every authored-camera cache from the
    // previous scene. Starting from a clean state also ensures a no-camera
    // load reaches the caller's FrameScene fallback exactly once.
    m_UseAuthoredCamera = false;
    m_AuthoredCamera = FScene3DCameraState{};
    m_ActiveCameraNodeId =
        m_LoadResult.ActiveCamera.IsAuthored
            ? m_LoadResult.ActiveCamera.NodeId : -1;
    m_UseAuthoredCamera = RefreshAuthoredCameraPose();
}

bool ALegacyScene3DAdapter::RefreshAuthoredCameraPose() noexcept {
    if (m_HasExplicitCameraOverride && m_ActiveCameraNodeId >= 0) {
        const ANode* node =
            Graph().Root().FindBySerialId(m_ActiveCameraNodeId);
        const ACameraComponent3D* component =
            node != nullptr ? FindCamera(*node) : nullptr;
        FScene3DCameraState live;
        if (node != nullptr && component != nullptr
            && BuildLiveCameraState(*node, *component, live)) {
            m_AuthoredCamera = live;
            m_Projection =
                live.Projection == EScene3DCameraProjection::Orthographic
                    ? ESceneProjectionMode::Orthographic
                    : ESceneProjectionMode::Perspective;
            m_UseAuthoredCamera = true;
            return true;
        }
        // An explicit override is sticky only while it remains a valid,
        // effectively enabled camera. Once invalid, return to deterministic
        // automatic selection instead of retaining a stale pose.
        m_HasExplicitCameraOverride = false;
    }

    FScene3DCameraState deterministic;
    bool found = false;
    ResolveDeterministicCameraRecursive(
        Graph().Root(), deterministic, found);
    if (!found) {
        const bool lost_authored_camera = m_UseAuthoredCamera;
        m_UseAuthoredCamera = false;
        m_ActiveCameraNodeId = -1;
        m_AuthoredCamera = FScene3DCameraState{};
        m_Projection = ESceneProjectionMode::Perspective;
        if (lost_authored_camera) FrameScene();
        return false;
    }
    m_AuthoredCamera = deterministic;
    m_ActiveCameraNodeId = deterministic.NodeId;
    m_Projection =
        deterministic.Projection == EScene3DCameraProjection::Orthographic
            ? ESceneProjectionMode::Orthographic
            : ESceneProjectionMode::Perspective;
    m_UseAuthoredCamera = true;
    return true;
}

u32 ALegacyScene3DAdapter::CameraCount() const noexcept {
    return CountCamerasRecursive(Graph().Root());
}

bool ALegacyScene3DAdapter::SetActiveCamera(const char* stable_id) noexcept {
    if (stable_id == nullptr || stable_id[0] == '\0') return false;
    const ANode* node =
        FindCameraByStableIdRecursive(Graph().Root(), stable_id);
    const ACameraComponent3D* component =
        node != nullptr ? FindCamera(*node) : nullptr;
    FScene3DCameraState live;
    if (node == nullptr || component == nullptr
        || !BuildLiveCameraState(*node, *component, live)) {
        return false;
    }
    m_AuthoredCamera = live;
    m_ActiveCameraNodeId = live.NodeId;
    m_UseAuthoredCamera = true;
    m_HasExplicitCameraOverride = true;
    m_Projection =
        live.Projection == EScene3DCameraProjection::Orthographic
            ? ESceneProjectionMode::Orthographic
            : ESceneProjectionMode::Perspective;
    return true;
}

bool ALegacyScene3DAdapter::SetActiveCamera(i32 node_id) noexcept {
    const ANode* node = Graph().Root().FindBySerialId(node_id);
    const ACameraComponent3D* component =
        node != nullptr ? FindCamera(*node) : nullptr;
    FScene3DCameraState live;
    if (node == nullptr || component == nullptr
        || !BuildLiveCameraState(*node, *component, live)) {
        return false;
    }
    m_AuthoredCamera = live;
    m_ActiveCameraNodeId = live.NodeId;
    m_UseAuthoredCamera = true;
    m_HasExplicitCameraOverride = true;
    m_Projection =
        live.Projection == EScene3DCameraProjection::Orthographic
            ? ESceneProjectionMode::Orthographic
            : ESceneProjectionMode::Perspective;
    return true;
}

bool ALegacyScene3DAdapter::ClearActiveCameraOverride() noexcept {
    m_HasExplicitCameraOverride = false;
    return RefreshAuthoredCameraPose();
}

void ALegacyScene3DAdapter::FrameScene() noexcept {
    FVec3 minimum{FLT_MAX, FLT_MAX, FLT_MAX};
    FVec3 maximum{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    bool found = false;
    TArray<const ANode*> stack;
    if (!stack.TryAdd(&Graph().Root())) return;
    while (!stack.IsEmpty()) {
        const ANode* node = stack.Last();
        stack.Pop();
        if (node == nullptr) continue;
        FVec3 local_minimum, local_maximum;
        if (TryLocalVisualBounds(*node, local_minimum, local_maximum)) {
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
            if (!stack.TryAdd(node->Child(index))) return;
    }
    if (!found) {
        m_OrbitCameraState.target = FVec3{0.0f, 0.0f, 0.0f};
        m_OrbitCameraState.distance = 8.0f;
    } else {
        m_OrbitCameraState.target = (minimum + maximum) * 0.5f;
        const f32 radius = Length((maximum - minimum) * 0.5f);
        m_OrbitCameraState.distance = radius > 0.25f ? radius * 2.8f : 3.0f;
        if (m_OrbitCameraState.distance < 3.0f) m_OrbitCameraState.distance = 3.0f;
        if (m_OrbitCameraState.distance > 10000.0f) m_OrbitCameraState.distance = 10000.0f;
    }
    m_PreviousOrbitCameraState = m_OrbitCameraState;
    m_PresentedOrbitCameraState = m_OrbitCameraState;
    m_IsOrbitCameraObstructionPresentationActive = false;
    UpdateCameraView();
}

bool ALegacyScene3DAdapter::RaycastWater(
    const FRay3& ray,
    FWaterRaycastHit& out_hit,
    f32 max_distance) const noexcept {
    out_hit = FWaterRaycastHit{};
    if (!Finite(ray.origin) || !Finite(ray.direction)
        || LengthSq(ray.direction) < 1.0e-12f
        || !std::isfinite(max_distance) || max_distance <= 0.0f) {
        return false;
    }

    struct FEntry {
        const ANode* Node = nullptr;
        bool ParentVisible = true;
        bool ParentEnabled = true;
    };
    TArray<FEntry> stack;
    if (!stack.TryAdd(FEntry{&Graph().Root(), true, true}))
        return false;

    f32 best_distance = max_distance;
    FNodeId best_node{};
    FVec3 best_point{};
    FVec3 best_normal{0.0f, 1.0f, 0.0f};
    bool best_is_water = false;
    bool have_hit = false;

    while (!stack.IsEmpty()) {
        const FEntry entry = stack.Last();
        stack.Pop();
        const ANode* node = entry.Node;
        if (node == nullptr || node->IsPendingDestroy()) continue;
        const bool enabled = entry.ParentEnabled && node->IsEnabled();
        const bool visible = entry.ParentVisible && node->IsVisible();
        for (u32 index = node->ChildCount(); index > 0u; --index) {
            if (!stack.TryAdd(FEntry{
                    node->Child(index - 1u), visible, enabled})) {
                return false;
            }
        }
        if (!enabled || !visible) continue;

        const AMeshComponent3D* mesh = FindSprite(*node) == nullptr
            ? FindMesh(*node) : nullptr;
        if (mesh == nullptr) continue;
        const FRayHit3 hit =
            RaycastMeshWorld(*node, *mesh, ray, best_distance);
        if (!hit.hit || hit.t < 0.0f) continue;
        const bool water =
            FindWater(*node) != nullptr && IsPlanarWaterMesh(*mesh);
        constexpr f32 kTieEpsilon = 1.0e-5f;
        const bool closer =
            !have_hit || hit.t < best_distance - kTieEpsilon;
        const bool opaque_wins_tie =
            have_hit && !water && best_is_water
            && Abs(hit.t - best_distance) <= kTieEpsilon;
        if (!closer && !opaque_wins_tie) continue;
        have_hit = true;
        best_distance = hit.t;
        best_node = node->Id();
        best_point = hit.point;
        best_normal = hit.normal;
        best_is_water = water;
    }

    if (!have_hit || !best_is_water || !best_node.IsValid()) return false;
    out_hit.Node = best_node;
    out_hit.Point = best_point;
    out_hit.Normal = best_normal;
    out_hit.Distance = best_distance;
    return true;
}

bool ALegacyScene3DAdapter::AddWaterDisturbance(
    FNodeId surface,
    FVec3 world_point,
    f32 radius,
    f32 strength) noexcept {
    ANode* node = Graph().Get(surface);
    if (node == nullptr || !IsEffectivelyActive(*node)) {
        return false;
    }
    AMeshComponent3D* mesh = FindMesh(*node);
    AWaterSurface3DComponent* water = FindWater(*node);
    if (mesh == nullptr || water == nullptr || !IsPlanarWaterMesh(*mesh))
        return false;
    m_Water.SetParams(water->ToRenderParams());
    return m_Water.AddDisturbanceForSurface(
        static_cast<u64>(surface.m_Packed),
        world_point, radius, strength);
}

bool ALegacyScene3DAdapter::AddWaterWake(
    FNodeId surface,
    FVec3 world_point,
    FVec3 world_velocity,
    f32 radius,
    f32 strength) noexcept {
    ANode* node = Graph().Get(surface);
    if (node == nullptr || !IsEffectivelyActive(*node)) {
        return false;
    }
    AMeshComponent3D* mesh = FindMesh(*node);
    AWaterSurface3DComponent* water = FindWater(*node);
    if (mesh == nullptr || water == nullptr || !IsPlanarWaterMesh(*mesh))
        return false;
    m_Water.SetParams(water->ToRenderParams());
    return m_Water.AddWakeForSurface(
        static_cast<u64>(surface.m_Packed),
        world_point, world_velocity, radius, strength);
}

void ALegacyScene3DAdapter::OnEnter() noexcept {
    /** 自由cameraの既定キーを所有するscene-local入力map。 */
    FInputMap& input_map = Services().Input();
    input_map.BindAxisKeys(m_OrbitCameraActions.move_forward_action, EKey::S, EKey::W);
    input_map.BindAxisKeys(m_OrbitCameraActions.move_right_action, EKey::A, EKey::D);
    input_map.BindAxisKeys(m_OrbitCameraActions.move_up_action, EKey::Q, EKey::E);
    input_map.BindAxisKeys(m_OrbitCameraActions.look_yaw_action, EKey::Left, EKey::Right);
    input_map.BindAxisKeys(m_OrbitCameraActions.look_pitch_action, EKey::Down, EKey::Up);
    input_map.BindAxisKeys(m_OrbitCameraActions.zoom_action, EKey::PageDown, EKey::PageUp);
    GetGame().SetClearColor(0.025f, 0.035f, 0.055f, 1.0f);
    // Keep lighting, visible sky, fog and water reflection on one authored
    // environment. 太陽の向きと色は毎フレーム UpdateSkyFromSun() が上書きするので、
    // ここで設定するのは «光が 1 灯も無いときの見え方» の初期値にあたる。
    // 雲は既定で切ってある。必要な場面だけ Sky().SetFallbackCloudsEnabled(true) で入れる
    // (本格的なボリューメトリック雲は別機能)。
    m_Sky.PresetDay();
    m_Sky.SetSunDirection(Normalize(kDefaultSunDirection));
    m_Sky.SetSunColor(kDefaultSkySunColor);
    m_Sky.SetZenithColor(FVec3{0.16f, 0.33f, 0.62f});
    m_Sky.SetHorizonColor(FVec3{0.62f, 0.70f, 0.80f});
    m_Sky.SetGroundColor(FVec3{0.20f, 0.19f, 0.21f});
    m_Sky.SetFallbackCloudsEnabled(false);
    m_PostParams.bloom_threshold = 1.1f;
    m_PostParams.bloom_intensity = 0.32f;
    m_PostParams.bloom_radius = 0.82f;
    m_PostParams.bloom_scatter = 0.64f;
    // 環境光は物理ベースの大気から来るので、明るさは «太陽の強さ» と «空の状態» で
    // 大きく動く。固定の露出だと、太陽を強くしただけで白く飛び、夕暮れでは潰れる。
    // 自動露出にして、シーンの明るさへ合わせる (手で調整させない)。
    m_PostParams.exposure = 0.92f;            // 自動露出への手動補正 (EV) として働く
    m_PostParams.auto_exposure_enabled = true;
    m_PostParams.tonemap_kind = 0;
    m_PostParams.vignette_intensity = 0.075f;
    m_PostParams.vignette_radius = 0.84f;
    m_PostParams.chromatic_aberration = 0.0f;
    m_PostParams.grain_intensity = 0.002f;
    m_PostParams.cas_strength = 0.16f;
    m_PostParams.taa_enabled = false;
    // Load already adopted the authored selection. Re-resolve here instead of
    // keying off the last load result so a failed transactional hot reload
    // cannot eject the still-published scene from its valid camera.
    if (!RefreshAuthoredCameraPose()) FrameScene();
}

void ALegacyScene3DAdapter::OnExit() noexcept {
    DrainAndReleaseGpu();
}

void ALegacyScene3DAdapter::OnUpdate(f32 dt) noexcept {
    m_Time += dt;
    // Interaction lifetime is simulation state. It advances even while the
    // renderer is hidden, still compiling, resized, or using opaque fallback.
    m_Water.Update(dt);
    m_PostParams.delta_time =
        std::isfinite(dt) && dt > 0.0f ? dt : 0.0f;
    m_PostParams.grain_time += m_PostParams.delta_time;
    // graph の tick は基底 AScene::Update_Internal が OnUpdate 後に必ず実行する。
    // ここで手動 tick すると二重更新になる。
    RefreshAuthoredCameraPose();
    // 自由カメラは «編集中に見回す» ためのもの。入れたままだと矢印キーと Escape を
    // ゲームから奪う。SetFreeCameraEnabled(false) で切れる。
    if (m_FreeCameraEnabled && CInput::IsKeyPressed(EKey::Escape)) {
        GetGame().Quit();
        return;
    }

    UpdatePresentedCameraView_Internal(m_PostParams.delta_time);
}

/** scene入力を6軸へ変換し、自由camera状態を固定刻みで進める。 */
void ALegacyScene3DAdapter::OnFixedUpdate(f32 fixed_dt) noexcept
{
    if (!m_FreeCameraEnabled || m_UseAuthoredCamera) return;
    /** 現在tickの6 actionから生成する正規化camera入力。 */
    COrbitCameraController3D::FOrbitCameraInput3D input{};
    if (!m_OrbitCameraActions.TryEvaluate(Services().Input(), Services().FixedInput(), input)) return;
    /** 失敗時にprevious/currentを維持する次状態候補。 */
    COrbitCameraController3D::FOrbitCameraState3D candidate = m_OrbitCameraState;
    if (!m_OrbitCameraController.TryStep(input, fixed_dt, candidate)) return;
    m_PreviousOrbitCameraState = m_OrbitCameraState;
    m_OrbitCameraState = candidate;
    UpdateCameraView();
}

void ALegacyScene3DAdapter::OnRender(FRenderContext& context) noexcept {
    RefreshAuthoredCameraPose();
    if (!EnsureGpu(context)) return;
    UpdatePresentedCameraView_Internal(0.0f);
    UpdateCameraProjection(context.Width(), context.Height());
    CollectSceneLights();
    UpdateSkyFromSun();

    CRenderer& renderer = context.GetRenderer();
    IRhiDevice* device = renderer.Device();
    IRhiSwapchain* swapchain = renderer.Swapchain();
    IRhiTexture* depth = renderer.DepthBuffer();
    if (device == nullptr || swapchain == nullptr) return;

    // 空を環境光として焼く。太陽が動いたときだけ焼き直す。
    (void)EnsureEnvironmentLighting(*device, context.Cmd());

    // 骨で動くメッシュを CPU で変形し、普通の頂点バッファへ入れ直す。**影も遮蔽も
    // この後のパスが読むので、必ず一番先に。** 遅らせると、影だけ前フレームの姿勢になる。
    (void)UpdateSkinnedMeshes(*device);

    // 太陽から見た深度を先に描く。PBR パスがこれを参照して影を落とす。
    if (EnsureShadowMap(*device)) (void)RenderShadowPass(context);
    else m_ShadowDrawn = false;

    // 法線と深度を先に描いて遮蔽を出す。PBR パスがこれを読むので、必ずその «前» に。
    (void)RenderAmbientOcclusionPass(*device, context);

    // 雲を計算する。描画パスの外で回す (結果は雲自身のテクスチャへ書かれる)。
    RenderClouds(*device, context.Cmd(), context.Width(), context.Height());

    EGpuCommitSubsystem frame_commit = EGpuCommitSubsystem::None;
    const bool hdr_ready = EnsureHdrFrameResources(
        *device, context.Width(), context.Height(),
        renderer.ColorFormat(), renderer.DepthFormat(),
        frame_commit);
    IRhiTexture* hdr = m_Post.HdrRenderTarget();
    if (!hdr_ready || hdr == nullptr
        || hdr->Width() != context.Width()
        || hdr->Height() != context.Height()) {
        return;
    }

    FWaterDraw water_draws[CWaterSurface3D::kMaxTrackedSurfaces]{};
    u32 water_count = CollectWaterDraws(
        water_draws, depth, context.Width(), context.Height());

    IRhiCommandList& command_list = context.Cmd();
    const bool ssss_resources_ready =
        m_SsssRequested
        && m_HdrSsssGpuState == EShaderGpuState::Ready
        && m_SsssGpuState == EShaderGpuState::Ready
        && m_BlitGpuState == EShaderGpuState::Ready
        && ActiveHdrShader().HasSubsurfaceMrtPipeline()
        && depth != nullptr
        && m_SsssDiffuse && m_SsssMaterial && m_SsssNormal
        && m_Ssss.Width() == context.Width()
        && m_Ssss.Height() == context.Height()
        && m_SsssDiffuse->Width() == context.Width()
        && m_SsssDiffuse->Height() == context.Height()
        && m_SsssMaterial->Width() == context.Width()
        && m_SsssMaterial->Height() == context.Height()
        && m_SsssNormal->Width() == context.Width()
        && m_SsssNormal->Height() == context.Height();
    const u64 pbr_scene_upper =
        static_cast<u64>(Graph().NodeCount());
    const u64 pbr_base_required_wide =
        pbr_scene_upper + static_cast<u64>(water_count);
    const u64 pbr_full_required_wide =
        pbr_scene_upper * (ssss_resources_ready ? 2u : 1u) +
        static_cast<u64>(water_count);
    const u32 pbr_base_required =
        pbr_base_required_wide > static_cast<u64>(~u32{0})
            ? ~u32{0} : static_cast<u32>(pbr_base_required_wide);
    const u32 pbr_full_required =
        pbr_full_required_wide > static_cast<u64>(~u32{0})
            ? ~u32{0} : static_cast<u32>(pbr_full_required_wide);
    // Reset exactly once for the complete command-list frame. A successful
    // SSSS MRT can be followed by a full single-target rebuild, and water can
    // still consume the same pool during its depth-copy fail-open pass.
    const bool pbr_full_pool_ready =
        ActiveHdrShader().BeginFrame(pbr_full_required);
    // Growth is incremental. If the two-pass SSSS reserve stops after enough
    // buffers for the complete base frame, disable SSSS but keep rendering.
    // Below the base bound, fail before opening RT0 rather than publishing a
    // scene with an arbitrary mesh suffix missing.
    const bool pbr_base_pool_ready =
        pbr_full_pool_ready ||
        ActiveHdrShader().ObjectBufferCapacity() >= pbr_base_required;
    if (!pbr_base_pool_ready) return;
    const bool ssss_frame_ready =
        ssss_resources_ready && pbr_full_pool_ready;

    command_list.BeginRenderToTexture(
        *hdr, FClearColor{0.025f, 0.035f, 0.055f, 1.0f},
        depth, 1.0f);
    RenderSky(*device, command_list, hdr->PixelFormat(), renderer.DepthFormat());
    if (!ssss_frame_ready) {
        (void)DrawPbrScene(
            context, ActiveHdrShader(),
            water_count > 0u ? water_draws : nullptr,
            water_count);
        command_list.EndRenderToTexture(*hdr);
    } else {
        command_list.EndRenderToTexture(*hdr);
        IRhiTexture* ssss_targets[4] = {
            hdr, m_SsssDiffuse.Get(), m_SsssMaterial.Get(),
            m_SsssNormal.Get(),
        };
        const bool mrt_bound =
            command_list.BeginRenderToTextureMrtLoad(
            ssss_targets, 4u, FClearColor{0, 0, 0, 0},
            (1u << 1u) | (1u << 2u) | (1u << 3u),
            depth, false, 1.0f);
        bool mrt_draws_valid = false;
        if (mrt_bound) {
            FViewport viewport{};
            viewport.width = static_cast<f32>(context.Width());
            viewport.height = static_cast<f32>(context.Height());
            command_list.SetViewport(viewport);
            FScissorRect scissor{};
            scissor.right = static_cast<i32>(context.Width());
            scissor.bottom = static_cast<i32>(context.Height());
            command_list.SetScissor(scissor);
            mrt_draws_valid = DrawPbrScene(
                context, ActiveHdrShader(),
                water_count > 0u ? water_draws : nullptr,
                water_count, true);
            command_list.EndRenderToTextureMrt(ssss_targets, 4u);
        }

        FSubsurfaceScatteringParams ssss_params{};
        ssss_params.radius_world = 0.012f;
        ssss_params.channel_radius = FVec3{1.0f, 0.55f, 0.25f};
        ssss_params.strength = 1.0f;
        ssss_params.depth_sigma = 0.001f;
        ssss_params.normal_power = 24.0f;
        ssss_params.max_radius_pixels = 64.0f;
        const bool ssss_rendered =
            mrt_bound && mrt_draws_valid
            && m_Ssss.Render(
                command_list, *hdr, *m_SsssDiffuse, *depth,
                *m_SsssNormal, *m_SsssMaterial,
                Inverse(m_Camera.ViewProjection()), ssss_params);
        if (ssss_rendered && m_Ssss.OutputTexture() != nullptr) {
            m_Blit.Copy(
                command_list, *m_Ssss.OutputTexture(), *hdr);
        } else {
            // The MRT PS deliberately disables analytic SSS. Rebuild the
            // complete frame through the established one-target shader when
            // any bounded draw or composite cannot be issued; never publish a
            // partially populated HDR scene.
            command_list.BeginRenderToTexture(
                *hdr, FClearColor{0.025f, 0.035f, 0.055f, 1.0f},
                depth, 1.0f);
            RenderSky(*device, command_list, hdr->PixelFormat(), renderer.DepthFormat());
            (void)DrawPbrScene(
                context, ActiveHdrShader(),
                water_count > 0u ? water_draws : nullptr,
                water_count);
            command_list.EndRenderToTexture(*hdr);
        }
    }

    if (water_count > 0u && m_WaterBackground.Get() != nullptr
        && m_WaterDepthSnapshot.Get() != nullptr && depth != nullptr) {
        m_Blit.Copy(command_list, *hdr, *m_WaterBackground);
        const bool copied_depth = command_list.CopyDepthTexture(
            *depth, *m_WaterDepthSnapshot);
        command_list.BeginRenderToTextureLoad(*hdr, depth);
        if (copied_depth) {
            DrawWaterScene(
                context, water_draws, water_count,
                *m_WaterBackground, *m_WaterDepthSnapshot);
        } else {
            // These nodes were intentionally omitted from the opaque pass.
            // A backend copy failure must never make authored water disappear.
            DrawWaterFallback(context, water_draws, water_count);
        }
        command_list.EndRenderToTexture(*hdr);
        if (!copied_depth) {
            m_DepthSnapshotFailed = true;
            m_WaterDepthSnapshot.Reset();
            ACS_LOG_WARN(
                "LegacyScene3DAdapter: interactive-water depth copy failed; "
                "opaque PBR fallback remains active");
        }
    }

    // 雲を最後に乗せる。手前の物で隠れるよう、完成したシーンの深度を使う。
    if (depth != nullptr) CompositeClouds(command_list, *hdr, *depth, context.Width(), context.Height());

    // SPR3Dは不透明物と水面を描いた後にalpha合成する。深度は読むだけで、
    // editorと同じく書き換えない。
    if (depth != nullptr && m_SpriteGpuState == EShaderGpuState::Ready && !m_CustomSprites.IsEmpty()) {
        command_list.BeginRenderToTextureLoad(*hdr, depth);
        (void)DrawSpriteScene(context);
        command_list.EndRenderToTexture(*hdr);
    }

    // 反射を作る。**空と雲まで乗った完成後**に作るので、水面や磨いた床に空も映る。
    // 使うのは次のフレームの PBR パス。
    if (depth != nullptr) {
        (void)RenderReflectionPass(*device, context, *hdr, *depth);
    } else {
        m_SsrValid = false;
    }
    m_PrevViewProjection = m_Camera.ViewProjection();
    m_HasPrevViewProjection = true;

    m_PostParams.taa_depth_texture = nullptr;
    m_Post.Render(
        command_list, *swapchain, renderer.CurrentBuffer(), m_PostParams);
}

void ALegacyScene3DAdapter::CollectSceneLights() noexcept {
    // 視点を渡すのは、点光源が上限を越えたときに近いものを残させるため。
    m_Lights.CollectFrom(Root(), m_Camera.Eye());
    if (m_Lights.DroppedCount() != 0u) {
        // 黙って消えると «置いたのに光らない» の原因が分からなくなる。
        ACS_LOG_WARN("Scene3D: %u lights exceeded the shader limit and were dropped",
                     m_Lights.DroppedCount());
    }
}

bool ALegacyScene3DAdapter::ComputeSceneBounds(FVec3& out_center, f32& out_radius) const noexcept {
    FVec3 minimum{FLT_MAX, FLT_MAX, FLT_MAX};
    FVec3 maximum{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    bool found = false;

    TArray<const ANode*> stack;
    if (!stack.TryAdd(&Graph().Root())) return false;
    while (!stack.IsEmpty()) {
        const ANode* node = stack.Last();
        stack.Pop();
        if (node == nullptr) continue;
        FVec3 local_minimum, local_maximum;
        if (TryLocalVisualBounds(*node, local_minimum, local_maximum)) {
            const FMat4 world = node->World().ToMat4();
            for (u32 corner = 0u; corner < 8u; ++corner) {
                const FVec3 local{
                    (corner & 1u) != 0u ? local_maximum.x : local_minimum.x,
                    (corner & 2u) != 0u ? local_maximum.y : local_minimum.y,
                    (corner & 4u) != 0u ? local_maximum.z : local_minimum.z,
                };
                ExpandBounds(TransformPoint(local, world), minimum, maximum);
            }
            found = true;
        }
        for (u32 index = 0u; index < node->ChildCount(); ++index)
            if (!stack.TryAdd(node->Child(index))) return false;
    }
    if (!found) return false;

    out_center = (minimum + maximum) * 0.5f;
    // 少し余裕を足す。ぴったりだと端のメッシュの影が投影範囲から外れて切れる。
    out_radius = Length((maximum - minimum) * 0.5f) + 1.0f;
    return true;
}

FVec3 ALegacyScene3DAdapter::SunColorForAtmosphere() const noexcept {
    if (m_Lights.DirectionalCount() == 0u) return kDefaultSunColor;

    return m_Lights.DirectionalLights()[0].color;
}

FVec3 ALegacyScene3DAdapter::PhysicalSunIntensity(FVec3 sun_color) noexcept {
    // 光の色には強さが掛かっている。一番大きい成分を «設定した強さ» とみなし、
    // 残りを色味として扱う。editor_abi と同じ換算 (既定 2.35 のとき 22.0)。
    f32 peak = sun_color.x > sun_color.y ? sun_color.x : sun_color.y;
    if (sun_color.z > peak) peak = sun_color.z;
    if (peak <= 0.0f) return FVec3{0.0f, 0.0f, 0.0f};

    const f32 radiance =
        kAtmosphereRadianceAtDefault * (peak / kAtmosphereDefaultIntensity);
    const f32 scale = radiance / peak;

    return FVec3{sun_color.x * scale, sun_color.y * scale, sun_color.z * scale};
}

void ALegacyScene3DAdapter::RenderClouds(
    IRhiDevice& device, IRhiCommandList& command_list, u32 width, u32 height) noexcept {
    m_CloudsDrawn = false;
    if (m_CloudParams.Coverage <= 0.001f) return;   // 出さない設定。
    if (width == 0u || height == 0u) return;

    if (!m_CloudsReady) {
        if (m_Clouds.Init(device, EFormat::R16G16B16A16_Float).IsErr()) {
            ACS_LOG_WARN("Scene3D: volumetric cloud init failed; clouds stay off");
            // 二度と試さない。毎フレーム失敗し続けても意味がない。
            m_CloudParams.Coverage = 0.0f;
            return;
        }
        m_CloudsReady = true;
    }

    // 参照描画の切り替えは内部の解像度が変わるので、作り直しが要る。
    m_Clouds.SetReferenceMode(m_CloudParams.bReferenceMode);

    if (width != m_CloudsWidth || height != m_CloudsHeight
        || m_CloudParams.bReferenceMode != m_CloudsSizedForReference) {
        if (!m_Clouds.EnsureSize(device, width, height, m_CloudParams.RenderScale,
                                 m_CloudParams.bReferenceMode)) {
            ACS_LOG_WARN("Scene3D: volumetric cloud sizing failed");
            return;
        }
        m_CloudsWidth = width;
        m_CloudsHeight = height;
        m_CloudsSizedForReference = m_CloudParams.bReferenceMode;

        if (m_CloudParams.bReferenceMode) {
            ACS_LOG_WARN("Scene3D: clouds are in reference mode (full resolution, no temporal); "
                         "this is for comparison only and is very slow");
        }
    }

    m_Clouds.SetLayer(FVolumetricCloudLayer{
        m_CloudParams.BaseAltitude, m_CloudParams.TopAltitude, m_CloudParams.NoiseScale});

    // 太陽光が雲へ届くまでに大気で失う分を掛ける。これが無いと夕方でも雲が昼の白さのまま。
    // 雲の層は薄いので、真ん中の高さで代表させる。
    FVolumetricCloudLighting lighting = m_CloudParams.Lighting;
    const f32 midAltitude =
        (m_CloudParams.BaseAltitude + m_CloudParams.TopAltitude) * 0.5f;
    lighting.SunTransmittance = SunTransmittanceAtAltitude(midAltitude, SunDirection());

    // 雲頂は天頂の空を、雲底は地平の空を受ける。空の色は m_Sky が持っている値を使う
    // (見えている空は大気から焼いたものだが、上下の «色の傾き» はこちらで足りる)。
    lighting.SkyZenithColor = m_Sky.ZenithColor();
    m_Clouds.SetLighting(lighting);
    m_Clouds.SetWeather(m_CloudParams.Weather);
    m_Clouds.SetRange(m_CloudParams.Range);
    m_Clouds.SetUpperLayer(FVolumetricCloudUpperLayer{
        m_CloudParams.UpperLayer.BaseAltitude,
        m_CloudParams.UpperLayer.TopAltitude,
        m_CloudParams.UpperLayer.CoverageScale,
        m_CloudParams.UpperLayer.DensityScale});

    // 雲を照らすのは物を照らすのと同じ太陽。ここを別にすると、雲だけ違う方向から
    // 光っているように見える。
    const FVec3 sun_color = SunColorForAtmosphere();
    // 通常の C++ 描画でも、遠方座標に左右されない視線復元行列を渡す。
    const FMat4 cloud_camera_relative_inverse_view_projection = BuildCameraRelativeInverseViewProjection(m_Camera.View(), m_Camera.Projection());

    m_Clouds.RenderComputeCameraRelative(command_list, cloud_camera_relative_inverse_view_projection, m_Camera.Eye(), SunDirection(), sun_color, m_Sky.HorizonColor(), m_CloudParams.Coverage, m_CloudParams.Density, m_CloudParams.Wind, m_Time);

    m_CloudsDrawn = true;
}

void ALegacyScene3DAdapter::CompositeClouds(
    IRhiCommandList& command_list, IRhiTexture& target,
    IRhiTexture& scene_depth, u32 width, u32 height) noexcept {
    if (!m_CloudsDrawn) return;

    // 完成したシーンの深度を渡すので、手前にある物が雲を隠す。
    command_list.BeginRenderToTextureLoad(target, nullptr);
    m_Clouds.Composite(command_list, scene_depth, width, height);
    command_list.EndRenderToTexture(target);
}

void ALegacyScene3DAdapter::RenderSky(
    IRhiDevice& device, IRhiCommandList& command_list,
    EFormat color_format, EFormat depth_format) noexcept {
    if (m_IblReady && m_Ibl.EnvCubemap() != nullptr) {
        // 環境光と同じ cubemap を空として描く。光と空が同じものから来る。
        const FVec3 radiance = PhysicalSunIntensity(SunColorForAtmosphere());
        // 最終解像度で描く太陽円盤の放射輝度。
        const FVec3 sunDiscRadiance{radiance.x * kSunDiscRadianceScale, radiance.y * kSunDiscRadianceScale, radiance.z * kSunDiscRadianceScale};
        // 通常のC++描画でもワールド原点からの距離に依存しない視線を使う。
        const FMat4 skyCameraRelativeInverse = BuildCameraRelativeInverseViewProjection(m_Camera.View(), m_Camera.Projection());
        m_Ibl.DrawEnvSkyboxCameraRelative(device, command_list, skyCameraRelativeInverse, color_format, depth_format, SunDirection(), sunDiscRadiance, kSunAngularRadius);
        return;
    }

    // 環境光をまだ焼けていないとき用。解析的な空で埋める。
    if (m_SkyGpuState == ESkyGpuState::Ready) {
        m_Sky.SetFallbackCloudTime(m_Time);
        m_Sky.Render(command_list, m_Camera);
    }
}

bool ALegacyScene3DAdapter::EnsureEnvironmentLighting(
    IRhiDevice& device, IRhiCommandList& command_list) noexcept {
    const FVec3 sun = SunDirection();

    // 焼き直しは重い。太陽がほとんど動いていないなら前回のものをそのまま使う。
    if (m_IblReady) {
        const f32 dx = sun.x - m_IblBakedSunDirection.x;
        const f32 dy = sun.y - m_IblBakedSunDirection.y;
        const f32 dz = sun.z - m_IblBakedSunDirection.z;
        if (dx * dx + dy * dy + dz * dz < kIblRebakeThresholdSquared) return true;
    }

    // BRDF LUT は太陽に依存しないので一度だけ。
    if (!m_Ibl.HasBrdfLut() && m_Ibl.EnsureBrdfLut(device, command_list).IsErr()) {
        ACS_LOG_WARN("Scene3D: BRDF LUT bake failed; environment lighting stays off");
        return false;
    }

    // 物理ベースの大気から焼く。太陽の高さで空の色が変わるので、夕暮れの赤みが
    // 何も設定しなくても環境光に乗る。だめなら見えている空 (CSky) から焼く。
    if (!m_AtmosphereTried) {
        m_AtmosphereTried = true;
        (void)m_Atmosphere.Init(device);
    }

    // 太陽だけは毎回いまの光から作る。残り (地面の色など) は場面が決めたものを使う。
    FAtmosphereParams atmosphere = m_AtmosphereParams;
    atmosphere.sun_dir = sun;
    atmosphere.sun_intensity = PhysicalSunIntensity(SunColorForAtmosphere());

    TArray<f32> equirect;
    u32 width = kAtmosphereEquirectWidth;
    u32 height = kAtmosphereEquirectHeight;

    bool baked = m_Atmosphere.Ready()
        && m_Atmosphere.BakeEquirect(device, command_list, atmosphere, width, height, equirect);

    if (!baked) {
        // GPU で焼けない環境向けの逃げ道。同期で回すので解像度を落とす。
        width = kAtmosphereCpuEquirectWidth;
        height = kAtmosphereCpuEquirectHeight;
        equirect = CAtmosphere::BakeEquirect(width, height, atmosphere);
        baked = equirect.Num() != 0u;
    }

    const bool loaded = baked
        && m_Ibl.LoadEquirectHdrFromMemory(
               device, command_list, equirect.GetData(), width, height).IsOk();

    if (!loaded && m_Ibl.EnsureEnvCubemap(device, command_list, m_Sky).IsErr()) {
        ACS_LOG_WARN("Scene3D: environment bake failed (atmosphere and sky both)");
        return false;
    }
    if (m_Ibl.EnsureIrradiance(device, command_list).IsErr()) {
        ACS_LOG_WARN("Scene3D: irradiance bake failed");
        return false;
    }
    if (m_Ibl.EnsurePrefilter(device, command_list).IsErr()) {
        ACS_LOG_WARN("Scene3D: prefilter bake failed");
        return false;
    }

    m_IblBakedSunDirection = sun;
    m_IblReady = true;
    return true;
}

bool ALegacyScene3DAdapter::EnsureShadowMap(IRhiDevice& device) noexcept {
    u32 wanted = m_ShadowParams.CascadeCount;
    if (wanted == 0u) wanted = 1u;
    if (wanted > CShadowMap::kMaxCascades) wanted = CShadowMap::kMaxCascades;

    if (m_ShadowReady && m_ShadowCascadeCount == wanted) return true;

    // 枚数が変わったら作り直す。atlas の幅が枚数で決まるので、使い回せない。
    if (m_ShadowReady) m_Shadow.Shutdown();
    m_ShadowReady = false;

    if (m_Shadow.Init(device, kShadowMapSize, wanted).IsErr()) {
        ACS_LOG_WARN("Scene3D: shadow map init failed; shadows stay off");
        return false;
    }

    m_ShadowReady = true;
    m_ShadowCascadeCount = wanted;
    return true;
}

bool ALegacyScene3DAdapter::EnsureAmbientOcclusion(
    IRhiDevice& device, u32 width, u32 height) noexcept {
    if (width == 0u || height == 0u) return false;
    if (m_SsaoReady && m_SsaoWidth == width && m_SsaoHeight == height) return true;

    if (!m_SsaoReady) {
        if (m_NormalDepth.Init(device, width, height).IsErr()) {
            ACS_LOG_WARN("Scene3D: normal/depth prepass init failed; SSAO stays off");
            return false;
        }
        if (m_Ssao.Init(device, width, height).IsErr()) {
            ACS_LOG_WARN("Scene3D: SSAO init failed; SSAO stays off");
            m_NormalDepth.Shutdown();
            return false;
        }
        m_SsaoReady = true;
    } else {
        // 画面の大きさが変わった。作り直せなかったら切る。古い大きさのまま読むと
        // 遮蔽が画面とずれて «物と関係の無い位置に汚れ» が出る。
        if (m_NormalDepth.Resize(width, height).IsErr() ||
            m_Ssao.Resize(width, height).IsErr()) {
            ACS_LOG_WARN("Scene3D: SSAO resize failed; SSAO stays off");
            m_SsaoReady = false;
            m_Ssao.Shutdown();
            m_NormalDepth.Shutdown();
            return false;
        }
    }

    m_SsaoWidth = width;
    m_SsaoHeight = height;
    return true;
}

bool ALegacyScene3DAdapter::RenderNormalDepthPrepass(
    FRenderContext& context) noexcept {
    if (!m_SsaoReady) return false;

    // 描く数を先に数える。object CB の入れ物は使い回しなので、足りないまま描くと
    // 後ろのメッシュの法線が黙って抜け、そこだけ遮蔽が付かない。
    u32 mesh_count = 0u;
    TArray<const ANode*> count_stack;
    if (!count_stack.TryAdd(&Graph().Root())) return false;
    while (!count_stack.IsEmpty()) {
        const ANode* node = count_stack.Last();
        count_stack.Pop();
        if (node == nullptr) continue;
        if (FindSprite(*node) != nullptr) {
            for (u32 index = 0u; index < node->ChildCount(); ++index)
                if (!count_stack.TryAdd(node->Child(index))) return false;
            continue;
        }
        if (const AMeshComponent3D* component = FindMesh(*node)) {
            if (GpuMeshFor(*component) != nullptr) ++mesh_count;
        }
        for (u32 index = 0u; index < node->ChildCount(); ++index)
            if (!count_stack.TryAdd(node->Child(index))) return false;
    }
    if (mesh_count == 0u) return false;
    if (!m_NormalDepth.BeginFrame(mesh_count)) return false;

    IRhiCommandList& command_list = context.Cmd();
    // 動きは使わない (遮蔽が要るのは法線と深度だけ)。前フレームの行列に現フレームの
    // ものを渡して、動き量を 0 にしておく。
    const FMat4 view_projection = m_Camera.ViewProjection();
    if (!m_NormalDepth.Begin(command_list, view_projection, view_projection))
        return false;

    bool complete = true;
    TArray<const ANode*> stack;
    if (!stack.TryAdd(&Graph().Root())) {
        m_NormalDepth.End(command_list);
        return false;
    }
    while (!stack.IsEmpty()) {
        const ANode* node = stack.Last();
        stack.Pop();
        if (node == nullptr) continue;

        for (u32 index = 0u; index < node->ChildCount(); ++index)
            if (!stack.TryAdd(node->Child(index))) { complete = false; break; }

        const AMeshComponent3D* const component =
            FindSprite(*node) == nullptr ? FindMesh(*node) : nullptr;
        if (component == nullptr) continue;

        const FGpuMesh* const gpu = GpuMeshFor(*component);
        if (gpu == nullptr || !gpu->vertex_buffer || !gpu->index_buffer) continue;

        const FMat4 model = node->World().ToMat4();
        if (!m_NormalDepth.DrawMesh(command_list, *gpu, model, model))
            complete = false;
    }
    m_NormalDepth.End(command_list);
    return complete;
}

bool ALegacyScene3DAdapter::RenderAmbientOcclusionPass(
    IRhiDevice& device, FRenderContext& context) noexcept {
    m_SsaoDrawn = false;
    if (m_SsaoParams.Intensity <= 0.0f) return false;
    if (!EnsureAmbientOcclusion(device, context.Width(), context.Height()))
        return false;
    if (!RenderNormalDepthPrepass(context)) return false;

    IRhiTexture* const prepass_depth = m_NormalDepth.DepthTexture();
    IRhiTexture* const prepass_normal = m_NormalDepth.OutputNormalTexture();
    if (prepass_depth == nullptr || prepass_normal == nullptr) return false;

    const FMat4 view_projection = m_Camera.ViewProjection();
    m_Ssao.Render(
        device, context.Cmd(), *prepass_depth, *prepass_normal,
        view_projection, Inverse(view_projection), m_Camera.View(),
        m_Camera.Eye(), SunDirection(),
        m_SsaoParams.Intensity,
        m_SsaoParams.Radius > 0.0f ? m_SsaoParams.Radius : 0.5f);

    m_SsaoDrawn = m_Ssao.OutputTexture() != nullptr;
    return m_SsaoDrawn;
}

bool ALegacyScene3DAdapter::EnsureReflections(
    IRhiDevice& device, EFormat hdr_format, u32 width, u32 height) noexcept {
    if (width == 0u || height == 0u) return false;
    if (m_SsrReady && m_SsrWidth == width && m_SsrHeight == height) return true;

    if (!m_SsrReady) {
        if (m_Ssr.Init(device, hdr_format, width, height).IsErr()) {
            ACS_LOG_WARN("Scene3D: SSR init failed; reflections stay off");
            return false;
        }
        m_SsrReady = true;
    } else if (m_Ssr.Resize(width, height).IsErr()) {
        // 古い大きさの反射を読むと、画面と対応しない «別の場所の景色» が映る。
        ACS_LOG_WARN("Scene3D: SSR resize failed; reflections stay off");
        m_SsrReady = false;
        m_SsrValid = false;
        m_Ssr.Shutdown();
        return false;
    }

    // 深度の階層。これが無いと反射のレイは 1 段だけの粗い探索になり、ほとんど何にも
    // 当たらない。**無くても反射自体は成立する**ので、作れなくても切らない。
    if (!m_HiZReady) {
        m_HiZReady = m_HiZ.Init(device, width, height).IsOk();
        if (!m_HiZReady)
            ACS_LOG_WARN("Scene3D: Hi-Z init failed; reflections stay coarse");
    } else if (m_HiZ.Resize(width, height).IsErr()) {
        ACS_LOG_WARN("Scene3D: Hi-Z resize failed; reflections stay coarse");
        m_HiZReady = false;
        m_HiZ.Shutdown();
    }

    // 大きさが変わった直後の履歴は前の大きさのもの。1 フレーム捨てる。
    m_SsrValid = false;
    m_SsrWidth = width;
    m_SsrHeight = height;
    return true;
}

bool ALegacyScene3DAdapter::RenderReflectionPass(
    IRhiDevice& device, FRenderContext& context,
    IRhiTexture& scene_color, IRhiTexture& scene_depth) noexcept {
    if (m_SsrParams.Intensity <= 0.0f) {
        m_SsrValid = false;
        return false;
    }
    if (!EnsureReflections(device, scene_color.PixelFormat(),
                           context.Width(), context.Height())) {
        m_SsrValid = false;
        return false;
    }
    // 法線は遮蔽と同じ前段のもの。前段が描けていないフレームは反射も作れない。
    IRhiTexture* const prepass_normal =
        m_SsaoDrawn ? m_NormalDepth.OutputNormalTexture() : nullptr;
    if (prepass_normal == nullptr) {
        m_SsrValid = false;
        return false;
    }

    const FMat4 view_projection = m_Camera.ViewProjection();
    // 初回は前フレームが無い。単位行列を渡すと再投影が切れる、という約束になっている。
    const FMat4 previous = m_HasPrevViewProjection
        ? m_PrevViewProjection : FMat4{};

    // 深度の階層を先に作る。レイがこれを使って遠くまで飛べるようになる。
    if (m_HiZReady) m_HiZ.Build(device, context.Cmd(), scene_depth);

    m_Ssr.Render(
        device, context.Cmd(), scene_color, scene_depth, *prepass_normal,
        view_projection, Inverse(view_projection), previous,
        m_Camera.Eye(), m_SsrParams.Intensity,
        m_NormalDepth.OutputTexture(),
        m_HiZReady ? m_HiZ.EvenTexture() : nullptr,
        m_HiZReady ? m_HiZ.OddTexture() : nullptr,
        m_HiZReady ? m_HiZ.MipCount() : 0u);

    m_SsrValid = m_Ssr.HasValidOutput() && m_Ssr.OutputTexture() != nullptr;
    return m_SsrValid;
}

namespace {

/**
 * 1 頂点を骨で動かす。
 *
 * @details
 * 影響する 4 本の行列を重みで混ぜてから 1 回だけ掛ける。行列を混ぜてから掛けるのは
 * 「4 回変換して混ぜる」のと同じ結果で、掛け算が 1 回で済む (線形変換なので入れ替えられる)。
 *
 * @param source 元の頂点。
 * @param palette ボーンパレット。
 * @param bone_count パレットの数。
 * @param out 書き込み先。
 */
void SkinVertex(const FSkinnedVertex& source, const FMat4* palette, u32 bone_count,
                FMeshVertex& out) noexcept {
    FMat4 blended{};
    for (u32 row = 0u; row < 4u; ++row)
        for (u32 column = 0u; column < 4u; ++column) blended.m[row][column] = 0.0f;

    f32 total = 0.0f;
    for (u32 influence = 0u; influence < 4u; ++influence) {
        const f32 weight = source.weights[influence];
        if (weight <= 0.0f) continue;

        const u32 bone = source.bones[influence];
        if (bone >= bone_count) continue;

        const FMat4& matrix = palette[bone];
        for (u32 row = 0u; row < 4u; ++row)
            for (u32 column = 0u; column < 4u; ++column)
                blended.m[row][column] += matrix.m[row][column] * weight;
        total += weight;
    }

    // どの骨にも届かなかった頂点は、動かさずそのまま置く。0 行列を掛けると原点へ潰れる。
    if (total <= 1e-6f) {
        out.position = source.position;
        out.normal = source.normal;
    } else {
        out.position = TransformPoint(source.position, blended);
        // 法線は平行移動を受けない。非一様スケールでは厳密には逆転置が要るが、
        // 骨のスケールは普通ほぼ一様なので、回転成分で足りる。
        out.normal = Normalize(TransformVector(source.normal, blended));
    }
    out.u = source.u;
    out.v = source.v;
}

} // namespace

bool ALegacyScene3DAdapter::UpdateSkinnedMeshes(IRhiDevice& device) noexcept {
    m_SkinnedDrawn.Empty();

    TArray<ANode*> stack;
    if (!stack.TryAdd(&Graph().Root())) return false;

    FMat4 palette[CSkinnedShader::kMaxBones];
    bool any = false;

    while (!stack.IsEmpty()) {
        ANode* const node = stack.Last();
        stack.Pop();
        if (node == nullptr) continue;
        if (!node->IsVisible() || !node->IsEnabled()) continue;

        for (u32 index = 0u; index < node->ChildCount(); ++index)
            if (!stack.TryAdd(node->Child(index))) break;

        ASkinnedMeshComponent3D* const component =
            node->GetComponent<ASkinnedMeshComponent3D>();
        if (component == nullptr || !component->IsRenderable()) continue;

        FSkinnedInstance* const instance = SkinnedInstanceFor(device, *component);
        if (instance == nullptr) continue;

        const ASkinnedMeshAsset& asset = *component->MeshAsset();
        const u32 bone_count =
            component->Player().WritePalette(palette, CSkinnedShader::kMaxBones);

        // CPU で動かして、動的な頂点バッファへ入れ直す。**そうすると以降は普通のメッシュ**
        // なので、影・遮蔽・反射・IBL が静的メッシュと同じように効く。
        // GPU スキニングより CPU を使うが、質感が揃わない方が問題として大きい。
        for (usize index = 0u; index < asset.Vertices().Num(); ++index)
            SkinVertex(asset.Vertices()[index], palette, bone_count, instance->Scratch[index]);

        if (instance->Mesh.vertex_buffer) {
            instance->Mesh.vertex_buffer->Update(
                instance->Scratch.GetData(),
                instance->Scratch.Num() * sizeof(FMeshVertex));
        }

        FSkinnedDraw draw;
        draw.Mesh = &instance->Mesh;
        draw.Model = node->World().ToMat4();
        draw.Color = component->Color();
        if (m_SkinnedDrawn.TryAdd(draw)) any = true;
    }

    return any;
}

ALegacyScene3DAdapter::FSkinnedInstance* ALegacyScene3DAdapter::SkinnedInstanceFor(
    IRhiDevice& device, const ASkinnedMeshComponent3D& component) noexcept {
    // **インスタンスごとに持つ。** 同じモデルでも姿勢が違えば頂点が違うので、
    // アセット単位で共有すると全員が最後の 1 体の姿勢になる。
    for (usize index = 0u; index < m_SkinnedInstances.Num(); ++index) {
        if (m_SkinnedInstances[index].Component == &component)
            return &m_SkinnedInstances[index];
    }

    const ASkinnedMeshAsset* const asset = component.MeshAsset().Get();
    if (asset == nullptr) return nullptr;

    FSkinnedInstance instance;
    instance.Component = &component;
    if (!instance.Scratch.TrySetNum(asset->Vertices().Num())) return nullptr;

    FBufferDesc vertex_desc{};
    vertex_desc.size = instance.Scratch.Num() * sizeof(FMeshVertex);
    vertex_desc.usage = EBufferUsage::Vertex;
    vertex_desc.cpu_writable = true;   // 毎フレーム書き換える
    auto vertex_buffer = CreateRhiBuffer(device, vertex_desc);
    if (vertex_buffer.IsErr()) {
        ACS_LOG_WARN("Scene3D: skinned vertex buffer creation failed; model stays hidden");
        return nullptr;
    }

    FBufferDesc index_desc{};
    index_desc.size = asset->Indices().Num() * sizeof(u32);
    index_desc.usage = EBufferUsage::Index32;
    index_desc.initial_data = asset->Indices().GetData();
    auto index_buffer = CreateRhiBuffer(device, index_desc);
    if (index_buffer.IsErr()) {
        ACS_LOG_WARN("Scene3D: skinned index buffer creation failed; model stays hidden");
        return nullptr;
    }

    instance.Mesh.vertex_buffer = Move(vertex_buffer.Value());
    instance.Mesh.index_buffer = Move(index_buffer.Value());
    instance.Mesh.vertex_count = static_cast<u32>(instance.Scratch.Num());
    instance.Mesh.index_count = static_cast<u32>(asset->Indices().Num());
    instance.Mesh.vertex_stride = sizeof(FMeshVertex);

    if (!m_SkinnedInstances.TryAdd(Move(instance))) return nullptr;
    return &m_SkinnedInstances[m_SkinnedInstances.Num() - 1u];
}

bool ALegacyScene3DAdapter::RenderShadowPass(FRenderContext& context) noexcept {
    m_ShadowDrawn = false;
    if (!m_ShadowReady) return false;

    IRhiTexture* const depth = m_Shadow.DepthTexture();
    IRhiPipeline* const pipeline = m_Shadow.CasterPipeline();
    IRhiBuffer* const light_cb = m_Shadow.LightCB();
    if (depth == nullptr || pipeline == nullptr || light_cb == nullptr) return false;

    FVec3 center{0.0f, 0.0f, 0.0f};
    f32 radius = 0.0f;
    if (!ComputeSceneBounds(center, radius) || radius <= 0.0f) return false;

    // 影を落とすメッシュを先に数える。SetCaster は 1 灯ぶんの入れ物を使い回すので、
    // 足りないまま描くと後ろのメッシュの影が黙って抜ける。
    u32 caster_count = 0u;
    TArray<const ANode*> count_stack;
    if (!count_stack.TryAdd(&Graph().Root())) return false;
    while (!count_stack.IsEmpty()) {
        const ANode* node = count_stack.Last();
        count_stack.Pop();
        if (node == nullptr) continue;
        if (FindSprite(*node) != nullptr) {
            for (u32 index = 0u; index < node->ChildCount(); ++index)
                if (!count_stack.TryAdd(node->Child(index))) return false;
            continue;
        }
        if (const AMeshComponent3D* component = FindMesh(*node)) {
            if (component->CastsShadow() && GpuMeshFor(*component) != nullptr) ++caster_count;
        }
        for (u32 index = 0u; index < node->ChildCount(); ++index)
            if (!count_stack.TryAdd(node->Child(index))) return false;
    }
    // 骨で動くメッシュも影を落とす。**入れないと、そこだけ影が抜けて浮いて見える。**
    caster_count += static_cast<u32>(m_SkinnedDrawn.Num());
    if (caster_count == 0u) return false;

    if (!m_Shadow.BeginFrame(caster_count)) return false;

    // 影の «撮り方» を決める。
    //
    // 1 枚なら、これまでどおりシーン全体を 1 つの投影で覆う。
    // 2 枚以上なら、カメラの視錐台を near から «影を描く距離» までで分割し、手前を細かく
    // 撮る。**カメラの far は使わない。** far は数 km あることがあり、そこまで枚数を配ると
    // 手前がすかすかになる。
    const u32 cascade_count = m_Shadow.CascadeCount();
    if (cascade_count > 1u) {
        f32 shadow_distance = m_ShadowParams.Distance;
        if (!(shadow_distance > 0.0f)) shadow_distance = 120.0f;
        // シーンがそれより小さいなら、わざわざ広く撮らない (そのぶん手前が細かくなる)。
        const f32 scene_reach = radius * 4.0f;
        if (scene_reach > 0.0f && scene_reach < shadow_distance) shadow_distance = scene_reach;

        m_Shadow.SetDirectionalLightCascades(
            SunDirection(), m_Camera.View(), m_Camera.Projection(),
            0.05f, shadow_distance, Clamp(m_ShadowParams.SplitBlend, 0.0f, 1.0f));
    } else {
        m_Shadow.SetDirectionalLight(SunDirection(), center, radius);
    }

    IRhiCommandList& command_list = context.Cmd();
    command_list.BeginShadowPass(*depth, 1.0f);
    command_list.SetPipeline(*pipeline);

    for (u32 cascade = 0u; cascade < cascade_count; ++cascade) {
        // 枚数ぶん、同じ物をもう一度描く。**これが CSM の値段。**
        m_Shadow.SetCurrentCascade(cascade);
        command_list.SetViewport(m_Shadow.CascadeViewport(cascade));
        command_list.SetScissor(m_Shadow.CascadeScissor(cascade));

        IRhiBuffer* const cascade_cb = m_Shadow.LightCB();
        if (cascade_cb == nullptr) continue;
        command_list.SetConstantBuffer(0, *cascade_cb);

        TArray<const ANode*> stack;
        if (!stack.TryAdd(&Graph().Root())) break;
        while (!stack.IsEmpty()) {
            const ANode* node = stack.Last();
            stack.Pop();
            if (node == nullptr) continue;

            for (u32 index = 0u; index < node->ChildCount(); ++index)
                if (!stack.TryAdd(node->Child(index))) break;

            const AMeshComponent3D* const component =
                FindSprite(*node) == nullptr ? FindMesh(*node) : nullptr;
            if (component == nullptr || !component->CastsShadow()) continue;

            const FGpuMesh* const gpu = GpuMeshFor(*component);
            if (gpu == nullptr || !gpu->vertex_buffer || !gpu->index_buffer) continue;

            if (!m_Shadow.TrySetCaster(node->World().ToMat4())) continue;

            IRhiBuffer* const object_cb = m_Shadow.CasterObjectCB();
            if (object_cb == nullptr) continue;

            command_list.SetConstantBuffer(1, *object_cb);
            command_list.SetVertexBuffer(*gpu->vertex_buffer, gpu->vertex_stride);
            command_list.SetIndexBuffer(*gpu->index_buffer);
            command_list.DrawIndexed(gpu->index_count, 0u, 0);
        }

        // 骨で動くメッシュも同じ形で落とす。CPU で変形済みなので区別が要らない。
        for (usize index = 0u; index < m_SkinnedDrawn.Num(); ++index) {
            const FSkinnedDraw& draw = m_SkinnedDrawn[index];
            if (draw.Mesh == nullptr || !draw.Mesh->vertex_buffer || !draw.Mesh->index_buffer)
                continue;
            if (!m_Shadow.TrySetCaster(draw.Model)) continue;

            IRhiBuffer* const object_cb = m_Shadow.CasterObjectCB();
            if (object_cb == nullptr) continue;

            command_list.SetConstantBuffer(1, *object_cb);
            command_list.SetVertexBuffer(*draw.Mesh->vertex_buffer, draw.Mesh->vertex_stride);
            command_list.SetIndexBuffer(*draw.Mesh->index_buffer);
            command_list.DrawIndexed(draw.Mesh->index_count, 0u, 0);
        }
    }

    command_list.EndShadowPass(*depth);

    if (m_Shadow.CasterOverflowed()) {
        // 黙って抜けると «一部だけ影が出ない» になり、原因が分かりにくい。
        ACS_LOG_WARN("Scene3D: shadow caster buffer overflowed; some shadows are missing");
    }

    m_ShadowDrawn = m_Shadow.CasterDrawCount() > 0u;
    return m_ShadowDrawn;
}

void ALegacyScene3DAdapter::UpdateSkyFromSun() noexcept {
    m_Sky.SetSunDirection(SunDirection());

    // 空に描く太陽の色は «明るさ» ではなく «色味»。光の色は強さが掛かっているので、
    // 一番大きい成分を 1 に揃えてから渡す。そうしないと強い光で太陽が白く飛ぶ。
    if (m_Lights.DirectionalCount() == 0u) {
        m_Sky.SetSunColor(kDefaultSkySunColor);
        return;
    }

    const FVec3 color = m_Lights.DirectionalLights()[0].color;
    f32 peak = color.x > color.y ? color.x : color.y;
    if (color.z > peak) peak = color.z;
    if (peak <= 0.0f) {
        m_Sky.SetSunColor(kDefaultSkySunColor);
        return;
    }

    const f32 scale = 1.0f / peak;
    m_Sky.SetSunColor(FVec3{color.x * scale, color.y * scale, color.z * scale});
}

FVec3 ALegacyScene3DAdapter::SunDirection() const noexcept {
    if (m_Lights.DirectionalCount() > 0u) return m_Lights.DirectionalLights()[0].direction;
    return Normalize(kDefaultSunDirection);
}

FVec3 ALegacyScene3DAdapter::SunColorForWater() const noexcept {
    if (m_Lights.DirectionalCount() == 0u) return kDefaultWaterSunColor;

    const FVec3 color = m_Lights.DirectionalLights()[0].color;
    return FVec3{color.x * kWaterSunBoost, color.y * kWaterSunBoost, color.z * kWaterSunBoost};
}

bool ALegacyScene3DAdapter::DrawPbrScene(
    FRenderContext& context,
    CPbrShader& shader,
    const FWaterDraw* excluded_water,
    u32 excluded_count,
    bool subsurface_mrt) noexcept {
    // シーンに置かれた光を使う。1 灯も無いときだけ既定の太陽へ落とす
    // (既存の authored scene は光を持たないので、見え方を変えないため)。
    FDirLight fallback[1];
    fallback[0].direction = Normalize(kDefaultSunDirection);
    fallback[0].color = kDefaultSunColor;

    const bool use_scene_lights = m_Lights.DirectionalCount() > 0u;
    shader.SetLights(
        m_Camera.ViewProjection(),
        m_Camera.Eye(),
        use_scene_lights ? m_Lights.DirectionalLights() : fallback,
        use_scene_lights ? m_Lights.DirectionalCount() : 1u,
        kDefaultAmbient);
    shader.SetPointLights(m_Lights.PointLights(), m_Lights.PointCount());

    // 環境光。空を映したものを渡すと、陰の側が «一定の暗い色» で潰れなくなる。
    if (m_IblReady) {
        shader.SetIbl(m_Ibl.IrradianceMap(), m_Ibl.PrefilterMap(),
                      m_Ibl.BrdfLut(), m_Ibl.PrefilterMips());
    }

    // 影。描けなかったフレームは nullptr を渡して切る (前のフレームの深度を
    // 使い回すと、動いた物の影が 1 フレーム遅れて付いてくる)。
    if (m_ShadowDrawn) {
        const f32 bias = m_ShadowParams.DepthBias > 0.0f
            ? m_ShadowParams.DepthBias : kShadowDepthBias;
        // texel は «1 枚ぶん» の大きさ。atlas 全体の幅で割ると、枚数を増やすほど
        // ぼかしが細くなって縞が出る。
        const f32 texel = 1.0f / static_cast<f32>(kShadowMapSize);
        const u32 cascade_count = m_Shadow.CascadeCount();
        if (cascade_count > 1u) {
            FMat4 light_vp[CShadowMap::kMaxCascades];
            f32 splits[CShadowMap::kMaxCascades];
            for (u32 cascade = 0u; cascade < cascade_count; ++cascade) {
                light_vp[cascade] = m_Shadow.LightViewProjection(cascade);
                splits[cascade] = m_Shadow.CascadeSplit(cascade);
            }
            shader.SetShadowMapCascades(
                m_Shadow.DepthTexture(), light_vp, splits, cascade_count, bias, texel);
        } else {
            shader.SetShadowMap(m_Shadow.DepthTexture(), m_Shadow.LightViewProjection(),
                                bias, texel);
        }
    } else {
        shader.SetShadowMap(nullptr, FMat4{});
    }
    // 同じフレームで積分した雲透過率を、太陽である第0有向光源の直接光だけへ掛ける。
    // 雲を描けなかったフレームは無効値を渡し、古い影を残さない。
    shader.SetCloudShadowMap(m_CloudsDrawn ? m_Clouds.WorldShadowMap() : FVolumetricCloudWorldShadowMap{});

    // 遮蔽。物と床が接するところを締める。影の地図は解像度の都合でそこまで届かず、
    // これが無いと «置いてあるのか浮いているのか» が読めない。
    // 影と同じく、描けなかったフレームは切る (前のフレームの遮蔽は画面とずれている)。
    if (m_SsaoDrawn) {
        shader.SetSsao(m_Ssao.OutputTexture(), m_SsaoParams.Intensity,
                       m_SsaoWidth, m_SsaoHeight);
    } else {
        shader.SetSsao(nullptr, 0.0f, 1u, 1u);
    }

    // 反射。**前のフレームで作ったもの**を混ぜる。反射を作るには完成したシーンの色が
    // 要るので、同じフレームの結果は間に合わない。SSR は元々時間方向に均すので破綻しない。
    if (m_SsrValid) {
        shader.SetSsr(m_Ssr.OutputTexture(), m_SsrParams.Intensity);
    } else {
        shader.SetSsr(nullptr, 0.0f);
    }
    // 霧。場面から触れる (Fog())。高さの基準を決めていなければシーンの位置から自動で。
    const f32 fog_base = m_Fog.HeightBase == FLT_MAX
        ? m_OrbitCameraState.target.y - m_OrbitCameraState.distance * 0.25f
        : m_Fog.HeightBase;
    shader.SetFog(m_Fog.Color, m_Fog.Density, m_Fog.HeightFalloff, fog_base);

    struct FRenderEntry {
        const ANode* Node = nullptr;
        bool ParentVisible = true;
        bool ParentEnabled = true;
    };
    TArray<FRenderEntry> stack;
    if (!stack.TryAdd(FRenderEntry{&Graph().Root(), true, true}))
        return false;

    bool draws_valid = true;
    while (!stack.IsEmpty()) {
        const FRenderEntry entry = stack.Last();
        stack.Pop();
        const ANode* node = entry.Node;
        if (node == nullptr) continue;
        const bool enabled = entry.ParentEnabled && node->IsEnabled();
        const bool visible = entry.ParentVisible && node->IsVisible();
        for (u32 index = node->ChildCount(); index > 0u; --index) {
            if (!stack.TryAdd(
                    FRenderEntry{node->Child(index - 1u), visible, enabled})) {
                return false;
            }
        }
        if (!enabled || !visible) continue;
        if (FindSprite(*node) != nullptr) continue;
        const AMeshComponent3D* component = FindMesh(*node);
        if (component == nullptr) continue;
        bool excluded = false;
        for (u32 index = 0u; index < excluded_count; ++index) {
            if (excluded_water[index].Node == node) {
                excluded = true;
                break;
            }
        }
        if (excluded) continue;
        const FGpuMesh* gpu = GpuMeshFor(*component);
        if (gpu == nullptr || !gpu->vertex_buffer || !gpu->index_buffer)
            continue;

        FVec4 tint = component->Color();
        FVec3 base{tint.x, tint.y, tint.z};
        f32 metallic = 0.0f;
        f32 roughness = 0.5f;
        f32 ao = 1.0f;
        shader.ClearSubstrateSurface();
        shader.SetExtParams(0.0f, 0.1f, 0.0f);
        shader.SetEmissive(FVec3{0.0f, 0.0f, 0.0f}, 0.0f);
        shader.SetSheen(FVec3{1.0f, 1.0f, 1.0f}, 0.0f);
        shader.SetSubsurface(FVec3{1.0f, 0.3f, 0.2f}, 0.0f);
        shader.SetNormalMap(nullptr, 0.0f);

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
            shader.SetExtParams(
                pbr.clearcoat, pbr.clearcoatRoughness, pbr.anisotropy);
            shader.SetEmissive(pbr.emissive, pbr.emissiveStrength);
            shader.SetSheen(
                pbr.sheenColor, pbr.sheen, pbr.sheenRoughness);
            shader.SetSubsurface(
                pbr.subsurfaceColor, pbr.subsurface);
            if (material.substrate.enabled)
                (void)shader.SetSubstrateMaterial(material.substrate, m_Time);
        } else if (const AWaterSurface3DComponent* water = FindWater(*node)) {
            const FWaterSurface3DParams parameters = water->ToRenderParams();
            base = parameters.shallow_color * 0.72f
                + parameters.deep_color * 0.28f;
            roughness = parameters.roughness;
        }
        if (subsurface_mrt) {
            if (!shader.DrawMeshSubsurfaceMrt(
                    context.Cmd(), *gpu, node->World().ToMat4(),
                    base, metallic, roughness, ao)) {
                draws_valid = false;
            }
        } else {
            draws_valid =
                shader.DrawMesh(
                    context.Cmd(), *gpu, node->World().ToMat4(),
                    base, metallic, roughness, ao) &&
                draws_valid;
        }
    }

    // 骨で動くメッシュ。CPU で変形済みなので、**ここから先はただのメッシュ**として
    // 静的なものと同じ shader で描く。IBL も影も遮蔽も反射も同じように効く。
    for (usize index = 0u; index < m_SkinnedDrawn.Num(); ++index) {
        const FSkinnedDraw& draw = m_SkinnedDrawn[index];
        if (draw.Mesh == nullptr) continue;

        shader.ClearSubstrateSurface();
        shader.SetExtParams(0.0f, 0.1f, 0.0f);
        shader.SetEmissive(FVec3{0.0f, 0.0f, 0.0f}, 0.0f);
        shader.SetSheen(FVec3{1.0f, 1.0f, 1.0f}, 0.0f);
        shader.SetSubsurface(FVec3{1.0f, 0.3f, 0.2f}, 0.0f);
        shader.SetNormalMap(nullptr, 0.0f);

        if (subsurface_mrt) {
            if (!shader.DrawMeshSubsurfaceMrt(
                    context.Cmd(), *draw.Mesh, draw.Model, draw.Color, 0.0f, 0.55f, 1.0f)) {
                draws_valid = false;
            }
        } else {
            draws_valid =
                shader.DrawMesh(
                    context.Cmd(), *draw.Mesh, draw.Model, draw.Color, 0.0f, 0.55f, 1.0f) &&
                draws_valid;
        }
    }

    return draws_valid;
}

bool ALegacyScene3DAdapter::EnsureGpu(FRenderContext& context) noexcept {
    if (m_GpuReady) return true;
    if (m_GpuAttempted) return false;
    IRhiDevice* device = context.GetRenderer().Device();
    if (device == nullptr) return false;
    m_GpuAttempted = true;

    const TSharedPtr<AMeshAsset> cube = Primitive::MakeCube();
    const TSharedPtr<AMeshAsset> sphere =
        Primitive::MakeSphere(0.5f, 48u, 24u);
    const TSharedPtr<AMeshAsset> plane = Primitive::MakePlane();
    if (!cube || !sphere || !plane || UploadMesh(*device, *cube, m_Cube).IsErr() || UploadMesh(*device, *sphere, m_Sphere).IsErr() || UploadMesh(*device, *plane, m_Plane).IsErr() || !UploadGraphMeshes(*device) || !UploadGraphSprites(*device)) {
        ACS_LOG_ERROR("LegacyScene3DAdapter: GPU mesh initialization failed");
        DrainAndReleaseGpu();
        m_GpuAttempted = true;
        return false;
    }
    m_GpuReady = true;
    return true;
}

bool ALegacyScene3DAdapter::EnsureHdrFrameResources(
    IRhiDevice& device,
    u32 width,
    u32 height,
    EFormat swapchain_format,
    EFormat depth_format,
    EGpuCommitSubsystem& frame_commit) noexcept {
    if (width == 0u || height == 0u) return false;
    m_FrameDepthFormat = depth_format;
    if (m_DepthAttemptWidth != 0u
        && (m_DepthAttemptWidth != width
            || m_DepthAttemptHeight != height)) {
        m_DepthSnapshotFailed = false;
    }
    const FSceneRenderFeatures scene_features =
        ScanSceneRenderFeatures(Graph().Root());
    const bool scene_has_water = scene_features.has_water;
    const bool scene_has_sprites = scene_features.has_sprites;
    // CSceneNodeGraph has no mutation revision yet. Scan alongside the existing
    // water feature query so retained Graph references, visibility changes
    // and runtime material edits take effect on the very next frame.
    m_SsssRequested = scene_features.needs_subsurface_mrt;
    const bool scene_needs_subsurface = m_SsssRequested;

    const char* const backend_name = device.BackendName();
    const bool raw_dx12 = backend_name != nullptr
        && std::strcmp(backend_name, "DX12") == 0;

    if (m_PostGpuState == EShaderGpuState::Unavailable) {
        if (device.SupportsAsyncShaderCompilation()) {
            auto result = CPostProcess::BeginCompileShadersAsync(device);
            if (result.IsErr()) {
                m_PostGpuState = EShaderGpuState::Failed;
                ACS_LOG_WARN(
                    "LegacyScene3DAdapter: asynchronous post shader "
                    "submission failed; stable clear remains active: %s",
                    result.Error().message);
            } else {
                m_PostPendingShaders = Move(result.Value());
                m_PostGpuState = EShaderGpuState::Compiling;
            }
        } else if (raw_dx12) {
            if (!BeginPostCpuCompilation())
                m_PostGpuState = EShaderGpuState::Failed;
        } else {
            m_PostGpuState = EShaderGpuState::Failed;
            ACS_LOG_WARN(
                "LegacyScene3DAdapter: post shader compilation is "
                "unsupported by backend %s; stable clear remains active",
                backend_name ? backend_name : "(unknown)");
        }
    }

    if (m_HdrShaderGpuState == EShaderGpuState::Unavailable) {
        m_HdrPendingSlot = static_cast<u8>(m_HdrActiveSlot ^ 1u);
        m_HdrShaders[m_HdrPendingSlot].Shutdown();
        m_HdrPendingIsInitialized = false;
        if (device.SupportsAsyncShaderCompilation()) {
            auto result = CPbrShader::BeginCompileShadersAsync(
                device, false);
            if (result.IsErr()) {
                m_HdrShaderGpuState = EShaderGpuState::Failed;
                ACS_LOG_WARN(
                    "LegacyScene3DAdapter: asynchronous HDR PBR shader "
                    "submission failed; stable clear remains active: %s",
                    result.Error().message);
            } else {
                m_HdrPendingShaders = Move(result.Value());
                m_HdrShaderGpuState = EShaderGpuState::Compiling;
            }
        } else if (raw_dx12) {
            if (!BeginHdrPbrCpuCompilation(
                    device, m_Post.HdrFormat(), depth_format))
                m_HdrShaderGpuState = EShaderGpuState::Failed;
        } else {
            m_HdrShaderGpuState = EShaderGpuState::Failed;
            ACS_LOG_WARN(
                "LegacyScene3DAdapter: HDR PBR shader compilation is "
                "unsupported by backend %s; stable clear remains active",
                backend_name ? backend_name : "(unknown)");
        }
    }

    AdvancePostInitialization(
        device, width, height, swapchain_format, frame_commit);
    AdvanceHdrPbrInitialization(device, frame_commit);
    AdvanceHdrSsssInitialization(
        device, frame_commit, scene_needs_subsurface);
    AdvanceSubsurfaceInitialization(
        device, width, height, frame_commit,
        scene_needs_subsurface);
    if (m_SkyGpuState == ESkyGpuState::Unavailable) {
        if (device.SupportsAsyncShaderCompilation()) {
            auto result = CSky::BeginCompileShadersAsync(device);
            if (result.IsErr()) {
                m_SkyGpuState = ESkyGpuState::Failed;
                ACS_LOG_WARN(
                    "LegacyScene3DAdapter: asynchronous sky compilation "
                    "submission failed; the stable HDR clear remains active: "
                    "%s",
                    result.Error().message);
            } else {
                m_SkyPendingShaders = Move(result.Value());
                m_SkyGpuState = ESkyGpuState::Compiling;
            }
        } else if (raw_dx12) {
            if (!BeginSkyCpuCompilation()) {
                m_SkyGpuState = ESkyGpuState::Failed;
            }
        } else {
            m_SkyGpuState = ESkyGpuState::Failed;
            ACS_LOG_WARN(
                "LegacyScene3DAdapter: sky compilation is unsupported by "
                "backend %s; the stable HDR clear remains active",
                backend_name ? backend_name : "(unknown)");
        }
    }
    AdvanceSkyInitialization(device, frame_commit);

    if (scene_has_sprites && m_SpriteGpuState == EShaderGpuState::Unavailable) {
        if (device.SupportsAsyncShaderCompilation()) {
            auto result =
                CSprite3DRenderer::BeginCompileShadersAsync(device);
            if (result.IsErr()) {
                m_SpriteGpuState = EShaderGpuState::Failed;
                ACS_LOG_WARN("LegacyScene3DAdapter: asynchronous 3D sprite shader submission failed: %s", result.Error().message);
            } else {
                m_SpritePendingShaders = Move(result.Value());
                m_SpriteGpuState = EShaderGpuState::Compiling;
            }
        } else if (raw_dx12) {
            if (!BeginSpriteCpuCompilation())
                m_SpriteGpuState = EShaderGpuState::Failed;
        } else {
            m_SpriteGpuState = EShaderGpuState::Failed;
            ACS_LOG_WARN("LegacyScene3DAdapter: 3D sprite shader compilation is unsupported by backend %s", backend_name ? backend_name : "(unknown)");
        }
    }
    AdvanceSpriteInitialization(device, depth_format, frame_commit, scene_has_sprites);

    const bool scene_needs_blit =
        scene_has_water
        || (scene_needs_subsurface
            && m_SsssGpuState == EShaderGpuState::Ready);
    if (scene_needs_blit
        && m_BlitGpuState == EShaderGpuState::Unavailable) {
        if (device.SupportsAsyncShaderCompilation()) {
            auto result = CBlit::BeginCompileShadersAsync(device);
            if (result.IsErr()) {
                m_BlitGpuState = EShaderGpuState::Failed;
                ACS_LOG_WARN(
                    "LegacyScene3DAdapter: asynchronous blit shader "
                    "submission failed; dependent effects stay on their "
                    "analytic fallback: %s",
                    result.Error().message);
            } else {
                m_BlitPendingShaders = Move(result.Value());
                m_BlitGpuState = EShaderGpuState::Compiling;
            }
        } else if (raw_dx12) {
            if (!BeginBlitCpuCompilation())
                m_BlitGpuState = EShaderGpuState::Failed;
        } else {
            m_BlitGpuState = EShaderGpuState::Failed;
            ACS_LOG_WARN(
                "LegacyScene3DAdapter: blit shader compilation is "
                "unsupported by backend %s; dependent effects stay on "
                "their analytic fallback",
                backend_name ? backend_name : "(unknown)");
        }
    }
    AdvanceBlitInitialization(
        device, frame_commit, scene_needs_blit);
    EnsureSubsurfaceAuxTargets(
        device, width, height, frame_commit,
        scene_needs_subsurface);

    if (scene_has_water
        && m_BlitGpuState == EShaderGpuState::Ready
        && (m_WaterBackground.Get() == nullptr
            || m_WaterBackground->Width() != width
            || m_WaterBackground->Height() != height)
        && (m_BackgroundAttemptWidth != width
            || m_BackgroundAttemptHeight != height)
        && TryClaimGpuCommit(
            frame_commit, EGpuCommitSubsystem::Water)) {
        m_BackgroundAttemptWidth = width;
        m_BackgroundAttemptHeight = height;
        FTextureDesc description{};
        description.width = width;
        description.height = height;
        description.format = m_Post.HdrFormat();
        description.is_render_target = true;
        auto texture = CreateRhiTexture(device, description);
        if (texture.IsErr()) {
            ACS_LOG_WARN(
                "LegacyScene3DAdapter: water background allocation failed; "
                "water remains on opaque PBR fallback: %s",
                texture.Error().message);
        } else {
            m_WaterBackground = Move(texture.Value());
        }
    }

    // A depth snapshot is needed even when the renderer's live depth exposes
    // an SRV: water samples the immutable opaque copy while the original stays
    // bound as a writable DSV for water-to-water and opaque occlusion.
    // The caller validates the live allocation before collecting any water.
    if (scene_has_water && depth_format == EFormat::D32_Float
        && !m_DepthSnapshotFailed
        && (m_WaterDepthSnapshot.Get() == nullptr
            || m_WaterDepthSnapshot->Width() != width
            || m_WaterDepthSnapshot->Height() != height)
        && (m_DepthAttemptWidth != width
            || m_DepthAttemptHeight != height)
        && TryClaimGpuCommit(
            frame_commit, EGpuCommitSubsystem::Water)) {
        m_DepthAttemptWidth = width;
        m_DepthAttemptHeight = height;
        FTextureDesc description{};
        description.width = width;
        description.height = height;
        description.format = depth_format;
        description.is_depth_target = true;
        description.shader_visible_depth = true;
        description.sample_count = 1u;
        auto texture = CreateRhiTexture(device, description);
        if (texture.IsErr()) {
            m_DepthSnapshotFailed = true;
            ACS_LOG_WARN(
                "LegacyScene3DAdapter: water depth snapshot allocation "
                "failed; opaque PBR fallback remains active: %s",
                texture.Error().message);
        } else {
            m_WaterDepthSnapshot = Move(texture.Value());
        }
    }

    if (m_WaterGpuState == EWaterGpuState::Unavailable
        && scene_has_water) {
        if (device.SupportsAsyncShaderCompilation()) {
            auto result =
                CWaterSurface3D::BeginCompileShadersAsync(device);
            if (result.IsErr()) {
                m_WaterGpuState = EWaterGpuState::Failed;
                ACS_LOG_WARN(
                    "LegacyScene3DAdapter: interactive-water shader "
                    "submission failed; opaque PBR fallback remains active: "
                    "%s",
                    result.Error().message);
            } else {
                m_WaterPendingShaders = Move(result.Value());
                m_WaterGpuState = EWaterGpuState::Compiling;
            }
        } else if (raw_dx12) {
            if (!BeginWaterCpuCompilation()) {
                m_WaterGpuState = EWaterGpuState::Failed;
            }
        } else {
            m_WaterGpuState = EWaterGpuState::Failed;
            ACS_LOG_WARN(
                "LegacyScene3DAdapter: interactive-water compilation is "
                "unsupported by backend %s; opaque PBR fallback remains "
                "active",
                backend_name ? backend_name : "(unknown)");
        }
    }
    AdvanceWaterInitialization(device, frame_commit, scene_has_water);
    return m_PostGpuState == EShaderGpuState::Ready
        && m_HdrShaderGpuState == EShaderGpuState::Ready;
}

void ALegacyScene3DAdapter::SkyCpuCompileWorkerEntry(
    void* user) noexcept {
    auto& runtime =
        *static_cast<ALegacyScene3DAdapter*>(user);
    auto result = CSky::CompileShadersCpu();
    const bool succeeded = result.IsOk();
    if (succeeded) {
        runtime.m_SkyPendingShaders = Move(result.Value());
    } else {
        ACS_LOG_WARN(
            "LegacyScene3DAdapter: sky CPU shader compilation failed; "
            "the stable HDR clear remains active: %s",
            result.Error().message);
    }
    runtime.m_SkyCompileWorkerState.store(
        succeeded ? 2 : -1, std::memory_order_release);
}

void ALegacyScene3DAdapter::WaterCpuCompileWorkerEntry(
    void* user) noexcept {
    auto& runtime =
        *static_cast<ALegacyScene3DAdapter*>(user);
    auto result = CWaterSurface3D::CompileShadersCpu();
    const bool succeeded = result.IsOk();
    if (succeeded) {
        runtime.m_WaterPendingShaders = Move(result.Value());
    } else {
        ACS_LOG_WARN(
            "LegacyScene3DAdapter: interactive-water CPU shader compilation "
            "failed; opaque PBR fallback remains active: %s",
            result.Error().message);
    }
    runtime.m_WaterCompileWorkerState.store(
        succeeded ? 2 : -1, std::memory_order_release);
}

void ALegacyScene3DAdapter::HdrPbrCpuCompileWorkerEntry(
    void* user) noexcept {
    auto& runtime =
        *static_cast<ALegacyScene3DAdapter*>(user);
    bool succeeded = false;
    auto shaders = CPbrShader::CompileShadersCpu(false);
    if (shaders.IsErr()) {
        ACS_LOG_WARN(
            "LegacyScene3DAdapter: HDR PBR CPU shader compilation failed; "
            "stable clear remains active: %s",
            shaders.Error().message);
    } else if (runtime.m_HdrCompileDevice == nullptr) {
        ACS_LOG_WARN(
            "LegacyScene3DAdapter: HDR PBR background candidate lost its "
            "raw-DX12 device; stable clear remains active");
    } else {
        auto initialized =
            runtime.m_HdrShaders[runtime.m_HdrPendingSlot]
                .BuildInitializedCandidateForRawDx12(
                    *runtime.m_HdrCompileDevice,
                    Move(shaders.Value()),
                    runtime.m_HdrCompileRtFormat,
                    runtime.m_HdrCompileDepthFormat);
        succeeded = initialized.IsOk();
        if (!succeeded) {
            ACS_LOG_WARN(
                "LegacyScene3DAdapter: HDR PBR background candidate "
                "creation failed; stable clear remains active: %s",
                initialized.Error().message);
        }
    }
    runtime.m_HdrPendingIsInitialized = succeeded;
    runtime.m_HdrCompileWorkerState.store(
        succeeded ? 2 : -1, std::memory_order_release);
}

void ALegacyScene3DAdapter::HdrSsssCpuCompileWorkerEntry(
    void* user) noexcept {
    auto& runtime =
        *static_cast<ALegacyScene3DAdapter*>(user);
    bool succeeded = false;
    auto shaders = CPbrShader::CompileShadersCpu(true);
    if (shaders.IsErr()) {
        ACS_LOG_WARN(
            "LegacyScene3DAdapter: SSSS PBR CPU shader compilation "
            "failed; analytic SSS remains active: %s",
            shaders.Error().message);
    } else if (runtime.m_HdrSsssCompileDevice == nullptr) {
        ACS_LOG_WARN(
            "LegacyScene3DAdapter: SSSS PBR background candidate lost "
            "its raw-DX12 device; analytic SSS remains active");
    } else {
        auto initialized =
            runtime.m_HdrShaders[runtime.m_HdrSsssPendingSlot]
                .BuildInitializedCandidateForRawDx12(
                    *runtime.m_HdrSsssCompileDevice,
                    Move(shaders.Value()),
                    runtime.m_HdrSsssCompileRtFormat,
                    runtime.m_HdrSsssCompileDepthFormat);
        succeeded = initialized.IsOk()
            && runtime.m_HdrShaders[runtime.m_HdrSsssPendingSlot]
                   .HasSubsurfaceMrtPipeline();
        if (!succeeded) {
            ACS_LOG_WARN(
                "LegacyScene3DAdapter: SSSS PBR background candidate "
                "creation failed; analytic SSS remains active%s%s",
                initialized.IsErr() ? ": " : "",
                initialized.IsErr() ? initialized.Error().message : "");
        }
    }
    runtime.m_HdrSsssPendingIsInitialized = succeeded;
    runtime.m_HdrSsssCompileWorkerState.store(
        succeeded ? 2 : -1, std::memory_order_release);
}

void ALegacyScene3DAdapter::SubsurfaceCpuCompileWorkerEntry(
    void* user) noexcept {
    auto& runtime =
        *static_cast<ALegacyScene3DAdapter*>(user);
    auto shaders = CSubsurfaceScattering::CompileShadersCpu();
    bool succeeded = false;
    if (shaders.IsErr()) {
        ACS_LOG_WARN(
            "LegacyScene3DAdapter: SSSS CPU shader compilation failed; "
            "analytic SSS remains active: %s",
            shaders.Error().message);
    } else if (runtime.m_SsssCompileDevice == nullptr) {
        ACS_LOG_WARN(
            "LegacyScene3DAdapter: SSSS background candidate has no "
            "raw-DX12 device; analytic SSS remains active");
    } else {
        auto initialized =
            runtime.m_Ssss.BuildPipelineCandidateForRawDx12(
                *runtime.m_SsssCompileDevice,
                Move(shaders.Value()));
        succeeded = initialized.IsOk()
            && runtime.m_Ssss.HasPipelineResources();
        if (!succeeded) {
            ACS_LOG_WARN(
                "LegacyScene3DAdapter: SSSS background pipeline "
                "candidate creation failed; analytic SSS remains "
                "active%s%s",
                initialized.IsErr() ? ": " : "",
                initialized.IsErr()
                    ? initialized.Error().message : "");
        }
    }
    runtime.m_SsssPendingIsInitialized = succeeded;
    runtime.m_SsssCompileWorkerState.store(
        succeeded ? 2 : -1, std::memory_order_release);
}

void ALegacyScene3DAdapter::PostCpuCompileWorkerEntry(
    void* user) noexcept {
    auto& runtime =
        *static_cast<ALegacyScene3DAdapter*>(user);
    auto result = CPostProcess::CompileShadersCpu();
    const bool succeeded = result.IsOk();
    if (succeeded) {
        runtime.m_PostPendingShaders = Move(result.Value());
    } else {
        ACS_LOG_WARN(
            "LegacyScene3DAdapter: post CPU shader compilation failed; "
            "stable clear remains active: %s",
            result.Error().message);
    }
    runtime.m_PostCompileWorkerState.store(
        succeeded ? 2 : -1, std::memory_order_release);
}

void ALegacyScene3DAdapter::BlitCpuCompileWorkerEntry(
    void* user) noexcept {
    auto& runtime =
        *static_cast<ALegacyScene3DAdapter*>(user);
    auto result = CBlit::CompileShadersCpu();
    const bool succeeded = result.IsOk();
    if (succeeded) {
        runtime.m_BlitPendingShaders = Move(result.Value());
    } else {
        ACS_LOG_WARN(
            "LegacyScene3DAdapter: blit CPU shader compilation failed; "
            "dependent effects stay on their analytic fallback: %s",
            result.Error().message);
    }
    runtime.m_BlitCompileWorkerState.store(
        succeeded ? 2 : -1, std::memory_order_release);
}

void ALegacyScene3DAdapter::SpriteCpuCompileWorkerEntry(void* user) noexcept
{
    auto& runtime = *static_cast<ALegacyScene3DAdapter*>(user);
    auto result = CSprite3DRenderer::CompileShadersCpu();
    const bool succeeded = result.IsOk();
    if (succeeded) {
        runtime.m_SpritePendingShaders = Move(result.Value());
    } else {
        ACS_LOG_WARN("LegacyScene3DAdapter: 3D sprite CPU shader compilation failed: %s", result.Error().message);
    }
    runtime.m_SpriteCompileWorkerState.store(succeeded ? 2 : -1, std::memory_order_release);
}

bool ALegacyScene3DAdapter::BeginSkyCpuCompilation() noexcept {
    if (m_SkyCompileWorker.Joinable()
        || m_SkyCompileWorkerState.load(
               std::memory_order_acquire) != 0) {
        ACS_LOG_WARN(
            "LegacyScene3DAdapter: sky CPU compile worker is already active; "
            "the stable HDR clear remains active");
        return false;
    }

    m_SkyCompileWorkerState.store(1, std::memory_order_release);
    FThreadConfig config{};
    config.name = L"ACS runtime sky compile";
    auto worker = FThread::Spawn(
        &ALegacyScene3DAdapter::SkyCpuCompileWorkerEntry,
        this, config);
    if (worker.IsErr()) {
        m_SkyCompileWorkerState.store(0, std::memory_order_release);
        ACS_LOG_WARN(
            "LegacyScene3DAdapter: failed to create sky CPU compile worker; "
            "the stable HDR clear remains active: %s",
            worker.Error().message);
        return false;
    }
    m_SkyCompileWorker = Move(worker.Value());
    m_SkyGpuState = ESkyGpuState::CpuCompiling;
    ACS_LOG_INFO(
        "LegacyScene3DAdapter: raw-DX12 sky shader compilation dispatched "
        "to a CPU worker");
    return true;
}

bool ALegacyScene3DAdapter::BeginWaterCpuCompilation() noexcept {
    if (m_WaterCompileWorker.Joinable()
        || m_WaterCompileWorkerState.load(
               std::memory_order_acquire) != 0) {
        ACS_LOG_WARN(
            "LegacyScene3DAdapter: interactive-water CPU compile worker is "
            "already active; opaque PBR fallback remains active");
        return false;
    }

    m_WaterCompileWorkerState.store(1, std::memory_order_release);
    FThreadConfig config{};
    config.name = L"ACS runtime water compile";
    auto worker = FThread::Spawn(
        &ALegacyScene3DAdapter::WaterCpuCompileWorkerEntry,
        this, config);
    if (worker.IsErr()) {
        m_WaterCompileWorkerState.store(0, std::memory_order_release);
        ACS_LOG_WARN(
            "LegacyScene3DAdapter: failed to create interactive-water CPU "
            "compile worker; opaque PBR fallback remains active: %s",
            worker.Error().message);
        return false;
    }
    m_WaterCompileWorker = Move(worker.Value());
    m_WaterGpuState = EWaterGpuState::CpuCompiling;
    ACS_LOG_INFO(
        "LegacyScene3DAdapter: raw-DX12 interactive-water shader compilation "
        "dispatched to a CPU worker");
    return true;
}

bool ALegacyScene3DAdapter::BeginHdrPbrCpuCompilation(
    IRhiDevice& device,
    EFormat rt_format,
    EFormat depth_format) noexcept {
    if (m_HdrCompileWorker.Joinable()
        || m_HdrCompileWorkerState.load(
               std::memory_order_acquire) != 0) {
        return false;
    }
    m_HdrCompileDevice = &device;
    m_HdrCompileRtFormat = rt_format;
    m_HdrCompileDepthFormat = depth_format;
    m_HdrPendingIsInitialized = false;
    m_HdrCompileWorkerState.store(1, std::memory_order_release);
    FThreadConfig config{};
    config.name = L"ACS runtime HDR PBR compile";
    auto worker = FThread::Spawn(
        &ALegacyScene3DAdapter::HdrPbrCpuCompileWorkerEntry,
        this, config);
    if (worker.IsErr()) {
        m_HdrCompileWorkerState.store(0, std::memory_order_release);
        m_HdrCompileDevice = nullptr;
        ACS_LOG_WARN(
            "LegacyScene3DAdapter: failed to create HDR PBR CPU compile "
            "worker; stable clear remains active: %s",
            worker.Error().message);
        return false;
    }
    m_HdrCompileWorker = Move(worker.Value());
    m_HdrShaderGpuState = EShaderGpuState::CpuCompiling;
    ACS_LOG_INFO(
        "LegacyScene3DAdapter: raw-DX12 base-only HDR PBR shader and "
        "complete RHI candidate dispatched to a worker");
    return true;
}

bool ALegacyScene3DAdapter::BeginHdrSsssCpuCompilation(
    IRhiDevice& device,
    EFormat rt_format,
    EFormat depth_format) noexcept {
    if (m_HdrSsssCompileWorker.Joinable()
        || m_HdrSsssCompileWorkerState.load(
               std::memory_order_acquire) != 0) {
        return false;
    }
    m_HdrSsssCompileDevice = &device;
    m_HdrSsssCompileRtFormat = rt_format;
    m_HdrSsssCompileDepthFormat = depth_format;
    m_HdrSsssPendingIsInitialized = false;
    m_HdrSsssCompileWorkerState.store(1, std::memory_order_release);
    FThreadConfig config{};
    config.name = L"ACS runtime SSSS PBR compile";
    auto worker = FThread::Spawn(
        &ALegacyScene3DAdapter::HdrSsssCpuCompileWorkerEntry,
        this, config);
    if (worker.IsErr()) {
        m_HdrSsssCompileWorkerState.store(
            0, std::memory_order_release);
        m_HdrSsssCompileDevice = nullptr;
        ACS_LOG_WARN(
            "LegacyScene3DAdapter: failed to create SSSS PBR CPU "
            "compile worker; analytic SSS remains active: %s",
            worker.Error().message);
        return false;
    }
    m_HdrSsssCompileWorker = Move(worker.Value());
    m_HdrSsssGpuState = EShaderGpuState::CpuCompiling;
    ACS_LOG_INFO(
        "LegacyScene3DAdapter: optional four-target SSSS PBR candidate "
        "dispatched after the base renderer became ready");
    return true;
}

bool ALegacyScene3DAdapter::BeginSubsurfaceCpuCompilation(
    IRhiDevice& device) noexcept {
    if (m_SsssCompileWorker.Joinable()
        || m_SsssCompileWorkerState.load(
               std::memory_order_acquire) != 0) {
        return false;
    }
    m_SsssCompileDevice = &device;
    m_SsssPendingIsInitialized = false;
    m_SsssCompileWorkerState.store(1, std::memory_order_release);
    FThreadConfig config{};
    config.name = L"ACS runtime SSSS compile";
    auto worker = FThread::Spawn(
        &ALegacyScene3DAdapter::SubsurfaceCpuCompileWorkerEntry,
        this, config);
    if (worker.IsErr()) {
        m_SsssCompileWorkerState.store(0, std::memory_order_release);
        m_SsssCompileDevice = nullptr;
        ACS_LOG_WARN(
            "LegacyScene3DAdapter: failed to create SSSS CPU compile "
            "worker; analytic SSS remains active: %s",
            worker.Error().message);
        return false;
    }
    m_SsssCompileWorker = Move(worker.Value());
    m_SsssGpuState = EShaderGpuState::CpuCompiling;
    ACS_LOG_INFO(
        "LegacyScene3DAdapter: SSSS compositor shaders dispatched to a "
        "CPU worker");
    return true;
}

bool ALegacyScene3DAdapter::BeginPostCpuCompilation() noexcept {
    if (m_PostCompileWorker.Joinable()
        || m_PostCompileWorkerState.load(
               std::memory_order_acquire) != 0) {
        return false;
    }
    m_PostCompileWorkerState.store(1, std::memory_order_release);
    FThreadConfig config{};
    config.name = L"ACS runtime post compile";
    auto worker = FThread::Spawn(
        &ALegacyScene3DAdapter::PostCpuCompileWorkerEntry,
        this, config);
    if (worker.IsErr()) {
        m_PostCompileWorkerState.store(0, std::memory_order_release);
        ACS_LOG_WARN(
            "LegacyScene3DAdapter: failed to create post CPU compile worker; "
            "stable clear remains active: %s",
            worker.Error().message);
        return false;
    }
    m_PostCompileWorker = Move(worker.Value());
    m_PostGpuState = EShaderGpuState::CpuCompiling;
    ACS_LOG_INFO(
        "LegacyScene3DAdapter: raw-DX12 post shader compilation dispatched "
        "to a CPU worker");
    return true;
}

bool ALegacyScene3DAdapter::BeginBlitCpuCompilation() noexcept {
    if (m_BlitCompileWorker.Joinable()
        || m_BlitCompileWorkerState.load(
               std::memory_order_acquire) != 0) {
        return false;
    }
    m_BlitCompileWorkerState.store(1, std::memory_order_release);
    FThreadConfig config{};
    config.name = L"ACS runtime blit compile";
    auto worker = FThread::Spawn(
        &ALegacyScene3DAdapter::BlitCpuCompileWorkerEntry,
        this, config);
    if (worker.IsErr()) {
        m_BlitCompileWorkerState.store(0, std::memory_order_release);
        ACS_LOG_WARN(
            "LegacyScene3DAdapter: failed to create blit CPU compile worker; "
            "dependent effects stay on their analytic fallback: %s",
            worker.Error().message);
        return false;
    }
    m_BlitCompileWorker = Move(worker.Value());
    m_BlitGpuState = EShaderGpuState::CpuCompiling;
    ACS_LOG_INFO(
        "LegacyScene3DAdapter: raw-DX12 blit shader compilation dispatched "
        "to a CPU worker");
    return true;
}

bool ALegacyScene3DAdapter::BeginSpriteCpuCompilation() noexcept
{
    if (m_SpriteCompileWorker.Joinable() || m_SpriteCompileWorkerState.load(std::memory_order_acquire) != 0) {
        return false;
    }
    m_SpriteCompileWorkerState.store(1, std::memory_order_release);
    FThreadConfig config{};
    config.name = L"ACS runtime 3D sprite compile";
    auto worker = FThread::Spawn(&ALegacyScene3DAdapter::SpriteCpuCompileWorkerEntry, this, config);
    if (worker.IsErr()) {
        m_SpriteCompileWorkerState.store(0, std::memory_order_release);
        ACS_LOG_WARN("LegacyScene3DAdapter: failed to create 3D sprite CPU compile worker: %s", worker.Error().message);
        return false;
    }
    m_SpriteCompileWorker = Move(worker.Value());
    m_SpriteGpuState = EShaderGpuState::CpuCompiling;
    ACS_LOG_INFO("LegacyScene3DAdapter: raw-DX12 3D sprite shader compilation dispatched to a CPU worker");
    return true;
}

bool ALegacyScene3DAdapter::TryClaimGpuCommit(
    EGpuCommitSubsystem& frame_commit,
    EGpuCommitSubsystem subsystem) noexcept {
    if (frame_commit != EGpuCommitSubsystem::None) return false;
    frame_commit = subsystem;
    return true;
}

void ALegacyScene3DAdapter::AdvanceHdrPbrInitialization(
    IRhiDevice& device,
    EGpuCommitSubsystem& frame_commit) noexcept {
    if (m_HdrShaderGpuState == EShaderGpuState::CpuCompiling) {
        const i32 worker_state = m_HdrCompileWorkerState.load(
            std::memory_order_acquire);
        if (worker_state == 1) return;
        m_HdrCompileWorker.Join();
        m_HdrCompileDevice = nullptr;
        m_HdrCompileWorkerState.store(0, std::memory_order_release);
        if (worker_state != 2 || !m_HdrPendingIsInitialized) {
            m_HdrPendingShaders = {};
            m_HdrPendingIsInitialized = false;
            m_HdrShaderGpuState = EShaderGpuState::Failed;
            return;
        }
        m_HdrShaderGpuState = EShaderGpuState::PendingCommit;
    } else if (m_HdrShaderGpuState == EShaderGpuState::Compiling) {
        const EShaderStatus status = m_HdrPendingShaders.Status();
        if (status == EShaderStatus::Compiling) return;
        if (status != EShaderStatus::Ready) {
            m_HdrPendingShaders = {};
            m_HdrPendingIsInitialized = false;
            m_HdrShaderGpuState = EShaderGpuState::Failed;
            ACS_LOG_WARN(
                "LegacyScene3DAdapter: asynchronous HDR PBR compilation "
                "failed; stable clear remains active");
            return;
        }
        m_HdrShaderGpuState = EShaderGpuState::PendingCommit;
    }
    if (m_HdrShaderGpuState != EShaderGpuState::PendingCommit) {
        return;
    }
    if (!TryClaimGpuCommit(
            frame_commit, EGpuCommitSubsystem::HdrPbr)) {
        return;
    }

    if (m_HdrPendingIsInitialized) {
        m_HdrActiveSlot = m_HdrPendingSlot;
        m_HdrPendingIsInitialized = false;
        m_HdrShaderGpuState = EShaderGpuState::Ready;
        ACS_LOG_INFO(
            "LegacyScene3DAdapter: background-built base-only HDR PBR "
            "candidate published without owner-thread RHI creation");
        return;
    }

    CPbrShader& candidate = m_HdrShaders[m_HdrPendingSlot];
    const auto result = candidate.InitWithCompiledShaders(
        device, Move(m_HdrPendingShaders),
        m_Post.HdrFormat(), m_FrameDepthFormat);
    if (result.IsErr()) {
        m_HdrPendingShaders = {};
        m_HdrShaderGpuState = EShaderGpuState::Failed;
        ACS_LOG_WARN(
            "LegacyScene3DAdapter: HDR PBR RHI commit failed; "
            "stable clear remains active: %s",
            result.Error().message);
        return;
    }
    m_HdrActiveSlot = m_HdrPendingSlot;
    m_HdrShaderGpuState = EShaderGpuState::Ready;
    ACS_LOG_INFO(
        "LegacyScene3DAdapter: HDR PBR renderer is ready");
}

void ALegacyScene3DAdapter::AdvanceHdrSsssInitialization(
    IRhiDevice& device,
    EGpuCommitSubsystem& frame_commit,
    bool scene_needs_subsurface) noexcept {
    // Retire completed optional work before the live feature gate. Materials
    // may be removed while compilation is in flight; an unpublished worker
    // candidate must not remain joinable or retain resources indefinitely.
    if (m_HdrSsssGpuState == EShaderGpuState::CpuCompiling) {
        const i32 worker_state = m_HdrSsssCompileWorkerState.load(
            std::memory_order_acquire);
        if (worker_state != 1) {
            m_HdrSsssCompileWorker.Join();
            m_HdrSsssCompileDevice = nullptr;
            m_HdrSsssCompileWorkerState.store(
                0, std::memory_order_release);
            if (worker_state == 2
                && m_HdrSsssPendingIsInitialized) {
                m_HdrSsssGpuState = EShaderGpuState::PendingCommit;
            } else {
                m_HdrShaders[m_HdrSsssPendingSlot].Shutdown();
                m_HdrSsssPendingShaders = {};
                m_HdrSsssPendingIsInitialized = false;
                m_HdrSsssGpuState = EShaderGpuState::Failed;
            }
        }
    } else if (m_HdrSsssGpuState == EShaderGpuState::Compiling) {
        const EShaderStatus status = m_HdrSsssPendingShaders.Status();
        if (status == EShaderStatus::Ready) {
            m_HdrSsssGpuState = EShaderGpuState::PendingCommit;
        } else if (status == EShaderStatus::Failed) {
            m_HdrSsssPendingShaders = {};
            m_HdrSsssGpuState = EShaderGpuState::Failed;
        }
    }

    if (!scene_needs_subsurface) {
        if (m_HdrSsssGpuState == EShaderGpuState::PendingCommit
            || m_HdrSsssGpuState == EShaderGpuState::Failed) {
            m_HdrSsssPendingShaders = {};
            m_HdrSsssPendingIsInitialized = false;
            if (m_HdrSsssPendingSlot != m_HdrActiveSlot)
                m_HdrShaders[m_HdrSsssPendingSlot].Shutdown();
            m_HdrSsssGpuState = EShaderGpuState::Unavailable;
        }
        return;
    }
    if (m_HdrShaderGpuState != EShaderGpuState::Ready) {
        return;
    }

    if (m_HdrSsssGpuState == EShaderGpuState::Unavailable) {
        m_HdrSsssPendingSlot =
            static_cast<u8>(m_HdrActiveSlot ^ 1u);
        m_HdrShaders[m_HdrSsssPendingSlot].Shutdown();
        m_HdrSsssPendingIsInitialized = false;
        const char* const backend_name = device.BackendName();
        const bool raw_dx12 = backend_name != nullptr
            && std::strcmp(backend_name, "DX12") == 0;
        if (device.SupportsAsyncShaderCompilation()) {
            auto result =
                CPbrShader::BeginCompileShadersAsync(device, true);
            if (result.IsErr()) {
                m_HdrSsssGpuState = EShaderGpuState::Failed;
                ACS_LOG_WARN(
                    "LegacyScene3DAdapter: asynchronous SSSS PBR "
                    "submission failed; analytic SSS remains active: %s",
                    result.Error().message);
            } else {
                m_HdrSsssPendingShaders = Move(result.Value());
                m_HdrSsssGpuState = EShaderGpuState::Compiling;
            }
        } else if (raw_dx12) {
            if (!BeginHdrSsssCpuCompilation(
                    device, m_Post.HdrFormat(), m_FrameDepthFormat)) {
                m_HdrSsssGpuState = EShaderGpuState::Failed;
            }
        } else {
            m_HdrSsssGpuState = EShaderGpuState::Failed;
            ACS_LOG_WARN(
                "LegacyScene3DAdapter: SSSS PBR compilation is "
                "unsupported by backend %s; analytic SSS remains active",
                backend_name ? backend_name : "(unknown)");
        }
    }

    if (m_HdrSsssGpuState == EShaderGpuState::CpuCompiling) {
        const i32 worker_state = m_HdrSsssCompileWorkerState.load(
            std::memory_order_acquire);
        if (worker_state == 1) return;
        m_HdrSsssCompileWorker.Join();
        m_HdrSsssCompileDevice = nullptr;
        m_HdrSsssCompileWorkerState.store(
            0, std::memory_order_release);
        if (worker_state != 2 || !m_HdrSsssPendingIsInitialized) {
            m_HdrShaders[m_HdrSsssPendingSlot].Shutdown();
            m_HdrSsssPendingShaders = {};
            m_HdrSsssPendingIsInitialized = false;
            m_HdrSsssGpuState = EShaderGpuState::Failed;
            return;
        }
        m_HdrSsssGpuState = EShaderGpuState::PendingCommit;
    } else if (m_HdrSsssGpuState == EShaderGpuState::Compiling) {
        const EShaderStatus status = m_HdrSsssPendingShaders.Status();
        if (status == EShaderStatus::Compiling) return;
        if (status != EShaderStatus::Ready) {
            m_HdrSsssPendingShaders = {};
            m_HdrSsssGpuState = EShaderGpuState::Failed;
            ACS_LOG_WARN(
                "LegacyScene3DAdapter: asynchronous SSSS PBR "
                "compilation failed; analytic SSS remains active");
            return;
        }
        m_HdrSsssGpuState = EShaderGpuState::PendingCommit;
    }
    if (m_HdrSsssGpuState != EShaderGpuState::PendingCommit
        || !TryClaimGpuCommit(
            frame_commit, EGpuCommitSubsystem::HdrSsss)) {
        return;
    }

    if (m_HdrSsssPendingIsInitialized) {
        m_HdrActiveSlot = m_HdrSsssPendingSlot;
        m_HdrSsssPendingIsInitialized = false;
        m_HdrSsssGpuState = EShaderGpuState::Ready;
        ACS_LOG_INFO(
            "LegacyScene3DAdapter: background-built four-target SSSS PBR "
            "candidate published without owner-thread RHI creation");
        return;
    }

    CPbrShader& candidate = m_HdrShaders[m_HdrSsssPendingSlot];
    const auto result = candidate.InitWithCompiledShaders(
        device, Move(m_HdrSsssPendingShaders),
        m_Post.HdrFormat(), m_FrameDepthFormat);
    if (result.IsErr() || !candidate.HasSubsurfaceMrtPipeline()) {
        candidate.Shutdown();
        m_HdrSsssPendingShaders = {};
        m_HdrSsssGpuState = EShaderGpuState::Failed;
        ACS_LOG_WARN(
            "LegacyScene3DAdapter: SSSS PBR RHI commit failed; "
            "the base renderer and analytic SSS remain active%s%s",
            result.IsErr() ? ": " : "",
            result.IsErr() ? result.Error().message : "");
        return;
    }
    m_HdrActiveSlot = m_HdrSsssPendingSlot;
    m_HdrSsssGpuState = EShaderGpuState::Ready;
    ACS_LOG_INFO(
        "LegacyScene3DAdapter: optional four-target SSSS PBR renderer "
        "is ready");
}

void ALegacyScene3DAdapter::AdvanceSubsurfaceInitialization(
    IRhiDevice& device,
    u32 width,
    u32 height,
    EGpuCommitSubsystem& frame_commit,
    bool scene_needs_subsurface) noexcept {
    // Drain completed optional work even when a hot material edit removes the
    // final SSS dependency during compilation.
    if (m_SsssGpuState == EShaderGpuState::CpuCompiling) {
        const i32 worker_state = m_SsssCompileWorkerState.load(
            std::memory_order_acquire);
        if (worker_state != 1) {
            m_SsssCompileWorker.Join();
            m_SsssCompileDevice = nullptr;
            m_SsssCompileWorkerState.store(
                0, std::memory_order_release);
            if (worker_state == 2
                && m_SsssPendingIsInitialized
                && m_Ssss.HasPipelineResources()) {
                m_SsssGpuState = EShaderGpuState::PendingCommit;
            } else {
                m_Ssss.Shutdown();
                m_SsssPendingShaders = {};
                m_SsssPendingIsInitialized = false;
                m_SsssGpuState = EShaderGpuState::Failed;
            }
        }
    } else if (m_SsssGpuState == EShaderGpuState::Compiling) {
        const EShaderStatus status = m_SsssPendingShaders.Status();
        if (status == EShaderStatus::Ready) {
            m_SsssGpuState = EShaderGpuState::PendingCommit;
        } else if (status == EShaderStatus::Failed) {
            m_SsssPendingShaders = {};
            m_SsssGpuState = EShaderGpuState::Failed;
        }
    }

    if (!scene_needs_subsurface) {
        if (m_SsssGpuState == EShaderGpuState::PendingCommit
            || m_SsssGpuState == EShaderGpuState::Failed) {
            m_SsssPendingShaders = {};
            m_SsssPendingIsInitialized = false;
            m_Ssss.Shutdown();
            m_SsssGpuState = EShaderGpuState::Unavailable;
        }
        return;
    }
    if (m_HdrSsssGpuState != EShaderGpuState::Ready
        || width == 0u || height == 0u) {
        return;
    }

    if (m_SsssGpuState == EShaderGpuState::Unavailable) {
        const char* const backend_name = device.BackendName();
        const bool raw_dx12 = backend_name != nullptr
            && std::strcmp(backend_name, "DX12") == 0;
        if (device.SupportsAsyncShaderCompilation()) {
            auto result =
                CSubsurfaceScattering::BeginCompileShadersAsync(device);
            if (result.IsErr()) {
                m_SsssGpuState = EShaderGpuState::Failed;
                ACS_LOG_WARN(
                    "LegacyScene3DAdapter: asynchronous SSSS submission "
                    "failed; analytic SSS remains active: %s",
                    result.Error().message);
            } else {
                m_SsssPendingShaders = Move(result.Value());
                m_SsssGpuState = EShaderGpuState::Compiling;
            }
        } else if (raw_dx12) {
            if (!BeginSubsurfaceCpuCompilation(device))
                m_SsssGpuState = EShaderGpuState::Failed;
        } else {
            m_SsssGpuState = EShaderGpuState::Failed;
            ACS_LOG_WARN(
                "LegacyScene3DAdapter: SSSS compilation is unsupported "
                "by backend %s; analytic SSS remains active",
                backend_name ? backend_name : "(unknown)");
        }
    }

    if (m_SsssGpuState == EShaderGpuState::CpuCompiling) {
        const i32 worker_state = m_SsssCompileWorkerState.load(
            std::memory_order_acquire);
        if (worker_state == 1) return;
        m_SsssCompileWorker.Join();
        m_SsssCompileDevice = nullptr;
        m_SsssCompileWorkerState.store(0, std::memory_order_release);
        if (worker_state != 2) {
            m_Ssss.Shutdown();
            m_SsssPendingShaders = {};
            m_SsssGpuState = EShaderGpuState::Failed;
            return;
        }
        m_SsssGpuState = EShaderGpuState::PendingCommit;
    } else if (m_SsssGpuState == EShaderGpuState::Compiling) {
        const EShaderStatus status = m_SsssPendingShaders.Status();
        if (status == EShaderStatus::Compiling) return;
        if (status != EShaderStatus::Ready) {
            m_SsssPendingShaders = {};
            m_SsssGpuState = EShaderGpuState::Failed;
            ACS_LOG_WARN(
                "LegacyScene3DAdapter: asynchronous SSSS compilation "
                "failed; analytic SSS remains active");
            return;
        }
        m_SsssGpuState = EShaderGpuState::PendingCommit;
    }
    if (m_SsssGpuState != EShaderGpuState::PendingCommit
        || !TryClaimGpuCommit(
            frame_commit, EGpuCommitSubsystem::Subsurface)) {
        return;
    }

    if (m_SsssPendingIsInitialized) {
        m_SsssPendingIsInitialized = false;
        m_SsssGpuState = EShaderGpuState::Ready;
        m_SsssResizeAttemptWidth = 0u;
        m_SsssResizeAttemptHeight = 0u;
        ACS_LOG_INFO(
            "LegacyScene3DAdapter: background-built SSSS pipeline "
            "candidate published without owner-thread RHI creation");
        return;
    }

    const auto result =
        m_Ssss.InitPipelineResourcesWithCompiledShaders(
            device, Move(m_SsssPendingShaders));
    if (result.IsErr()) {
        m_SsssPendingShaders = {};
        m_SsssGpuState = EShaderGpuState::Failed;
        ACS_LOG_WARN(
            "LegacyScene3DAdapter: SSSS RHI commit failed; "
            "analytic SSS remains active: %s",
            result.Error().message);
        return;
    }
    m_SsssGpuState = EShaderGpuState::Ready;
    m_SsssResizeAttemptWidth = 0u;
    m_SsssResizeAttemptHeight = 0u;
    ACS_LOG_INFO(
        "LegacyScene3DAdapter: diffuse-only bilateral SSSS pipelines "
        "are ready; full-resolution targets remain staged");
}

void ALegacyScene3DAdapter::EnsureSubsurfaceAuxTargets(
    IRhiDevice& device,
    u32 width,
    u32 height,
    EGpuCommitSubsystem& frame_commit,
    bool scene_needs_subsurface) noexcept {
    if (!scene_needs_subsurface
        || m_HdrSsssGpuState != EShaderGpuState::Ready
        || m_SsssGpuState != EShaderGpuState::Ready
        || m_BlitGpuState != EShaderGpuState::Ready
        || width == 0u || height == 0u) {
        return;
    }
    const bool active_matches =
        m_Ssss.Width() == width && m_Ssss.Height() == height
        && m_SsssDiffuse && m_SsssMaterial && m_SsssNormal
        && m_SsssDiffuse->Width() == width
        && m_SsssDiffuse->Height() == height
        && m_SsssMaterial->Width() == width
        && m_SsssMaterial->Height() == height
        && m_SsssNormal->Width() == width
        && m_SsssNormal->Height() == height;
    if (active_matches) return;

    // Pipeline publication, the compositor's internal pair and the external
    // MRT bundle are deliberately three different frame commits. Raw DX12
    // builds the first stage entirely on its worker.
    if (!m_Ssss.OutputTexture()
        || !m_Ssss.HorizontalTexture()
        || m_Ssss.Width() == 0u || m_Ssss.Height() == 0u) {
        if ((m_SsssResizeAttemptWidth == width
             && m_SsssResizeAttemptHeight == height)
            || !TryClaimGpuCommit(
                frame_commit, EGpuCommitSubsystem::Subsurface)) {
            return;
        }
        m_SsssResizeAttemptWidth = width;
        m_SsssResizeAttemptHeight = height;
        const auto initial_targets = m_Ssss.Resize(width, height);
        if (initial_targets.IsErr()) {
            ACS_LOG_WARN(
                "LegacyScene3DAdapter: initial SSSS internal target "
                "allocation failed at %ux%u; analytic SSS remains "
                "active: %s",
                width, height, initial_targets.Error().message);
        }
        return;
    }

    const bool pending_matches =
        m_SsssPendingDiffuse && m_SsssPendingMaterial
        && m_SsssPendingNormal
        && m_SsssPendingAuxWidth == width
        && m_SsssPendingAuxHeight == height;
    if (!pending_matches) {
        if ((m_SsssAuxAttemptWidth == width
             && m_SsssAuxAttemptHeight == height)
            || !TryClaimGpuCommit(
                frame_commit, EGpuCommitSubsystem::Subsurface)) {
            return;
        }
        m_SsssAuxAttemptWidth = width;
        m_SsssAuxAttemptHeight = height;
        m_SsssPendingDiffuse.Reset();
        m_SsssPendingMaterial.Reset();
        m_SsssPendingNormal.Reset();
        m_SsssPendingAuxWidth = 0u;
        m_SsssPendingAuxHeight = 0u;

        FTextureDesc description{};
        description.width = width;
        description.height = height;
        description.format = EFormat::R16G16B16A16_Float;
        description.is_render_target = true;
        auto diffuse = CreateRhiTexture(device, description);
        auto material = CreateRhiTexture(device, description);
        auto normal = CreateRhiTexture(device, description);
        if (diffuse.IsErr() || material.IsErr() || normal.IsErr()) {
            const char* message = diffuse.IsErr()
                ? diffuse.Error().message
                : (material.IsErr()
                    ? material.Error().message : normal.Error().message);
            ACS_LOG_WARN(
                "LegacyScene3DAdapter: SSSS MRT bundle allocation "
                "failed at %ux%u; analytic SSS remains active: %s",
                width, height, message);
            return;
        }
        m_SsssPendingDiffuse = Move(diffuse.Value());
        m_SsssPendingMaterial = Move(material.Value());
        m_SsssPendingNormal = Move(normal.Value());
        m_SsssPendingAuxWidth = width;
        m_SsssPendingAuxHeight = height;
        m_SsssResizeAttemptWidth = 0u;
        m_SsssResizeAttemptHeight = 0u;

        // Initial compositor creation already matches the viewport, so this
        // commit can publish the complete external bundle immediately.
        if (m_Ssss.Width() == width && m_Ssss.Height() == height) {
            m_SsssDiffuse = Move(m_SsssPendingDiffuse);
            m_SsssMaterial = Move(m_SsssPendingMaterial);
            m_SsssNormal = Move(m_SsssPendingNormal);
            m_SsssPendingAuxWidth = 0u;
            m_SsssPendingAuxHeight = 0u;
        }
        return;
    }

    // 全解像度の外部描画先3個を準備してから内部描画先2個のサイズを変更し、
    // 成功時だけ5個を公開する。途中で失敗した場合は公開済みの5個を変更しない。
    if (m_SsssResizeAttemptWidth == width
        && m_SsssResizeAttemptHeight == height) {
        return;
    }
    if (!TryClaimGpuCommit(
            frame_commit, EGpuCommitSubsystem::Subsurface)) {
        return;
    }
    m_SsssResizeAttemptWidth = width;
    m_SsssResizeAttemptHeight = height;
    const auto resize = m_Ssss.Resize(width, height);
    if (resize.IsErr()) {
        m_SsssPendingDiffuse.Reset();
        m_SsssPendingMaterial.Reset();
        m_SsssPendingNormal.Reset();
        m_SsssPendingAuxWidth = 0u;
        m_SsssPendingAuxHeight = 0u;
        ACS_LOG_WARN(
            "LegacyScene3DAdapter: SSSS resize failed at %ux%u; "
            "the previous complete stack remains valid and analytic SSS "
            "is used for this viewport: %s",
            width, height, resize.Error().message);
        return;
    }
    m_SsssDiffuse = Move(m_SsssPendingDiffuse);
    m_SsssMaterial = Move(m_SsssPendingMaterial);
    m_SsssNormal = Move(m_SsssPendingNormal);
    m_SsssPendingAuxWidth = 0u;
    m_SsssPendingAuxHeight = 0u;
}

void ALegacyScene3DAdapter::AdvancePostInitialization(
    IRhiDevice& device, u32 width, u32 height,
    EFormat swapchain_format,
    EGpuCommitSubsystem& frame_commit) noexcept {
    if (m_PostGpuState == EShaderGpuState::Ready) {
        if (m_FrameWidth == width && m_FrameHeight == height) return;
        if (!TryClaimGpuCommit(
                frame_commit, EGpuCommitSubsystem::Post)) {
            return;
        }
        const auto resize = m_Post.Resize(width, height);
        if (resize.IsErr()) {
            ACS_LOG_WARN(
                "LegacyScene3DAdapter: HDR resize failed at %ux%u; "
                "the previous targets remain valid and the stable clear "
                "remains active until a retry succeeds: %s",
                width, height, resize.Error().message);
            return;
        }
        m_FrameWidth = width;
        m_FrameHeight = height;
        return;
    }

    if (m_PostGpuState == EShaderGpuState::CpuCompiling) {
        const i32 worker_state = m_PostCompileWorkerState.load(
            std::memory_order_acquire);
        if (worker_state == 1) return;
        m_PostCompileWorker.Join();
        m_PostCompileWorkerState.store(0, std::memory_order_release);
        if (worker_state != 2) {
            m_PostPendingShaders = {};
            m_PostGpuState = EShaderGpuState::Failed;
            return;
        }
        m_PostGpuState = EShaderGpuState::PendingCommit;
    } else if (m_PostGpuState == EShaderGpuState::Compiling) {
        const EShaderStatus status = m_PostPendingShaders.Status();
        if (status == EShaderStatus::Compiling) return;
        if (status != EShaderStatus::Ready) {
            m_PostPendingShaders = {};
            m_PostGpuState = EShaderGpuState::Failed;
            ACS_LOG_WARN(
                "LegacyScene3DAdapter: asynchronous post compilation failed; "
                "stable clear remains active");
            return;
        }
        m_PostGpuState = EShaderGpuState::PendingCommit;
    }
    if (m_PostGpuState != EShaderGpuState::PendingCommit) {
        return;
    }
    if (!TryClaimGpuCommit(
            frame_commit, EGpuCommitSubsystem::Post)) {
        return;
    }

    const auto result = m_Post.InitWithCompiledShaders(
        device, Move(m_PostPendingShaders),
        width, height, swapchain_format);
    if (result.IsErr()) {
        m_PostPendingShaders = {};
        m_PostGpuState = EShaderGpuState::Failed;
        ACS_LOG_WARN(
            "LegacyScene3DAdapter: post RHI commit failed; "
            "stable clear remains active: %s",
            result.Error().message);
        return;
    }
    m_PostGpuState = EShaderGpuState::Ready;
    m_FrameWidth = width;
    m_FrameHeight = height;
    ACS_LOG_INFO(
        "LegacyScene3DAdapter: HDR post renderer is ready");
}

void ALegacyScene3DAdapter::AdvanceBlitInitialization(
    IRhiDevice& device,
    EGpuCommitSubsystem& frame_commit,
    bool requested) noexcept {
    if (m_BlitGpuState == EShaderGpuState::CpuCompiling) {
        const i32 worker_state = m_BlitCompileWorkerState.load(
            std::memory_order_acquire);
        if (worker_state == 1) return;
        m_BlitCompileWorker.Join();
        m_BlitCompileWorkerState.store(0, std::memory_order_release);
        if (worker_state != 2) {
            m_BlitPendingShaders = {};
            m_BlitGpuState = EShaderGpuState::Failed;
            return;
        }
        m_BlitGpuState = EShaderGpuState::PendingCommit;
    } else if (m_BlitGpuState == EShaderGpuState::Compiling) {
        const EShaderStatus status = m_BlitPendingShaders.Status();
        if (status == EShaderStatus::Compiling) return;
        if (status != EShaderStatus::Ready) {
            m_BlitPendingShaders = {};
            m_BlitGpuState = EShaderGpuState::Failed;
            ACS_LOG_WARN(
                "LegacyScene3DAdapter: asynchronous blit compilation failed; "
                "dependent effects stay on their analytic fallback");
            return;
        }
        m_BlitGpuState = EShaderGpuState::PendingCommit;
    }
    if (m_BlitGpuState != EShaderGpuState::PendingCommit) {
        return;
    }
    if (!requested
        || !TryClaimGpuCommit(
            frame_commit, EGpuCommitSubsystem::Blit)) {
        return;
    }

    const auto result = m_Blit.InitWithCompiledShaders(
        device, Move(m_BlitPendingShaders), m_Post.HdrFormat());
    if (result.IsErr()) {
        m_BlitPendingShaders = {};
        m_BlitGpuState = EShaderGpuState::Failed;
        ACS_LOG_WARN(
            "LegacyScene3DAdapter: blit RHI commit failed; "
            "dependent effects stay on their analytic fallback: %s",
            result.Error().message);
        return;
    }
    m_BlitGpuState = EShaderGpuState::Ready;
    ACS_LOG_INFO(
        "LegacyScene3DAdapter: HDR blit renderer is ready");
}

void ALegacyScene3DAdapter::AdvanceSpriteInitialization(IRhiDevice& device, EFormat depth_format, EGpuCommitSubsystem& frame_commit, bool requested) noexcept
{
    if (m_SpriteGpuState == EShaderGpuState::CpuCompiling) {
        const i32 worker_state = m_SpriteCompileWorkerState.load(std::memory_order_acquire);
        if (worker_state == 1) return;
        m_SpriteCompileWorker.Join();
        m_SpriteCompileWorkerState.store(0, std::memory_order_release);
        if (worker_state != 2) {
            m_SpritePendingShaders = {};
            m_SpriteGpuState = EShaderGpuState::Failed;
            return;
        }
        m_SpriteGpuState = EShaderGpuState::PendingCommit;
    } else if (m_SpriteGpuState == EShaderGpuState::Compiling) {
        const EShaderStatus status = m_SpritePendingShaders.Status();
        if (status == EShaderStatus::Compiling) return;
        if (status != EShaderStatus::Ready) {
            m_SpritePendingShaders = {};
            m_SpriteGpuState = EShaderGpuState::Failed;
            ACS_LOG_WARN("LegacyScene3DAdapter: asynchronous 3D sprite compilation failed");
            return;
        }
        m_SpriteGpuState = EShaderGpuState::PendingCommit;
    }
    if (!requested || m_SpriteGpuState != EShaderGpuState::PendingCommit || !TryClaimGpuCommit(frame_commit, EGpuCommitSubsystem::Sprite)) {
        return;
    }

    const auto result = m_SpriteRenderer.InitWithCompiledShaders(device, Move(m_SpritePendingShaders), m_Post.HdrFormat(), depth_format, static_cast<u32>(m_CustomSprites.Num()));
    if (result.IsErr()) {
        m_SpritePendingShaders = {};
        m_SpriteGpuState = EShaderGpuState::Failed;
        ACS_LOG_WARN("LegacyScene3DAdapter: 3D sprite RHI commit failed: %s", result.Error().message);
        return;
    }
    m_SpriteGpuState = EShaderGpuState::Ready;
    ACS_LOG_INFO("LegacyScene3DAdapter: alpha-blended 3D sprite renderer is ready");
}

void ALegacyScene3DAdapter::AdvanceSkyInitialization(
    IRhiDevice& device,
    EGpuCommitSubsystem& frame_commit) noexcept {
    if (m_SkyGpuState == ESkyGpuState::CpuCompiling) {
        const i32 worker_state = m_SkyCompileWorkerState.load(
            std::memory_order_acquire);
        if (worker_state == 1) return;
        m_SkyCompileWorker.Join();
        m_SkyCompileWorkerState.store(0, std::memory_order_release);
        if (worker_state != 2) {
            m_SkyPendingShaders = {};
            m_SkyGpuState = ESkyGpuState::Failed;
            return;
        }
        m_SkyGpuState = ESkyGpuState::PendingCommit;
    } else if (m_SkyGpuState == ESkyGpuState::Compiling) {
        const EShaderStatus status = m_SkyPendingShaders.Status();
        if (status == EShaderStatus::Compiling) return;
        if (status != EShaderStatus::Ready) {
            m_SkyPendingShaders = {};
            m_SkyGpuState = ESkyGpuState::Failed;
            ACS_LOG_WARN(
                "LegacyScene3DAdapter: asynchronous sky compilation failed; "
                "the stable HDR clear remains active");
            return;
        }
        m_SkyGpuState = ESkyGpuState::PendingCommit;
    }
    if (m_SkyGpuState != ESkyGpuState::PendingCommit) {
        return;
    }
    if (!TryClaimGpuCommit(
            frame_commit, EGpuCommitSubsystem::Sky)) {
        return;
    }

    const auto result = m_Sky.InitWithCompiledShaders(
        device, Move(m_SkyPendingShaders),
        m_Post.HdrFormat(), m_FrameDepthFormat);
    if (result.IsErr()) {
        m_SkyGpuState = ESkyGpuState::Failed;
        ACS_LOG_WARN(
            "LegacyScene3DAdapter: sky RHI commit failed; "
            "the stable HDR clear remains active: %s",
            result.Error().message);
        return;
    }
    m_SkyGpuState = ESkyGpuState::Ready;
    ACS_LOG_INFO(
        "LegacyScene3DAdapter: HDR sky renderer is ready");
}

void ALegacyScene3DAdapter::AdvanceWaterInitialization(
    IRhiDevice& device,
    EGpuCommitSubsystem& frame_commit,
    bool scene_has_water) noexcept {
    if (m_WaterGpuState == EWaterGpuState::CpuCompiling) {
        const i32 worker_state = m_WaterCompileWorkerState.load(
            std::memory_order_acquire);
        if (worker_state == 1) return;
        m_WaterCompileWorker.Join();
        m_WaterCompileWorkerState.store(
            0, std::memory_order_release);
        if (worker_state != 2) {
            m_WaterPendingShaders = {};
            m_WaterGpuState = EWaterGpuState::Failed;
            return;
        }
        m_WaterGpuState = EWaterGpuState::PendingCommit;
    } else if (m_WaterGpuState == EWaterGpuState::Compiling) {
        const EShaderStatus status = m_WaterPendingShaders.Status();
        if (status == EShaderStatus::Compiling) return;
        if (status != EShaderStatus::Ready) {
            m_WaterPendingShaders = {};
            m_WaterGpuState = EWaterGpuState::Failed;
            ACS_LOG_WARN(
                "LegacyScene3DAdapter: interactive-water shader compilation "
                "failed; opaque PBR fallback remains active");
            return;
        }
        m_WaterGpuState = EWaterGpuState::PendingCommit;
    }

    if (m_WaterGpuState == EWaterGpuState::PendingCommit) {
        if (!scene_has_water
            || !TryClaimGpuCommit(
                frame_commit, EGpuCommitSubsystem::Water)) {
            return;
        }
        const auto result = m_Water.BeginInitWithCompiledShaders(
            device, Move(m_WaterPendingShaders),
            m_Post.HdrFormat(), m_FrameDepthFormat, 1u);
        if (result.IsErr()) {
            m_WaterGpuState = EWaterGpuState::Failed;
            ACS_LOG_WARN(
                "LegacyScene3DAdapter: interactive-water RHI commit failed; "
                "opaque PBR fallback remains active: %s",
                result.Error().message);
            return;
        }
        m_WaterGpuState = EWaterGpuState::Buffering;
        return;
    }

    if (m_WaterGpuState != EWaterGpuState::Buffering
        || !scene_has_water
        || !TryClaimGpuCommit(
            frame_commit, EGpuCommitSubsystem::Water)) {
        return;
    }
    const auto result = m_Water.AdvanceInitialization(16u);
    if (result.IsErr()) {
        m_WaterGpuState = EWaterGpuState::Failed;
        ACS_LOG_WARN(
            "LegacyScene3DAdapter: interactive-water bounded initialization "
            "failed; opaque PBR fallback remains active: %s",
            result.Error().message);
    } else if (result.Value()) {
        m_WaterGpuState = EWaterGpuState::Ready;
        ACS_LOG_INFO(
            "LegacyScene3DAdapter: interactive-water renderer is ready");
    }
}

u32 ALegacyScene3DAdapter::CollectWaterDraws(
    FWaterDraw (&draws)[CWaterSurface3D::kMaxTrackedSurfaces],
    IRhiTexture* depth,
    u32 width,
    u32 height) const noexcept {
    if (m_WaterGpuState != EWaterGpuState::Ready
        || m_BlitGpuState != EShaderGpuState::Ready
        || m_DepthSnapshotFailed
        || m_WaterBackground.Get() == nullptr
        || m_WaterBackground->Width() != width
        || m_WaterBackground->Height() != height
        || m_WaterDepthSnapshot.Get() == nullptr
        || depth == nullptr || depth->Width() != width
        || depth->Height() != height
        || !IsDepthTextureCopyCompatible(
            *depth, *m_WaterDepthSnapshot)) {
        return 0u;
    }

    struct FEntry {
        const ANode* Node = nullptr;
        bool ParentVisible = true;
        bool ParentEnabled = true;
    };
    TArray<FEntry> stack;
    if (!stack.TryAdd(FEntry{&Graph().Root(), true, true}))
        return 0u;

    u32 count = 0u;
    while (!stack.IsEmpty()) {
        const FEntry entry = stack.Last();
        stack.Pop();
        const ANode* node = entry.Node;
        if (node == nullptr || node->IsPendingDestroy()) continue;
        const bool enabled = entry.ParentEnabled && node->IsEnabled();
        const bool visible = entry.ParentVisible && node->IsVisible();
        for (u32 index = node->ChildCount(); index > 0u; --index) {
            if (!stack.TryAdd(FEntry{
                    node->Child(index - 1u), visible, enabled})) {
                // Never leave a partially excluded scene: draw every water
                // node through opaque PBR when collection cannot complete.
                return 0u;
            }
        }
        if (!enabled || !visible
            || count >= CWaterSurface3D::kMaxTrackedSurfaces) {
            continue;
        }
        const AMeshComponent3D* mesh = FindSprite(*node) == nullptr
            ? FindMesh(*node) : nullptr;
        const AWaterSurface3DComponent* water = FindWater(*node);
        if (mesh == nullptr || water == nullptr
            || !IsPlanarWaterMesh(*mesh)) {
            continue;
        }
        const FGpuMesh* gpu = GpuMeshFor(*mesh);
        if (gpu == nullptr || !gpu->vertex_buffer || !gpu->index_buffer)
            continue;
        draws[count++] = FWaterDraw{node, mesh, water, gpu};
    }
    return count;
}

void ALegacyScene3DAdapter::DrawWaterScene(
    FRenderContext& context,
    const FWaterDraw* water_draws,
    u32 water_count,
    IRhiTexture& background,
    IRhiTexture& opaque_depth_snapshot) noexcept {
    if (water_draws == nullptr || water_count == 0u) return;
    // 物の陰りと同じ太陽を使う。ここだけ別の向きにすると、水面のきらめきが
    // 陰りと逆を向いて «何かおかしい» 画になる。
    const FVec3 sun_direction = SunDirection();
    m_Water.SetFrame(
        m_Camera.ViewProjection(), m_Camera.Eye(),
        context.Width(), context.Height(),
        sun_direction, SunColorForWater());
    m_Water.SetEnvironment(
        m_Sky.ZenithColor(),
        m_Sky.HorizonColor(),
        m_Sky.GroundColor());
    for (u32 index = 0u; index < water_count; ++index) {
        const FWaterDraw& draw = water_draws[index];
        if (draw.Node == nullptr || draw.Water == nullptr
            || draw.Gpu == nullptr) {
            continue;
        }
        m_Water.SetParams(draw.Water->ToRenderParams());
        m_Water.DrawMesh(
            context.Cmd(), *draw.Gpu, draw.Node->World().ToMat4(),
            &background, &opaque_depth_snapshot, nullptr,
            static_cast<u64>(draw.Node->Id().m_Packed), true);
    }
}

void ALegacyScene3DAdapter::DrawWaterFallback(
    FRenderContext& context,
    const FWaterDraw* water_draws,
    u32 water_count) noexcept {
    if (water_draws == nullptr || water_count == 0u) return;
    CPbrShader& shader = ActiveHdrShader();
    for (u32 index = 0u; index < water_count; ++index) {
        const FWaterDraw& draw = water_draws[index];
        if (draw.Node == nullptr || draw.Water == nullptr
            || draw.Gpu == nullptr) {
            continue;
        }
        const FWaterSurface3DParams parameters =
            draw.Water->ToRenderParams();
        const FVec3 fallback_color =
            parameters.shallow_color * 0.72f
            + parameters.deep_color * 0.28f;
        shader.ClearSubstrateSurface();
        shader.SetExtParams(0.0f, 0.1f, 0.0f);
        shader.SetEmissive(FVec3::Zero(), 0.0f);
        shader.SetSheen(FVec3::One(), 0.0f);
        shader.SetSubsurface(FVec3::Zero(), 0.0f);
        shader.SetNormalMap(nullptr, 0.0f);
        shader.DrawMesh(
            context.Cmd(), *draw.Gpu, draw.Node->World().ToMat4(),
            fallback_color, 0.0f, parameters.roughness, 1.0f);
    }
}

bool ALegacyScene3DAdapter::UploadGraphMeshes(IRhiDevice& device) noexcept {
    m_CustomMeshes.Reset();
    if (!m_CustomMeshes.TryReserve(Graph().NodeCount()))
        return false;
    TArray<ANode*> stack;
    if (!stack.TryAdd(&Graph().Root())) return false;
    while (!stack.IsEmpty()) {
        ANode* node = stack.Last();
        stack.Pop();
        if (node == nullptr) continue;
        AMeshComponent3D* component = FindSprite(*node) == nullptr
            ? FindMesh(*node) : nullptr;
        if (component != nullptr
            && component->Primitive() == EMeshPrimitive3D::Mesh) {
            AMeshAsset* mesh = component->Mesh();
            if (mesh == nullptr) return false;
            FCustomGpuMesh uploaded;
            uploaded.Component = component;
            if (UploadMesh(device, *mesh, uploaded.Mesh).IsErr()
                || !m_CustomMeshes.TryAdd(Move(uploaded))) {
                return false;
            }
        }
        for (u32 index = 0u; index < node->ChildCount(); ++index)
            if (!stack.TryAdd(node->Child(index))) return false;
    }
    return true;
}

bool ALegacyScene3DAdapter::UploadGraphSprites(IRhiDevice& device) noexcept
{
    TArray<FCustomGpuSprite> uploaded(*m_CustomSprites.GetAllocator());
    if (!uploaded.TryReserve(Graph().NodeCount())) return false;
    TArray<ANode*> stack;
    if (!stack.TryAdd(&Graph().Root())) return false;
    while (!stack.IsEmpty()) {
        ANode* node = stack.Last();
        stack.Pop();
        if (node == nullptr) continue;
        const ASprite3DComponent* component = FindSprite(*node);
        if (component != nullptr) {
            AImageAsset* image = component->Image();
            if (image == nullptr) return false;
            auto texture = UploadTexture(device, *image);
            if (texture.IsErr()) return false;
            FCustomGpuSprite sprite;
            sprite.Component = component;
            sprite.Texture = Move(texture.Value());
            if (!uploaded.TryAdd(Move(sprite))) return false;
        }
        for (u32 index = 0u; index < node->ChildCount(); ++index) {
            if (!stack.TryAdd(node->Child(index))) return false;
        }
    }

    TArray<CSprite3DRenderer::FDraw> draws(*m_SpriteDraws.GetAllocator());
    if (!draws.TryReserve(uploaded.Num())) return false;
    m_CustomSprites = Move(uploaded);
    m_SpriteDraws = Move(draws);
    return true;
}

IRhiTexture* ALegacyScene3DAdapter::TextureFor(const ASprite3DComponent& component) const noexcept
{
    for (u32 index = 0u; index < m_CustomSprites.Num(); ++index) {
        if (m_CustomSprites[index].Component == &component)
            return m_CustomSprites[index].Texture.Get();
    }
    return nullptr;
}

bool ALegacyScene3DAdapter::DrawSpriteScene(FRenderContext& context) noexcept
{
    if (m_SpriteGpuState != EShaderGpuState::Ready) return false;
    m_SpriteDraws.Reset();
    TArray<const ANode*> stack;
    if (!stack.TryAdd(&Graph().Root())) return false;
    while (!stack.IsEmpty()) {
        const ANode* node = stack.Last();
        stack.Pop();
        if (node == nullptr || node->IsPendingDestroy()) continue;
        for (u32 index = node->ChildCount(); index > 0u; --index) {
            if (!stack.TryAdd(node->Child(index - 1u))) return false;
        }
        if (!IsEffectivelyActive(*node)) continue;
        const ASprite3DComponent* component = FindSprite(*node);
        if (component == nullptr) continue;
        IRhiTexture* texture = TextureFor(*component);
        if (texture == nullptr || !m_SpriteDraws.TryAdd(CSprite3DRenderer::FDraw{node->World().ToMat4(), texture})) {
            return false;
        }
    }
    return m_SpriteRenderer.DrawBatch(context.Cmd(), m_Camera.ViewProjection(), m_SpriteDraws.GetData(), static_cast<u32>(m_SpriteDraws.Num()));
}

void ALegacyScene3DAdapter::DrainAndReleaseGpu() noexcept {
    // A raw-DX12 startup worker may own in-flight resource/PSO creation.
    // Join before the owner-thread queue drain, then release every resource.
    // This same barrier is required by public live reload: command lists from
    // the previous graph may still reference the active/inactive PBR slots.
    JoinCpuCompileWorkers();
    if (IRhiDevice* device = GetGame().GetRenderer().Device())
        device->WaitIdle();
    ReleaseGpu();
}

void ALegacyScene3DAdapter::ReleaseGpu() noexcept {
    JoinCpuCompileWorkers();
    m_HdrPendingShaders = {};
    m_HdrSsssPendingShaders = {};
    m_SsssPendingShaders = {};
    m_PostPendingShaders = {};
    m_BlitPendingShaders = {};
    m_SpritePendingShaders = {};
    m_SkyPendingShaders = {};
    m_Sky.Shutdown();
    m_WaterPendingShaders = {};
    m_Water.Shutdown();
    m_WaterBackground.Reset();
    m_WaterDepthSnapshot.Reset();
    m_SsssPendingDiffuse.Reset();
    m_SsssPendingMaterial.Reset();
    m_SsssPendingNormal.Reset();
    m_SsssDiffuse.Reset();
    m_SsssMaterial.Reset();
    m_SsssNormal.Reset();
    m_Ssss.Shutdown();
    m_Blit.Shutdown();
    m_SpriteRenderer.Shutdown();
    m_Post.Shutdown();
    m_HdrShaders[0].Shutdown();
    m_HdrShaders[1].Shutdown();
    m_CustomMeshes.Reset();
    m_CustomSprites.Reset();
    m_SpriteDraws.Reset();
    m_Cube = FGpuMesh{};
    m_Sphere = FGpuMesh{};
    m_Plane = FGpuMesh{};
    m_HdrShaderGpuState = EShaderGpuState::Unavailable;
    m_HdrSsssGpuState = EShaderGpuState::Unavailable;
    m_SsssGpuState = EShaderGpuState::Unavailable;
    m_HdrCompileDevice = nullptr;
    m_HdrSsssCompileDevice = nullptr;
    m_SsssCompileDevice = nullptr;
    m_HdrActiveSlot = 0u;
    m_HdrPendingSlot = 1u;
    m_HdrPendingIsInitialized = false;
    m_HdrSsssPendingSlot = 0u;
    m_HdrSsssPendingIsInitialized = false;
    m_SsssPendingIsInitialized = false;
    m_PostGpuState = EShaderGpuState::Unavailable;
    m_BlitGpuState = EShaderGpuState::Unavailable;
    m_SpriteGpuState = EShaderGpuState::Unavailable;
    m_WaterGpuState = EWaterGpuState::Unavailable;
    m_FrameWidth = 0u;
    m_FrameHeight = 0u;
    m_BackgroundAttemptWidth = 0u;
    m_BackgroundAttemptHeight = 0u;
    m_DepthAttemptWidth = 0u;
    m_DepthAttemptHeight = 0u;
    m_SsssResizeAttemptWidth = 0u;
    m_SsssResizeAttemptHeight = 0u;
    m_SsssAuxAttemptWidth = 0u;
    m_SsssAuxAttemptHeight = 0u;
    m_SsssPendingAuxWidth = 0u;
    m_SsssPendingAuxHeight = 0u;
    m_DepthSnapshotFailed = false;
    m_SsssRequested = false;
    m_SkyGpuState = ESkyGpuState::Unavailable;
    m_GpuReady = false;
    m_GpuAttempted = false;
}

void ALegacyScene3DAdapter::JoinCpuCompileWorkers() noexcept {
    m_HdrCompileWorker.Join();
    m_HdrCompileDevice = nullptr;
    m_HdrCompileWorkerState.store(0, std::memory_order_release);
    m_HdrSsssCompileWorker.Join();
    m_HdrSsssCompileDevice = nullptr;
    m_HdrSsssCompileWorkerState.store(
        0, std::memory_order_release);
    m_SsssCompileWorker.Join();
    m_SsssCompileDevice = nullptr;
    m_SsssCompileWorkerState.store(0, std::memory_order_release);
    m_PostCompileWorker.Join();
    m_PostCompileWorkerState.store(0, std::memory_order_release);
    m_BlitCompileWorker.Join();
    m_BlitCompileWorkerState.store(0, std::memory_order_release);
    m_SpriteCompileWorker.Join();
    m_SpriteCompileWorkerState.store(0, std::memory_order_release);
    m_SkyCompileWorker.Join();
    m_SkyCompileWorkerState.store(0, std::memory_order_release);
    m_WaterCompileWorker.Join();
    m_WaterCompileWorkerState.store(0, std::memory_order_release);
}

void ALegacyScene3DAdapter::UpdateCameraProjection(
    u32 width,
    u32 height) noexcept {
    const f32 safe_width = width > 0u ? static_cast<f32>(width) : 1.0f;
    const f32 safe_height = height > 0u ? static_cast<f32>(height) : 1.0f;
    const f32 aspect = safe_width / safe_height;
    if (m_UseAuthoredCamera) {
        if (m_AuthoredCamera.Projection ==
            EScene3DCameraProjection::Orthographic) {
            m_Camera.SetOrthographic(
                m_AuthoredCamera.OrthographicHeight * aspect,
                m_AuthoredCamera.OrthographicHeight,
                m_AuthoredCamera.NearPlane,
                m_AuthoredCamera.FarPlane);
        } else {
            m_Camera.SetPerspective(
                m_AuthoredCamera.FovYDegrees * kDeg2Rad,
                aspect,
                m_AuthoredCamera.NearPlane,
                m_AuthoredCamera.FarPlane);
        }
        return;
    }
    const f32 far_plane = m_PresentedOrbitCameraState.distance * 200.0f + 1000.0f;
    if (m_Projection == ESceneProjectionMode::Orthographic) {
        const f32 view_height = m_PresentedOrbitCameraState.distance * 1.25f;
        m_Camera.SetOrthographic(
            view_height * aspect, view_height, 0.01f, far_plane);
    } else {
        m_Camera.SetPerspective(
            55.0f * kDeg2Rad, aspect, 0.05f, far_plane);
    }
}

void ALegacyScene3DAdapter::SetOrbit(
    FVec3 target, f32 yaw, f32 pitch, f32 distance) noexcept {
    COrbitCameraController3D::FOrbitCameraState3D candidate{target, yaw, pitch, distance > 0.01f ? distance : 0.01f};
    if (!m_OrbitCameraController.TryStep(COrbitCameraController3D::FOrbitCameraInput3D{}, 0.0f, candidate)) return;
    m_OrbitCameraState = candidate;
    m_PreviousOrbitCameraState = candidate;
    m_PresentedOrbitCameraState = candidate;
    m_IsOrbitCameraObstructionPresentationActive = false;
    UpdateCameraView();
}

/** 障害物回避距離を隔離検証し、成功時だけ表示設定へ反映する。 */
bool ALegacyScene3DAdapter::TrySetOrbitCameraObstructionSettings(const FOrbitCameraObstructionSettings3D& settings) noexcept
{
    if (!std::isfinite(settings.TargetClearance) || !std::isfinite(settings.CameraClearance) || !std::isfinite(settings.ProbeRadius) || !std::isfinite(settings.RecoverySharpness) || settings.TargetClearance <= 0.0f || settings.CameraClearance < 0.0f || settings.ProbeRadius < 0.0f || settings.RecoverySharpness < 0.0f)
        return false;
    const f32 minimum_resolved_distance = settings.TargetClearance - settings.CameraClearance;
    if (!std::isfinite(minimum_resolved_distance) || minimum_resolved_distance < m_OrbitCameraController.Settings().minimum_distance) return false;
    m_OrbitCameraObstructionSettings = settings;
    if (!settings.Enabled || settings.RecoverySharpness <= 0.0f) m_IsOrbitCameraObstructionPresentationActive = false;
    UpdateCameraView();
    return true;
}

/** 自由cameraのprevious/currentを検証してからsnapshotへ公開する。 */
bool ALegacyScene3DAdapter::TryCaptureOrbitCameraSnapshot(COrbitCameraController3D::FOrbitCameraFixedStepSnapshot3D& output) const noexcept
{
    /** 検証完了まで呼び出し側出力へ触れないsnapshot候補。 */
    const COrbitCameraController3D::FOrbitCameraFixedStepSnapshot3D candidate{m_PreviousOrbitCameraState, m_OrbitCameraState};
    if (!m_OrbitCameraController.IsSnapshotValid(candidate)) return false;
    output = candidate;
    return true;
}

/** snapshotを隔離検証し、成功時だけ自由camera補間区間を置き換える。 */
bool ALegacyScene3DAdapter::TryRestoreOrbitCameraSnapshot(const COrbitCameraController3D::FOrbitCameraFixedStepSnapshot3D& snapshot) noexcept
{
    if (!m_OrbitCameraController.IsSnapshotValid(snapshot)) return false;
    m_PreviousOrbitCameraState = snapshot.previous;
    m_OrbitCameraState = snapshot.current;
    m_PresentedOrbitCameraState = snapshot.current;
    m_IsOrbitCameraObstructionPresentationActive = false;
    UpdateCameraView();
    return true;
}

/** 自由cameraの有効状態を切り替え、古い補間区間を残さない。 */
void ALegacyScene3DAdapter::SetFreeCameraEnabled(bool enabled) noexcept
{
    if (m_FreeCameraEnabled == enabled) return;
    m_FreeCameraEnabled = enabled;
    m_PreviousOrbitCameraState = m_OrbitCameraState;
    m_PresentedOrbitCameraState = m_OrbitCameraState;
    m_IsOrbitCameraObstructionPresentationActive = false;
    UpdateCameraView();
}

void ALegacyScene3DAdapter::UpdateCameraView() noexcept {
    if (m_UseAuthoredCamera) {
        m_Camera.SetLookDirection(m_AuthoredCamera.Position, m_AuthoredCamera.Forward, m_AuthoredCamera.Up);
        return;
    }
    UpdateOrbitCameraView_Internal(m_OrbitCameraState, 0.0f);
}

/** 指定した自由camera状態からviewを構築し、成功時だけ表示へ反映する。 */
void ALegacyScene3DAdapter::UpdateOrbitCameraView_Internal(const COrbitCameraController3D::FOrbitCameraState3D& state, f32 recovery_delta_seconds) noexcept
{
    /** scene障害物を反映してもsimulation stateを変更しないpresentation候補。 */
    COrbitCameraController3D::FOrbitCameraState3D resolved{};
    if (!TryResolveOrbitCameraObstruction_Internal(state, resolved)) return;
    /** 今回のqueryがdesired距離を短縮したならtrue。 */
    const bool has_obstruction = resolved.distance < state.distance;
    /** 接近または外向き復帰を反映した最終presentation候補。 */
    COrbitCameraController3D::FOrbitCameraState3D presented = resolved;
    if (m_IsOrbitCameraObstructionPresentationActive || has_obstruction) {
        if (!m_OrbitCameraController.TryAdvanceObstructionPresentation(resolved, m_PresentedOrbitCameraState.distance, m_OrbitCameraObstructionSettings.RecoverySharpness, recovery_delta_seconds, presented)) return;
    }
    /** cameraへ反映する左手座標系view候補。 */
    COrbitCameraController3D::FOrbitCameraView3D view{};
    if (!m_OrbitCameraController.TryBuildView(presented, view)) return;
    m_PresentedOrbitCameraState = presented;
    m_IsOrbitCameraObstructionPresentationActive = m_OrbitCameraObstructionSettings.Enabled && m_OrbitCameraObstructionSettings.RecoverySharpness > 0.0f && (has_obstruction || presented.distance < state.distance);
    // 軌道角度から直接前方を再構築し、遠方のeyeと注視点の減算誤差を避ける。
    const f32 pitchCosine = Cos(presented.pitch_radians);
    const FVec3 forward{Sin(presented.yaw_radians) * pitchCosine, -Sin(presented.pitch_radians), Cos(presented.yaw_radians) * pitchCosine};
    m_Camera.SetLookDirection(view.eye, forward, view.up);
}

/** target近傍を除く有効scene meshから自由cameraのpresentation距離を解決する。 */
bool ALegacyScene3DAdapter::TryResolveOrbitCameraObstruction_Internal(const COrbitCameraController3D::FOrbitCameraState3D& state, COrbitCameraController3D::FOrbitCameraState3D& output) const noexcept
{
    if (!m_OrbitCameraObstructionSettings.Enabled || m_OrbitCameraObstructionSettings.TargetClearance > state.distance) {
        output = state;
        return true;
    }
    /** desired eyeとtargetから作る正規化済み障害物探索ray。 */
    COrbitCameraController3D::FOrbitCameraView3D desired_view{};
    if (!m_OrbitCameraController.TryBuildView(state, desired_view)) return false;
    const FVec3 direction = (desired_view.eye - state.target) * (1.0f / state.distance);
    f32 obstruction_distance = 0.0f;
    FNodeId obstruction{};
    if (m_OrbitCameraObstructionSettings.ProbeRadius == 0.0f)
        obstruction = Graph().RaycastGeometryActiveRange(FRay3{state.target, direction}, m_OrbitCameraObstructionSettings.TargetClearance, state.distance, &obstruction_distance);
    else
        obstruction = Graph().SweepSphereActiveRange(FRay3{state.target, direction}, m_OrbitCameraObstructionSettings.ProbeRadius, m_OrbitCameraObstructionSettings.TargetClearance, state.distance, &obstruction_distance);
    if (!obstruction.IsValid()) {
        output = state;
        return true;
    }
    return m_OrbitCameraController.TryResolveObstructedState(state, obstruction_distance, m_OrbitCameraObstructionSettings.CameraClearance, output);
}

/** 固定tick状態と時計alphaから今回表示する自由camera viewを決める。 */
void ALegacyScene3DAdapter::UpdatePresentedCameraView_Internal(f32 recovery_delta_seconds) noexcept
{
    if (m_UseAuthoredCamera) {
        UpdateCameraView();
        return;
    }
    if (!m_FreeCameraEnabled || !GetGame().IsFixedTimestepEnabled()) {
        UpdateOrbitCameraView_Internal(m_OrbitCameraState, recovery_delta_seconds);
        return;
    }
    /** previous/current間を固定時計alphaで混ぜる表示状態。 */
    COrbitCameraController3D::FOrbitCameraState3D presented{};
    if (!m_OrbitCameraController.TryInterpolateState(m_PreviousOrbitCameraState, m_OrbitCameraState, GetGame().FixedStepInterpolationAlpha(), presented)) {
        UpdateCameraView();
        return;
    }
    UpdateOrbitCameraView_Internal(presented, recovery_delta_seconds);
}

const FGpuMesh* ALegacyScene3DAdapter::GpuMeshFor(
    const AMeshComponent3D& component) const noexcept {
    switch (component.Primitive()) {
    case EMeshPrimitive3D::Cube: return &m_Cube;
    case EMeshPrimitive3D::Sphere: return &m_Sphere;
    case EMeshPrimitive3D::Plane: return &m_Plane;
    case EMeshPrimitive3D::Mesh:
        for (u32 index = 0u; index < m_CustomMeshes.Num(); ++index) {
            if (m_CustomMeshes[index].Component == &component)
                return &m_CustomMeshes[index].Mesh;
        }
        return nullptr;
    }
    return nullptr;
}

} // namespace acs::game
