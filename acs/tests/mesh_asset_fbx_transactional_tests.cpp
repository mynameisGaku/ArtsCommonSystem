// SPDX-License-Identifier: Apache-2.0
#include "asset/MeshAsset.h"
#include "foundation/Move.h"
#include "memory/Memory.h"
#include "memory/SystemAllocator.h"
#include "test/Expect.h"
#include "test/Test.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <type_traits>

using namespace acs;

namespace {

/** 頂点0だけを使う単一三角形の最小ASCII FBX。 */
constexpr char kTriangleFbx[] =
    "; FBX 7.4.0 project file\n"
    "FBXHeaderExtension:  {\n"
    "    FBXHeaderVersion: 1003\n"
    "    FBXVersion: 7400\n"
    "}\n"
    "Definitions:  {\n"
    "    Version: 100\n"
    "    Count: 2\n"
    "    ObjectType: \"Model\" { Count: 1 }\n"
    "    ObjectType: \"Geometry\" { Count: 1 }\n"
    "}\n"
    "Objects:  {\n"
    "    Geometry: 1, \"Geometry::Triangle\", \"Mesh\" {\n"
    "        GeometryVersion: 124\n"
    "        Vertices: *9 { a: 2,3,4,5,3,4,2,6,4 }\n"
    "        PolygonVertexIndex: *3 { a: 0,1,-3 }\n"
    "    }\n"
    "    Model: 2, \"Model::Triangle\", \"Mesh\" {\n"
    "        Version: 232\n"
    "        Shading: T\n"
    "        Culling: \"CullingOff\"\n"
    "    }\n"
    "}\n"
    "Connections:  {\n"
    "    C: \"OO\",1,2\n"
    "}\n";

/** 三角形と四角形を含み、旧三角形化index採用順を識別できるASCII FBX。 */
constexpr char kMixedFacesFbx[] =
    "; FBX 7.4.0 project file\n"
    "FBXHeaderExtension:  {\n"
    "    FBXHeaderVersion: 1003\n"
    "    FBXVersion: 7400\n"
    "}\n"
    "Definitions:  {\n"
    "    Version: 100\n"
    "    Count: 2\n"
    "    ObjectType: \"Model\" { Count: 1 }\n"
    "    ObjectType: \"Geometry\" { Count: 1 }\n"
    "}\n"
    "Objects:  {\n"
    "    Geometry: 1, \"Geometry::LegacyFaces\", \"Mesh\" {\n"
    "        GeometryVersion: 124\n"
    "        Vertices: *21 { a: 0,0,0,1,0,0,0,1,0,10,0,0,11,0,0,11,1,0,10,1,0 }\n"
    "        PolygonVertexIndex: *7 { a: 0,1,-3,3,4,5,-7 }\n"
    "    }\n"
    "    Model: 2, \"Model::LegacyFaces\", \"Mesh\" {\n"
    "        Version: 232\n"
    "        Shading: T\n"
    "        Culling: \"CullingOff\"\n"
    "    }\n"
    "}\n"
    "Connections:  {\n"
    "    C: \"OO\",1,2\n"
    "}\n";

/** 二つのmeshの連結順とsubmesh範囲を識別できるASCII FBX。 */
constexpr char kTwoMeshesFbx[] =
    "; FBX 7.4.0 project file\n"
    "FBXHeaderExtension:  {\n"
    "    FBXHeaderVersion: 1003\n"
    "    FBXVersion: 7400\n"
    "}\n"
    "Definitions:  {\n"
    "    Version: 100\n"
    "    Count: 4\n"
    "    ObjectType: \"Model\" { Count: 2 }\n"
    "    ObjectType: \"Geometry\" { Count: 2 }\n"
    "}\n"
    "Objects:  {\n"
    "    Geometry: 1, \"Geometry::First\", \"Mesh\" {\n"
    "        GeometryVersion: 124\n"
    "        Vertices: *9 { a: 1,2,3,2,2,3,1,3,3 }\n"
    "        PolygonVertexIndex: *3 { a: 0,1,-3 }\n"
    "    }\n"
    "    Model: 2, \"Model::First\", \"Mesh\" {\n"
    "        Version: 232\n"
    "        Shading: T\n"
    "        Culling: \"CullingOff\"\n"
    "    }\n"
    "    Geometry: 3, \"Geometry::Second\", \"Mesh\" {\n"
    "        GeometryVersion: 124\n"
    "        Vertices: *9 { a: 10,20,30,11,20,30,10,21,30 }\n"
    "        PolygonVertexIndex: *3 { a: 0,1,-3 }\n"
    "    }\n"
    "    Model: 4, \"Model::Second\", \"Mesh\" {\n"
    "        Version: 232\n"
    "        Shading: T\n"
    "        Culling: \"CullingOff\"\n"
    "    }\n"
    "}\n"
    "Connections:  {\n"
    "    C: \"OO\",1,2\n"
    "    C: \"OO\",3,4\n"
    "}\n";

/** meshを持たず、旧revision 1を識別するASCII FBX。 */
constexpr char kEmptySceneFbx[] =
    "; FBX 7.4.0 project file\n"
    "FBXHeaderExtension:  {\n"
    "    FBXHeaderVersion: 1003\n"
    "    FBXVersion: 7400\n"
    "}\n"
    "Definitions:  {\n"
    "    Version: 100\n"
    "    Count: 0\n"
    "}\n"
    "Objects:  {\n"
    "}\n"
    "Connections:  {\n"
    "}\n";

/** scratch上限fixtureの頂点列より前の固定ASCII。 */
constexpr char kOversizedFacePrefix[] =
    "; FBX 7.4.0 project file\n"
    "FBXHeaderExtension:  {\n"
    "    FBXHeaderVersion: 1003\n"
    "    FBXVersion: 7400\n"
    "}\n"
    "Definitions:  {\n"
    "    Version: 100\n"
    "    Count: 2\n"
    "    ObjectType: \"Model\" { Count: 1 }\n"
    "    ObjectType: \"Geometry\" { Count: 1 }\n"
    "}\n"
    "Objects:  {\n"
    "    Geometry: 1, \"Geometry::OversizedFace\", \"Mesh\" {\n"
    "        GeometryVersion: 124\n"
    "        Vertices: *264 { a: ";

/** scratch上限fixtureのpolygon index列より後の固定ASCII。 */
constexpr char kOversizedFaceSuffix[] =
    " }\n"
    "    }\n"
    "    Model: 2, \"Model::OversizedFace\", \"Mesh\" {\n"
    "        Version: 232\n"
    "        Shading: T\n"
    "        Culling: \"CullingOff\"\n"
    "    }\n"
    "}\n"
    "Connections:  {\n"
    "    C: \"OO\",1,2\n"
    "}\n";

/** 指定した確保要求だけを失敗させ、未解放数を追跡するアロケータ。 */
class CFailOnFbxRequestAllocator final : public IAllocator {
public:
    /** 次のloadで失敗させる1始まりの要求番号を設定する。0なら失敗させない。 */
    void Begin(u64 failing_request) noexcept
    {
        m_RequestCount = 0u;
        m_FailingRequest = failing_request;
    }

