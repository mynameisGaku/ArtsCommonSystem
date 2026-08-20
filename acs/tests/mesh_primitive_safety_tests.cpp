// SPDX-License-Identifier: Apache-2.0
#include "asset/MeshPrimitive.h"
#include "math/Math.h"
#include "memory/Memory.h"
#include "memory/SystemAllocator.h"
#include "test/Expect.h"
#include "test/Test.h"

#include <cstddef>
#include <limits>
#include <type_traits>

using namespace acs;

namespace {

/** 指定した確保要求だけを失敗させ、未解放数を追跡するアロケータ。 */
class CFailOnMeshRequestAllocator final : public IAllocator {
public:
    /**
     * 次の生成処理で失敗させる要求番号を設定する。
     * @param request 1 始まりの要求番号。0 なら失敗させない。
     */
    void Begin(u64 request) noexcept
    {
        m_RequestCount = 0u;
        m_FailingRequest = request;
    }

    /** 現在の生成処理で受けた確保要求数を返す。 */
    u64 RequestCount() const noexcept { return m_RequestCount; }

    /** 現在残っている確保数を返す。 */
    u64 OutstandingAllocationCount() const noexcept { return m_Backing.AllocationCount(); }

    void* Alloc(usize size, usize alignment, FSourceLoc location) noexcept override
    {
        ++m_RequestCount;
        if (m_FailingRequest != 0u && m_RequestCount == m_FailingRequest) return nullptr;
        return m_Backing.Alloc(size, alignment, location);
    }

    void Free(void* pointer) noexcept override
    {
        m_Backing.Free(pointer);
    }

    u64 BytesAllocated() const noexcept override { return m_Backing.BytesAllocated(); }

    u64 AllocationCount() const noexcept override { return m_Backing.AllocationCount(); }

private:
    /** 実際の確保と解放を行う backing。 */
    CSystemAllocator m_Backing;

    /** 現在の生成処理で受けた確保要求数。 */
    u64 m_RequestCount = 0u;

    /** 失敗させる 1 始まりの要求番号。 */
    u64 m_FailingRequest = 0u;
};

/** テスト中だけ既定アロケータを差し替え、終了時に必ず戻す。 */
class CDefaultAllocatorScope final {
public:
    explicit CDefaultAllocatorScope(IAllocator& allocator) noexcept
        : m_Previous(&DefaultAllocator())
    {
        SetDefaultAllocator(&allocator);
    }

    ~CDefaultAllocatorScope() noexcept
    {
        SetDefaultAllocator(m_Previous);
    }

