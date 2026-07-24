// SPDX-License-Identifier: Apache-2.0

#include "test/Test.h"
#include "test/Expect.h"

#include "asset/MeshAsset.h"
#include "gameframework/LegacyScene3DAdapter.h"
#include "gameframework/MeshComponent3D.h"
#include "gameframework/WaterSurface3DComponent.h"
#include "math/Mat.h"
#include "math/Math.h"
#include "math/Quat.h"
#include "memory/SharedPtr.h"

using namespace acs;
using namespace acs::game;

namespace {

FScene3DSpawnResult SpawnWaterPlane(
    FLegacyScene3DAdapter& runtime,
    FStringView name) noexcept {
    FScene3DSpawnResult spawned = runtime.Graph().TrySpawn(name);
    if (!spawned) return spawned;
    spawned.Node->AddComponent<AMeshComponent3D>(
        EMeshPrimitive3D::Plane);
    spawned.Node->AddComponent<AWaterSurface3DComponent>();
    return spawned;
}

TSharedPtr<FMeshAsset> MakePlanarTriangle() noexcept {
    TSharedPtr<FMeshAsset> mesh = MakeShared<FMeshAsset>();
    if (!mesh) return {};
    mesh->Vertices().PushBack(
        FMeshVertex{FVec3{0.0f, 0.0f, 0.0f},
                    FVec3{0.0f, 1.0f, 0.0f}, 0.0f, 0.0f});
    mesh->Vertices().PushBack(
        FMeshVertex{FVec3{2.0f, 0.0f, 0.0f},
                    FVec3{0.0f, 1.0f, 0.0f}, 1.0f, 0.0f});
    mesh->Vertices().PushBack(
        FMeshVertex{FVec3{0.0f, 0.0f, 2.0f},
                    FVec3{0.0f, 1.0f, 0.0f}, 0.0f, 1.0f});
    mesh->Indices().PushBack(0u);
    mesh->Indices().PushBack(1u);
    mesh->Indices().PushBack(2u);
    return mesh;
}

} // namespace

ACS_TEST(LegacyScene3DWaterRuntime, TransformedPlaneUsesExactWorldHit) {
    FLegacyScene3DAdapter runtime;
    FScene3DSpawnResult surface =
        SpawnWaterPlane(runtime, FStringView("TransformedWater"));
    EXPECT_TRUE(surface.Succeeded());

    surface.Node->Local().position = FVec3{3.0f, 1.5f, -2.0f};
    surface.Node->Local().rotation =
        FQuat::AxisAngle(FVec3{1.0f, 0.0f, 0.0f}, 0.42f);
    surface.Node->Local().scale = FVec3{4.0f, 1.5f, 3.0f};

    const FMat4 model = surface.Node->World().ToMat4();
    const FVec3 expected_point =
        TransformPoint(FVec3{0.20f, 0.0f, 0.10f}, model);
    const FVec3 expected_normal = Normalize(TransformVector(
        FVec3{0.0f, 1.0f, 0.0f}, Transpose(Inverse(model))));
    const FRay3 ray{
        expected_point + expected_normal * 5.0f,
        -expected_normal,
    };

    FWaterRaycastHit hit{};
    EXPECT_TRUE(runtime.RaycastWater(ray, hit, 20.0f));
    EXPECT_TRUE(hit.Node == surface.Id);
    EXPECT_NEAR(hit.Distance, 5.0f, 1.0e-4f);
    EXPECT_NEAR(hit.Point.x, expected_point.x, 1.0e-4f);
    EXPECT_NEAR(hit.Point.y, expected_point.y, 1.0e-4f);
    EXPECT_NEAR(hit.Point.z, expected_point.z, 1.0e-4f);
    EXPECT_NEAR(hit.Normal.x, expected_normal.x, 1.0e-4f);
    EXPECT_NEAR(hit.Normal.y, expected_normal.y, 1.0e-4f);
    EXPECT_NEAR(hit.Normal.z, expected_normal.z, 1.0e-4f);

    EXPECT_TRUE(runtime.AddWaterDisturbance(
        hit.Node, hit.Point, 0.24f, 0.31f));
    EXPECT_TRUE(runtime.AddWaterWake(
        hit.Node, hit.Point, FVec3{3.0f, 0.0f, 1.0f},
        0.28f, 0.17f));
    EXPECT_EQ(runtime.ActiveWaterRippleCount(surface.Id), 2u);
}