    /** 現在のloadで受けた確保要求数を返す。 */
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
    /** 実際の確保と解放を行うbacking。 */
    CSystemAllocator m_Backing;

    /** 現在のloadで受けた確保要求数。 */
    u64 m_RequestCount = 0u;

    /** 失敗させる1始まりの要求番号。 */
    u64 m_FailingRequest = 0u;
};

/** テスト中だけ既定アロケータを差し替え、終了時に必ず戻す。 */
class CDefaultAllocatorScope final {
public:
    explicit CDefaultAllocatorScope(IAllocator& allocator) noexcept : m_Previous(&DefaultAllocator())
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

/** null終端ASCIIを終端なしbyte配列へ複製する。 */
TArray<byte> MakeBytes(const char* text) noexcept
{
    TArray<byte> bytes;
    if (text == nullptr) return bytes;

    /** FBX parserへ渡す終端なしbyte数。 */
    const usize length = std::strlen(text);
    if (!bytes.TrySetNum(length)) return TArray<byte>();
    if (length != 0u) MemCopy(bytes.GetData(), text, length);
    return bytes;
}

/** 固定bufferへ文字列を追記する。 */
bool TryAppendText(char* buffer, usize capacity, usize& length, const char* text) noexcept
{
    if (buffer == nullptr || text == nullptr || length > capacity) return false;

    /** 追記する文字数。 */
    const usize text_length = std::strlen(text);
    if (text_length > capacity - length) return false;
    MemCopy(buffer + length, text, text_length);
    length += text_length;
    return true;
}

/** 固定bufferへformat済みのu32値を追記する。 */
bool TryAppendNumber(char* buffer, usize capacity, usize& length, const char* format, u32 value) noexcept
{
    if (buffer == nullptr || format == nullptr || length >= capacity) return false;

    /** 終端を含む残り容量。 */
    const usize remaining = capacity - length;

    /** snprintfが要求した終端なし文字数。 */
    const int written = std::snprintf(buffer + length, remaining, format, value);
    if (written < 0 || static_cast<usize>(written) >= remaining) return false;
    length += static_cast<usize>(written);
    return true;
}

/** 固定256 index scratchを越える88角形ASCII FBXを構築する。 */
bool TryMakeOversizedFaceBytes(TArray<byte>& output) noexcept
{
    constexpr u32 vertex_count = 88u;
    constexpr usize buffer_capacity = 32768u;
    char buffer[buffer_capacity]{};
    usize length = 0u;
    if (!TryAppendText(buffer, buffer_capacity, length, kOversizedFacePrefix)) return false;

    for (u32 vertex = 0u; vertex < vertex_count; ++vertex) {
        if (!TryAppendNumber(buffer, buffer_capacity, length, vertex + 1u == vertex_count ? "%u,0,0" : "%u,0,0,", vertex)) return false;
    }
    if (!TryAppendText(buffer, buffer_capacity, length, " }\n        PolygonVertexIndex: *88 { a: ")) return false;
    for (u32 vertex = 0u; vertex < vertex_count; ++vertex) {
        if (vertex + 1u == vertex_count) {
            if (!TryAppendNumber(buffer, buffer_capacity, length, "-%u", vertex + 1u)) return false;
        } else if (!TryAppendNumber(buffer, buffer_capacity, length, "%u,", vertex)) {
            return false;
        }
    }
    if (!TryAppendText(buffer, buffer_capacity, length, kOversizedFaceSuffix)) return false;

    TArray<byte> staged;
    if (!staged.TrySetNum(length)) return false;
    MemCopy(staged.GetData(), buffer, length);
    output = Move(staged);
    return true;
}

/** 成功結果から型を検査してAMeshAssetを返す。 */
const AMeshAsset* ExpectMeshResult(const TResult<TSharedPtr<AAsset>>& result, FAssetId expected_id) noexcept
{
    EXPECT_TRUE(result.IsOk());
    if (result.IsErr()) return nullptr;

    const TSharedPtr<AAsset>& asset = result.Value();
    EXPECT_TRUE(asset.IsValid());
    if (!asset) return nullptr;
    EXPECT_EQ(asset->Type(), AMeshAsset::StaticType());
    EXPECT_EQ(asset->Id().value, expected_id.value);
    EXPECT_EQ(static_cast<u32>(asset->State()), static_cast<u32>(EAssetState::Ready));
    return static_cast<const AMeshAsset*>(asset.Get());
}

/** 頂点配列を期待する位置列とzero法線/UVへbyte単位で照合する。 */
void ExpectVertexBytes(const TArray<FMeshVertex>& vertices, const FVec3* positions, usize count) noexcept
{
    EXPECT_EQ(vertices.Num(), count);
    if (vertices.Num() != count) return;

    FMeshVertex expected[9]{};
    EXPECT_TRUE(count <= 9u);
    if (count > 9u) return;
    for (usize index = 0u; index < count; ++index) expected[index].position = positions[index];
    EXPECT_TRUE(MemCmp(vertices.GetData(), expected, sizeof(FMeshVertex) * count) == 0);
}

} // namespace

ACS_TEST(FbxLoaderTransactional, PublicSignatureAndMeshLayoutsRemainStable)
{
    static_assert(std::is_same_v<decltype(&CFbxAssetLoader::LoadFromBytes), TResult<TSharedPtr<AAsset>> (CFbxAssetLoader::*)(FAssetId, const TArray<byte>&) noexcept>);
    static_assert(std::is_same_v<FFbxAssetLoader, CFbxAssetLoader>);
    static_assert(sizeof(CFbxAssetLoader) == sizeof(void*));
    static_assert(alignof(CFbxAssetLoader) == alignof(void*));
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

ACS_TEST(FbxLoaderTransactional, PreservesLegacyMixedFaceBytesAndRevision)
{
    const TArray<byte> bytes = MakeBytes(kMixedFacesFbx);
    EXPECT_TRUE(bytes.Num() != 0u);
    if (bytes.Num() == 0u) return;

    constexpr FAssetId asset_id{0x1122334455667788ull};
    CFbxAssetLoader loader;
    const TResult<TSharedPtr<AAsset>> result = loader.LoadFromBytes(asset_id, bytes);
    const AMeshAsset* const mesh = ExpectMeshResult(result, asset_id);
    if (mesh == nullptr) return;

    constexpr FVec3 expected_positions[9] = {{0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, {11.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    ExpectVertexBytes(mesh->Vertices(), expected_positions, 9u);

    constexpr u32 expected_indices[9] = {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    EXPECT_EQ(mesh->Indices().Num(), 9u);
    if (mesh->Indices().Num() == 9u) EXPECT_TRUE(MemCmp(mesh->Indices().GetData(), expected_indices, sizeof(expected_indices)) == 0);

    constexpr FSubMesh expected_submeshes[1] = {{0u, 9u}};
    EXPECT_EQ(mesh->SubMeshes().Num(), 1u);
    if (mesh->SubMeshes().Num() == 1u) EXPECT_TRUE(MemCmp(mesh->SubMeshes().GetData(), expected_submeshes, sizeof(expected_submeshes)) == 0);
    EXPECT_EQ(mesh->GeometryRevision(), 32u);
}

ACS_TEST(FbxLoaderTransactional, PreservesEmptyTriangleAndMultiMeshRevision)
{
    CFbxAssetLoader loader;

    const TArray<byte> empty_bytes = MakeBytes(kEmptySceneFbx);
    const TResult<TSharedPtr<AAsset>> empty_result = loader.LoadFromBytes(FAssetId{1u}, empty_bytes);
    const AMeshAsset* const empty_mesh = ExpectMeshResult(empty_result, FAssetId{1u});
    if (empty_mesh != nullptr) {
        EXPECT_EQ(empty_mesh->Vertices().Num(), 0u);
        EXPECT_EQ(empty_mesh->Indices().Num(), 0u);
        EXPECT_EQ(empty_mesh->SubMeshes().Num(), 0u);
        EXPECT_EQ(empty_mesh->GeometryRevision(), 1u);
    }

    const TArray<byte> triangle_bytes = MakeBytes(kTriangleFbx);
    const TResult<TSharedPtr<AAsset>> triangle_result = loader.LoadFromBytes(FAssetId{2u}, triangle_bytes);
    const AMeshAsset* const triangle_mesh = ExpectMeshResult(triangle_result, FAssetId{2u});
    if (triangle_mesh != nullptr) {
        constexpr FVec3 triangle_positions[3] = {{2.0f, 3.0f, 4.0f}, {2.0f, 3.0f, 4.0f}, {2.0f, 3.0f, 4.0f}};
        ExpectVertexBytes(triangle_mesh->Vertices(), triangle_positions, 3u);
        constexpr u32 triangle_indices[3] = {0u, 1u, 2u};
        constexpr FSubMesh triangle_submeshes[1] = {{0u, 3u}};
        EXPECT_EQ(triangle_mesh->Indices().Num(), 3u);
        EXPECT_EQ(triangle_mesh->SubMeshes().Num(), 1u);
        if (triangle_mesh->Indices().Num() == 3u) EXPECT_TRUE(MemCmp(triangle_mesh->Indices().GetData(), triangle_indices, sizeof(triangle_indices)) == 0);
        if (triangle_mesh->SubMeshes().Num() == 1u) EXPECT_TRUE(MemCmp(triangle_mesh->SubMeshes().GetData(), triangle_submeshes, sizeof(triangle_submeshes)) == 0);
        EXPECT_EQ(triangle_mesh->GeometryRevision(), 14u);
    }

    const TArray<byte> multi_bytes = MakeBytes(kTwoMeshesFbx);
    const TResult<TSharedPtr<AAsset>> multi_result = loader.LoadFromBytes(FAssetId{3u}, multi_bytes);
    const AMeshAsset* const multi_mesh = ExpectMeshResult(multi_result, FAssetId{3u});
    if (multi_mesh != nullptr) {
        constexpr FVec3 multi_positions[6] = {{1.0f, 2.0f, 3.0f}, {1.0f, 2.0f, 3.0f}, {1.0f, 2.0f, 3.0f}, {10.0f, 20.0f, 30.0f}, {10.0f, 20.0f, 30.0f}, {10.0f, 20.0f, 30.0f}};
        ExpectVertexBytes(multi_mesh->Vertices(), multi_positions, 6u);
        constexpr u32 multi_indices[6] = {0u, 1u, 2u, 3u, 4u, 5u};
        constexpr FSubMesh multi_submeshes[2] = {{0u, 3u}, {3u, 3u}};
        EXPECT_EQ(multi_mesh->Indices().Num(), 6u);
        EXPECT_EQ(multi_mesh->SubMeshes().Num(), 2u);
        if (multi_mesh->Indices().Num() == 6u) EXPECT_TRUE(MemCmp(multi_mesh->Indices().GetData(), multi_indices, sizeof(multi_indices)) == 0);
        if (multi_mesh->SubMeshes().Num() == 2u) EXPECT_TRUE(MemCmp(multi_mesh->SubMeshes().GetData(), multi_submeshes, sizeof(multi_submeshes)) == 0);
        EXPECT_EQ(multi_mesh->GeometryRevision(), 27u);
    }
}

ACS_TEST(FbxLoaderTransactional, EveryAcsAllocationFailureReturnsMemoryAndLeaksNothing)
{
    const TArray<byte> bytes = MakeBytes(kMixedFacesFbx);
    EXPECT_TRUE(bytes.Num() != 0u);
    if (bytes.Num() == 0u) return;

    CFbxAssetLoader loader;
    CFailOnFbxRequestAllocator allocator;
    for (u64 failing_request = 1u; failing_request <= 5u; ++failing_request) {
        allocator.Begin(failing_request);
        {
            CDefaultAllocatorScope allocator_scope(allocator);
            const TResult<TSharedPtr<AAsset>> result = loader.LoadFromBytes(FAssetId{4u}, bytes);
            EXPECT_TRUE(result.IsErr());
            if (result.IsErr()) {
                EXPECT_EQ(static_cast<u16>(result.Error().category), static_cast<u16>(EErrCategory::Memory));
                EXPECT_EQ(result.Error().subcode, 402u);
            }
            EXPECT_EQ(allocator.RequestCount(), failing_request);
        }
        EXPECT_EQ(allocator.OutstandingAllocationCount(), 0u);
    }

    allocator.Begin(6u);
    {
        CDefaultAllocatorScope allocator_scope(allocator);
        const TResult<TSharedPtr<AAsset>> result = loader.LoadFromBytes(FAssetId{5u}, bytes);
        EXPECT_TRUE(result.IsOk());
        EXPECT_EQ(allocator.RequestCount(), 5u);
        EXPECT_EQ(allocator.OutstandingAllocationCount(), 4u);
    }
    EXPECT_EQ(allocator.OutstandingAllocationCount(), 0u);
}

ACS_TEST(FbxLoaderTransactional, OversizedFaceFailsBeforeAnyAcsAllocation)
{
    TArray<byte> bytes;
    EXPECT_TRUE(TryMakeOversizedFaceBytes(bytes));
    EXPECT_TRUE(bytes.Num() != 0u);
    if (bytes.Num() == 0u) return;

    CFbxAssetLoader loader;
    CFailOnFbxRequestAllocator allocator;
    allocator.Begin(0u);
    for (usize iteration = 0u; iteration < 8u; ++iteration) {
        CDefaultAllocatorScope allocator_scope(allocator);
        const TResult<TSharedPtr<AAsset>> result = loader.LoadFromBytes(FAssetId{6u}, bytes);
        EXPECT_TRUE(result.IsErr());
        if (result.IsErr()) {
            EXPECT_EQ(static_cast<u16>(result.Error().category), static_cast<u16>(EErrCategory::Asset));
            EXPECT_EQ(result.Error().subcode, 401u);
        }
        EXPECT_EQ(allocator.RequestCount(), 0u);
        EXPECT_EQ(allocator.OutstandingAllocationCount(), 0u);
    }
}