    CDefaultAllocatorScope(const CDefaultAllocatorScope&) = delete;
    CDefaultAllocatorScope& operator=(const CDefaultAllocatorScope&) = delete;

private:
    /** 差し替え前の既定アロケータ。 */
    IAllocator* m_Previous = nullptr;
};

/** 頂点の全属性を許容誤差付きで照合する。 */
void ExpectVertex(const FMeshVertex& vertex, const FVec3& position, const FVec3& normal, f32 u, f32 v) noexcept
{
    constexpr f32 epsilon = 1.0e-5f;
    EXPECT_NEAR(vertex.position.x, position.x, epsilon);
    EXPECT_NEAR(vertex.position.y, position.y, epsilon);
    EXPECT_NEAR(vertex.position.z, position.z, epsilon);
    EXPECT_NEAR(vertex.normal.x, normal.x, epsilon);
    EXPECT_NEAR(vertex.normal.y, normal.y, epsilon);
    EXPECT_NEAR(vertex.normal.z, normal.z, epsilon);
    EXPECT_NEAR(vertex.u, u, epsilon);
    EXPECT_NEAR(vertex.v, v, epsilon);
}

/** 単一サブメッシュが配列全体を覆うことを照合する。 */
void ExpectWholeMeshRange(const AMeshAsset& asset, usize vertex_count, usize index_count) noexcept
{
    EXPECT_EQ(asset.Vertices().Num(), vertex_count);
    EXPECT_EQ(asset.Indices().Num(), index_count);
    EXPECT_EQ(asset.SubMeshes().Num(), 1u);
    EXPECT_EQ(asset.SubMeshes()[0].first_index, 0u);
    EXPECT_EQ(asset.SubMeshes()[0].index_count, static_cast<u32>(index_count));
    EXPECT_EQ(asset.GeometryRevision(), 4u);
}

/** 三プリミティブ共通の四確保位置と互換 wrapper の失敗動作を検証する。 */
template<typename TTryCall, typename TMakeCall>
void ExpectAllocationFailureMatrix(TTryCall try_call, TMakeCall make_call)
{
    /** 失敗時の同一性を確認する既存出力。 */
    TSharedPtr<AMeshAsset> sentinel = MakeShared<AMeshAsset>();
    EXPECT_TRUE(sentinel.IsValid());
    AMeshAsset* const sentinel_address = sentinel.Get();

    CFailOnMeshRequestAllocator allocator;
    CDefaultAllocatorScope allocator_scope(allocator);
    for (u64 failing_request = 1u; failing_request <= 4u; ++failing_request) {
        TSharedPtr<AMeshAsset> output = sentinel;
        allocator.Begin(failing_request);
        EXPECT_FALSE(try_call(output));
        EXPECT_EQ(output.Get(), sentinel_address);
        EXPECT_EQ(allocator.RequestCount(), failing_request);
        EXPECT_EQ(allocator.OutstandingAllocationCount(), 0u);
        output.Reset();
        EXPECT_EQ(allocator.OutstandingAllocationCount(), 0u);

        allocator.Begin(failing_request);
        TSharedPtr<AMeshAsset> legacy_output = make_call();
        EXPECT_FALSE(legacy_output.IsValid());
        EXPECT_EQ(allocator.RequestCount(), failing_request);
        legacy_output.Reset();
        EXPECT_EQ(allocator.OutstandingAllocationCount(), 0u);
    }

    /** 五番目を失敗対象にして、成功経路の要求数も四件に固定する。 */
    TSharedPtr<AMeshAsset> output = sentinel;
    allocator.Begin(5u);
    EXPECT_TRUE(try_call(output));
    EXPECT_NE(output.Get(), sentinel_address);
    EXPECT_EQ(allocator.RequestCount(), 4u);
    EXPECT_EQ(allocator.OutstandingAllocationCount(), 4u);
    output.Reset();
    EXPECT_EQ(allocator.OutstandingAllocationCount(), 0u);
}

/** Try APIだけを持つ形状生成について、四確保位置と成功時の所有権を検証する。 */
template<typename TTryCall>
void ExpectTryAllocationFailureMatrix(TTryCall try_call)
{
    /** 失敗時の同一性を確認する既存出力。 */
    TSharedPtr<AMeshAsset> sentinel = MakeShared<AMeshAsset>();
    EXPECT_TRUE(sentinel.IsValid());
    AMeshAsset* const sentinel_address = sentinel.Get();

    CFailOnMeshRequestAllocator allocator;
    CDefaultAllocatorScope allocator_scope(allocator);
    for (u64 failing_request = 1u; failing_request <= 4u; ++failing_request) {
        TSharedPtr<AMeshAsset> output = sentinel;
        allocator.Begin(failing_request);
        EXPECT_FALSE(try_call(output));
        EXPECT_EQ(output.Get(), sentinel_address);
        EXPECT_EQ(allocator.RequestCount(), failing_request);
        EXPECT_EQ(allocator.OutstandingAllocationCount(), 0u);
    }

    TSharedPtr<AMeshAsset> output = sentinel;
    allocator.Begin(5u);
    EXPECT_TRUE(try_call(output));
    EXPECT_NE(output.Get(), sentinel_address);
    EXPECT_EQ(allocator.RequestCount(), 4u);
    EXPECT_EQ(allocator.OutstandingAllocationCount(), 4u);
    output.Reset();
    EXPECT_EQ(allocator.OutstandingAllocationCount(), 0u);
}

} // namespace