ACS_TEST(LegacyScene3DWaterRuntime, OpaqueForegroundRejectsInteraction) {
    FLegacyScene3DAdapter runtime;
    FScene3DSpawnResult surface =
        SpawnWaterPlane(runtime, FStringView("OccludedWater"));
    EXPECT_TRUE(surface.Succeeded());
    surface.Node->Local().scale = FVec3{10.0f, 1.0f, 10.0f};

    FScene3DSpawnResult blocker =
        runtime.Graph().TrySpawn(FStringView("ForegroundCube"));
    EXPECT_TRUE(blocker.Succeeded());
    blocker.Node->Local().position = FVec3{0.0f, 2.0f, 0.0f};
    blocker.Node->AddComponent<AMeshComponent3D>(
        EMeshPrimitive3D::Cube);

    const FRay3 ray{
        FVec3{0.0f, 5.0f, 0.0f},
        FVec3{0.0f, -1.0f, 0.0f},
    };
    FWaterRaycastHit hit{};
    EXPECT_FALSE(runtime.RaycastWater(ray, hit, 20.0f));
    EXPECT_FALSE(hit.IsValid());

    blocker.Node->SetVisible(false);
    EXPECT_TRUE(runtime.RaycastWater(ray, hit, 20.0f));
    EXPECT_TRUE(hit.Node == surface.Id);

    blocker.Node->SetVisible(true);
    blocker.Node->SetEnabled(false);
    EXPECT_TRUE(runtime.RaycastWater(ray, hit, 20.0f));
    EXPECT_TRUE(hit.Node == surface.Id);
}

ACS_TEST(LegacyScene3DWaterRuntime, HiddenAncestorRejectsDirectInteraction) {
    FLegacyScene3DAdapter runtime;
    FScene3DSpawnResult group =
        runtime.Graph().TrySpawn(FStringView("WaterGroup"));
    EXPECT_TRUE(group.Succeeded());
    FScene3DSpawnResult surface =
        runtime.Graph().TrySpawn(
            FStringView("NestedWater"), group.Node);
    EXPECT_TRUE(surface.Succeeded());
    surface.Node->AddComponent<AMeshComponent3D>(
        EMeshPrimitive3D::Plane);
    surface.Node->AddComponent<AWaterSurface3DComponent>();

    const FVec3 point{0.0f, 0.0f, 0.0f};
    group.Node->SetVisible(false);
    EXPECT_FALSE(runtime.AddWaterDisturbance(
        surface.Id, point, 0.2f, 0.2f));
    group.Node->SetVisible(true);
    group.Node->SetEnabled(false);
    EXPECT_FALSE(runtime.AddWaterWake(
        surface.Id, point, FVec3{1.0f, 0.0f, 0.0f},
        0.2f, 0.2f));
    EXPECT_EQ(runtime.ActiveWaterRippleCount(surface.Id), 0u);
}

ACS_TEST(LegacyScene3DWaterRuntime, CustomWaterUsesAuthoredTriangles) {
    FLegacyScene3DAdapter runtime;
    FScene3DSpawnResult surface =
        runtime.Graph().TrySpawn(FStringView("TriangleWater"));
    EXPECT_TRUE(surface.Succeeded());
    auto& mesh_component =
        surface.Node->AddComponent<AMeshComponent3D>();
    TSharedPtr<FMeshAsset> triangle = MakePlanarTriangle();
    EXPECT_TRUE(static_cast<bool>(triangle));
    mesh_component.SetMeshAsset(TSharedPtr<FAsset>(triangle));
    surface.Node->AddComponent<AWaterSurface3DComponent>();

    FWaterRaycastHit hit{};
    EXPECT_TRUE(runtime.RaycastWater(
        FRay3{FVec3{0.25f, 4.0f, 0.25f},
              FVec3{0.0f, -1.0f, 0.0f}},
        hit, 10.0f));
    EXPECT_TRUE(hit.Node == surface.Id);

    // This point is inside the triangle's AABB but outside the authored
    // triangle. A bounds-only water picker would return a false hit here.
    EXPECT_FALSE(runtime.RaycastWater(
        FRay3{FVec3{1.80f, 4.0f, 1.80f},
              FVec3{0.0f, -1.0f, 0.0f}},
        hit, 10.0f));
}

ACS_TEST(LegacyScene3DWaterRuntime, NonPlanarCustomMeshStaysOpaque) {
    FLegacyScene3DAdapter runtime;
    FScene3DSpawnResult surface =
        runtime.Graph().TrySpawn(FStringView("NonPlanarWater"));
    EXPECT_TRUE(surface.Succeeded());
    auto& mesh_component =
        surface.Node->AddComponent<AMeshComponent3D>();
    TSharedPtr<FMeshAsset> mesh = MakePlanarTriangle();
    EXPECT_TRUE(static_cast<bool>(mesh));
    mesh->Vertices()[2].position.y = 1.0f;
    mesh_component.SetMeshAsset(TSharedPtr<FAsset>(mesh));
    surface.Node->AddComponent<AWaterSurface3DComponent>();

    FWaterRaycastHit hit{};
    EXPECT_FALSE(runtime.RaycastWater(
        FRay3{FVec3{0.25f, 4.0f, 0.25f},
              FVec3{0.0f, -1.0f, 0.0f}},
        hit, 10.0f));
    EXPECT_FALSE(runtime.AddWaterDisturbance(
        surface.Id, FVec3{0.25f, 0.0f, 0.25f}));
    EXPECT_EQ(runtime.ActiveWaterRippleCount(surface.Id), 0u);
}