ACS_TEST(MeshPrimitiveSafety, PublicSignaturesAndLayoutsRemainStable)
{
    static_assert(std::is_same_v<decltype(&Primitive::MakeCube), TSharedPtr<AMeshAsset> (*)(f32) noexcept>);
    static_assert(std::is_same_v<decltype(&Primitive::MakeSphere), TSharedPtr<AMeshAsset> (*)(f32, u32, u32) noexcept>);
    static_assert(std::is_same_v<decltype(&Primitive::MakePlane), TSharedPtr<AMeshAsset> (*)(f32, f32) noexcept>);
    static_assert(std::is_same_v<decltype(&Primitive::TryMakeCube), bool (*)(f32, TSharedPtr<AMeshAsset>&) noexcept>);
    static_assert(std::is_same_v<decltype(&Primitive::TryMakeSphere), bool (*)(f32, u32, u32, TSharedPtr<AMeshAsset>&) noexcept>);
    static_assert(std::is_same_v<decltype(&Primitive::TryMakePlane), bool (*)(f32, f32, TSharedPtr<AMeshAsset>&) noexcept>);
    static_assert(std::is_same_v<decltype(&Primitive::TryMakePolygonXY), bool (*)(const FVec2*, u32, TSharedPtr<AMeshAsset>&) noexcept>);
    static_assert(sizeof(FMeshVertex) == 48u && alignof(FMeshVertex) == 16u);
    static_assert(offsetof(FMeshVertex, position) == 0u);
    static_assert(offsetof(FMeshVertex, normal) == 16u);
    static_assert(offsetof(FMeshVertex, u) == 32u);
    static_assert(offsetof(FMeshVertex, v) == 36u);
    static_assert(sizeof(FSubMesh) == 8u && alignof(FSubMesh) == 4u);
    static_assert(offsetof(FSubMesh, first_index) == 0u);
    static_assert(offsetof(FSubMesh, index_count) == 4u);
    EXPECT_TRUE(true);
}

ACS_TEST(MeshPrimitiveSafety, CubePreservesVertexAndIndexOrder)
{
    TSharedPtr<AMeshAsset> mesh;
    EXPECT_TRUE(Primitive::TryMakeCube(2.0f, mesh));
    EXPECT_TRUE(mesh.IsValid());
    if (!mesh) return;

    const AMeshAsset& asset = *mesh;
    ExpectWholeMeshRange(asset, 24u, 36u);

    /** 旧 API が生成していた二単位立方体の全頂点位置。 */
    const FVec3 positions[24] = {{-1.0f, -1.0f, -1.0f}, {1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, -1.0f}, {-1.0f, 1.0f, -1.0f}, {1.0f, -1.0f, 1.0f}, {-1.0f, -1.0f, 1.0f}, {-1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, {-1.0f, -1.0f, 1.0f}, {-1.0f, -1.0f, -1.0f}, {-1.0f, 1.0f, -1.0f}, {-1.0f, 1.0f, 1.0f}, {1.0f, -1.0f, -1.0f}, {1.0f, -1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, -1.0f}, {-1.0f, 1.0f, -1.0f}, {1.0f, 1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, {-1.0f, 1.0f, 1.0f}, {-1.0f, -1.0f, 1.0f}, {1.0f, -1.0f, 1.0f}, {1.0f, -1.0f, -1.0f}, {-1.0f, -1.0f, -1.0f}};

    /** 面ごとの外向き法線。 */
    const FVec3 normals[6] = {{0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, 1.0f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}};

    /** 各面で繰り返す UV 順序。 */
    constexpr f32 u_values[4] = {0.0f, 1.0f, 1.0f, 0.0f};
    constexpr f32 v_values[4] = {1.0f, 1.0f, 0.0f, 0.0f};
    for (usize vertex = 0u; vertex < 24u; ++vertex) {
        ExpectVertex(asset.Vertices()[vertex], positions[vertex], normals[vertex / 4u], u_values[vertex % 4u], v_values[vertex % 4u]);
    }

    for (usize face = 0u; face < 6u; ++face) {
        const u32 vertex_base = static_cast<u32>(face * 4u);
        const usize index_base = face * 6u;
        const u32 expected[6] = {vertex_base + 0u, vertex_base + 1u, vertex_base + 2u, vertex_base + 0u, vertex_base + 2u, vertex_base + 3u};
        for (usize index = 0u; index < 6u; ++index) EXPECT_EQ(asset.Indices()[index_base + index], expected[index]);
    }
}

ACS_TEST(MeshPrimitiveSafety, SpherePreservesRowMajorGeometryOrder)
{
    TSharedPtr<AMeshAsset> mesh;
    EXPECT_TRUE(Primitive::TryMakeSphere(2.0f, 3u, 2u, mesh));
    EXPECT_TRUE(mesh.IsValid());
    if (!mesh) return;

    const AMeshAsset& asset = *mesh;
    ExpectWholeMeshRange(asset, 12u, 36u);
    for (u32 ring = 0u; ring <= 2u; ++ring) {
        const f32 v = static_cast<f32>(ring) / 2.0f;
        const f32 phi = v * kPi;
        for (u32 segment = 0u; segment <= 3u; ++segment) {
            const f32 u = static_cast<f32>(segment) / 3.0f;
            const f32 theta = u * kPi * 2.0f;
            const FVec3 normal{Sin(phi) * Cos(theta), Cos(phi), Sin(phi) * Sin(theta)};
            const FVec3 position{normal.x * 2.0f, normal.y * 2.0f, normal.z * 2.0f};
            const usize vertex = static_cast<usize>(ring) * 4u + static_cast<usize>(segment);
            ExpectVertex(asset.Vertices()[vertex], position, normal, u, v);
        }
    }

    /** 3 x 2 四角形を時計回りに分割した旧 API の全インデックス順。 */
    constexpr u32 expected_indices[36] = {0u, 4u, 1u, 1u, 4u, 5u, 1u, 5u, 2u, 2u, 5u, 6u, 2u, 6u, 3u, 3u, 6u, 7u, 4u, 8u, 5u, 5u, 8u, 9u, 5u, 9u, 6u, 6u, 9u, 10u, 6u, 10u, 7u, 7u, 10u, 11u};
    for (usize index = 0u; index < 36u; ++index) EXPECT_EQ(asset.Indices()[index], expected_indices[index]);
}

ACS_TEST(MeshPrimitiveSafety, PlanePreservesVertexAndIndexOrder)
{
    TSharedPtr<AMeshAsset> mesh;
    EXPECT_TRUE(Primitive::TryMakePlane(4.0f, 6.0f, mesh));
    EXPECT_TRUE(mesh.IsValid());
    if (!mesh) return;

    const AMeshAsset& asset = *mesh;
    ExpectWholeMeshRange(asset, 4u, 6u);
    const FVec3 up{0.0f, 1.0f, 0.0f};
    ExpectVertex(asset.Vertices()[0], {-2.0f, 0.0f, -3.0f}, up, 0.0f, 1.0f);
    ExpectVertex(asset.Vertices()[1], { 2.0f, 0.0f, -3.0f}, up, 1.0f, 1.0f);
    ExpectVertex(asset.Vertices()[2], { 2.0f, 0.0f,  3.0f}, up, 1.0f, 0.0f);
    ExpectVertex(asset.Vertices()[3], {-2.0f, 0.0f,  3.0f}, up, 0.0f, 0.0f);

    constexpr u32 expected_indices[6] = {0u, 1u, 2u, 0u, 2u, 3u};
    for (usize index = 0u; index < 6u; ++index) EXPECT_EQ(asset.Indices()[index], expected_indices[index]);
}

ACS_TEST(MeshPrimitiveSafety, PolygonXYPreservesEditorFanGeometry)
{
    constexpr FVec2 points[4] = {{-2.0f, -1.0f}, {2.0f, -1.0f}, {1.0f, 3.0f}, {-1.0f, 2.0f}};
    TSharedPtr<AMeshAsset> mesh;
    EXPECT_TRUE(Primitive::TryMakePolygonXY(points, 4u, mesh));
    EXPECT_TRUE(mesh.IsValid());
    if (!mesh) return;

    ExpectWholeMeshRange(*mesh, 4u, 6u);
    for (u32 index = 0u; index < 4u; ++index)
        ExpectVertex(mesh->Vertices()[index], FVec3{points[index].x, points[index].y, 0.0f}, FVec3{0.0f, 0.0f, 1.0f}, 0.0f, 0.0f);
    constexpr u32 expected_indices[6] = {0u, 1u, 2u, 0u, 2u, 3u};
    for (u32 index = 0u; index < 6u; ++index) EXPECT_EQ(mesh->Indices()[index], expected_indices[index]);
}

ACS_TEST(MeshPrimitiveSafety, EveryAllocationFailurePreservesOutputAndLeavesNoPartialMesh)
{
    ExpectAllocationFailureMatrix([](TSharedPtr<AMeshAsset>& output) noexcept { return Primitive::TryMakeCube(1.0f, output); }, []() noexcept { return Primitive::MakeCube(1.0f); });
    ExpectAllocationFailureMatrix([](TSharedPtr<AMeshAsset>& output) noexcept { return Primitive::TryMakeSphere(1.0f, 8u, 4u, output); }, []() noexcept { return Primitive::MakeSphere(1.0f, 8u, 4u); });
    ExpectAllocationFailureMatrix([](TSharedPtr<AMeshAsset>& output) noexcept { return Primitive::TryMakePlane(1.0f, 1.0f, output); }, []() noexcept { return Primitive::MakePlane(1.0f, 1.0f); });
    constexpr FVec2 polygon[4] = {{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};
    ExpectTryAllocationFailureMatrix([&polygon](TSharedPtr<AMeshAsset>& output) noexcept { return Primitive::TryMakePolygonXY(polygon, 4u, output); });
}

ACS_TEST(MeshPrimitiveSafety, InvalidAndUnrepresentableInputsAllocateNothingAndPreserveOutput)
{
    /** 失敗時の同一性を確認する既存出力。 */
    TSharedPtr<AMeshAsset> sentinel = MakeShared<AMeshAsset>();
    EXPECT_TRUE(sentinel.IsValid());
    AMeshAsset* const sentinel_address = sentinel.Get();

    CFailOnMeshRequestAllocator allocator;
    CDefaultAllocatorScope allocator_scope(allocator);
    TSharedPtr<AMeshAsset> output = sentinel;

    /** 実行時にも保持される非数。 */
    const f32 not_a_number = std::numeric_limits<f32>::quiet_NaN();

    /** 正負の無限大。 */
    const f32 infinity = std::numeric_limits<f32>::infinity();

    allocator.Begin(0u);
    EXPECT_FALSE(Primitive::TryMakeCube(not_a_number, output));
    EXPECT_FALSE(Primitive::TryMakeCube(infinity, output));
    EXPECT_FALSE(Primitive::TryMakeCube(-infinity, output));
    EXPECT_FALSE(Primitive::TryMakePlane(not_a_number, 1.0f, output));
    EXPECT_FALSE(Primitive::TryMakePlane(1.0f, infinity, output));
    EXPECT_FALSE(Primitive::TryMakeSphere(not_a_number, 3u, 2u, output));
    EXPECT_FALSE(Primitive::TryMakeSphere(1.0f, ~u32(0), 2u, output));
    EXPECT_FALSE(Primitive::TryMakeSphere(1.0f, 3u, ~u32(0), output));
    constexpr FVec2 too_few_points[2] = {{0.0f, 0.0f}, {1.0f, 0.0f}};
    const FVec2 invalid_points[3] = {{0.0f, 0.0f}, {1.0f, not_a_number}, {0.0f, 1.0f}};
    EXPECT_FALSE(Primitive::TryMakePolygonXY(nullptr, 3u, output));
    EXPECT_FALSE(Primitive::TryMakePolygonXY(too_few_points, 2u, output));
    EXPECT_FALSE(Primitive::TryMakePolygonXY(invalid_points, 3u, output));
    EXPECT_EQ(output.Get(), sentinel_address);
    EXPECT_EQ(allocator.RequestCount(), 0u);
    EXPECT_EQ(allocator.OutstandingAllocationCount(), 0u);

    EXPECT_FALSE(Primitive::MakeCube(not_a_number).IsValid());
    EXPECT_FALSE(Primitive::MakePlane(1.0f, infinity).IsValid());
    EXPECT_FALSE(Primitive::MakeSphere(1.0f, ~u32(0), ~u32(0)).IsValid());
    EXPECT_EQ(allocator.RequestCount(), 0u);
    EXPECT_EQ(allocator.OutstandingAllocationCount(), 0u);
    output.Reset();
}

ACS_TEST(MeshPrimitiveSafety, SphereKeepsLegacyMinimumSubdivisionClamp)
{
    TSharedPtr<AMeshAsset> output;
    EXPECT_TRUE(Primitive::TryMakeSphere(1.0f, 0u, 0u, output));
    EXPECT_TRUE(output.IsValid());
    if (!output) return;
    ExpectWholeMeshRange(*output, 12u, 36u);
}

ACS_TEST(MeshPrimitiveSafety, SphereKeepsLegacyLargeSingleAxisInput)
{
    /** 旧 API で安全に生成できた 1024 超の経度分割。 */
    constexpr u32 segments = 1025u;

    /** 最小緯度分割との組み合わせで生成した互換 API の結果。 */
    TSharedPtr<AMeshAsset> output = Primitive::MakeSphere(1.0f, segments, 2u);
    EXPECT_TRUE(output.IsValid());
    if (!output) return;
    ExpectWholeMeshRange(*output, 3078u, 12300u);
}
