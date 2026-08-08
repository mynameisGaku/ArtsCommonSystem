// SPDX-License-Identifier: Apache-2.0
#include "render/DebugDraw.h"
#include "foundation/Limits.h"
#include "memory/Memory.h"
#include "test/Expect.h"
#include "test/Test.h"

#include <type_traits>

namespace acs {

namespace {

/** 偽RHI資源の種類。生成・破棄順の検証に使う。 */
enum class EFakeResourceKind : u8 {
    /** 頂点シェーダ。 */
    VertexShader,
    /** ピクセルシェーダ。 */
    PixelShader,
    /** graphics pipeline。 */
    Pipeline,
    /** 動的頂点バッファ。 */
    VertexBuffer,
    /** view-projection定数バッファ。 */
    ConstantBuffer,
};

/** 偽RHI factoryの失敗位置と資源寿命を記録する固定長状態。 */
class FFakeFactoryState {
public:
    /** 1 processで追跡できる資源数の固定上限。 */
    static constexpr u32 kMaximumResources = 128u;

    /** 生存資源がない状態で全記録を初期化する。 */
    void Reset(IAllocator& resource_allocator) noexcept
    {
        m_ResourceAllocator = &resource_allocator;
        m_FactoryCallCount = 0u;
        m_FailCall = 0u;
        m_NextId = 1u;
        m_LiveCount = 0u;
        m_CreatedCount = 0u;
        m_DestroyedCount = 0u;
        m_VertexShader = nullptr;
        m_PixelShader = nullptr;
        m_PipelineDesc = FPipelineDesc{};
        m_VertexBufferDesc = FBufferDesc{};
        m_ConstantBufferDesc = FBufferDesc{};
        for (u32 i = 0u; i < kMaximumResources; ++i) {
            m_Live[i] = false;
            m_Kinds[i] = EFakeResourceKind::VertexShader;
            m_CreatedIds[i] = 0u;
            m_DestroyedIds[i] = 0u;
        }
    }

    /** 次のInit試行に対するfactory失敗位置を設定する。0は全成功。 */
    void BeginAttempt(u32 fail_call) noexcept
    {
        m_FactoryCallCount = 0u;
        m_FailCall = fail_call;
        m_CreatedCount = 0u;
        m_DestroyedCount = 0u;
        m_VertexShader = nullptr;
        m_PixelShader = nullptr;
        m_PipelineDesc = FPipelineDesc{};
        m_VertexBufferDesc = FBufferDesc{};
        m_ConstantBufferDesc = FBufferDesc{};
    }

    /** factory呼出を1回進め、指定位置なら失敗を要求する。 */
    bool ShouldFail() noexcept
    {
        ++m_FactoryCallCount;
        return m_FailCall != 0u && m_FactoryCallCount == m_FailCall;
    }

    /** 新しい資源IDを払い出す。 */
    u32 AllocateId() noexcept
    {
        return m_NextId++;
    }

    /** 構築に成功した資源を生存集合へ追加する。 */
    void OnCreated(u32 id, EFakeResourceKind kind) noexcept
    {
        if (id >= kMaximumResources || m_CreatedCount >= kMaximumResources) return;
        m_Live[id] = true;
        m_Kinds[id] = kind;
        m_CreatedIds[m_CreatedCount++] = id;
        ++m_LiveCount;
    }

    /** 破棄された資源を生存集合から外し、破棄順を記録する。 */
    void OnDestroyed(u32 id) noexcept
    {
        if (id >= kMaximumResources || !m_Live[id]) return;
        m_Live[id] = false;
        --m_LiveCount;
        if (m_DestroyedCount < kMaximumResources) m_DestroyedIds[m_DestroyedCount++] = id;
    }

    /** 偽資源本体の確保元を返す。 */
    IAllocator& ResourceAllocator() const noexcept
    {
        return *m_ResourceAllocator;
    }

    /** 現試行で成功した生成数を返す。 */
    u32 CreatedCount() const noexcept
    {
        return m_CreatedCount;
    }

    /** 現試行で生成した指定番目の資源IDを返す。 */
    u32 CreatedId(u32 index) const noexcept
    {
        return m_CreatedIds[index];
    }

    /** 現試行中に破棄された資源数を返す。 */
    u32 DestroyedCount() const noexcept
    {
        return m_DestroyedCount;
    }

    /** 現試行中に指定番目に破棄された資源IDを返す。 */
    u32 DestroyedId(u32 index) const noexcept
    {
        return m_DestroyedIds[index];
    }

    /** 現在生存する偽資源数を返す。 */
    u32 LiveCount() const noexcept
    {
        return m_LiveCount;
    }

    /** 指定IDが現在生存しているかを返す。 */
    bool IsLive(u32 id) const noexcept
    {
        return id < kMaximumResources && m_Live[id];
    }

    /** 指定IDの資源種類を返す。 */
    EFakeResourceKind Kind(u32 id) const noexcept
    {
        return m_Kinds[id];
    }

    /** 現試行のfactory呼出数を返す。 */
    u32 FactoryCallCount() const noexcept
    {
        return m_FactoryCallCount;
    }

    /** 現試行で生成したシェーダのアドレスを記録する。 */
    void RecordShader(EShaderStage stage, IRhiShader* shader) noexcept
    {
        if (stage == EShaderStage::Vertex)
            m_VertexShader = shader;
        else if (stage == EShaderStage::Pixel)
            m_PixelShader = shader;
    }

    /** 現試行のgraphics pipeline記述を保存する。 */
    void RecordPipeline(const FPipelineDesc& desc) noexcept
    {
        m_PipelineDesc = desc;
    }

    /** 現試行の頂点または定数バッファ記述を保存する。 */
    void RecordBuffer(const FBufferDesc& desc) noexcept
    {
        if (desc.usage == EBufferUsage::Vertex)
            m_VertexBufferDesc = desc;
        else if (desc.usage == EBufferUsage::Uniform)
            m_ConstantBufferDesc = desc;
    }

    /** 現試行で生成した頂点シェーダを返す。 */
    IRhiShader* VertexShader() const noexcept
    {
        return m_VertexShader;
    }

    /** 現試行で生成したピクセルシェーダを返す。 */
    IRhiShader* PixelShader() const noexcept
    {
        return m_PixelShader;
    }

    /** 現試行のpipeline記述を返す。 */
    const FPipelineDesc& PipelineDesc() const noexcept
    {
        return m_PipelineDesc;
    }

    /** 現試行の頂点バッファ記述を返す。 */
    const FBufferDesc& VertexBufferDesc() const noexcept
    {
        return m_VertexBufferDesc;
    }

    /** 現試行の定数バッファ記述を返す。 */
    const FBufferDesc& ConstantBufferDesc() const noexcept
    {
        return m_ConstantBufferDesc;
    }

private:
    /** 偽資源本体だけに使う失敗しない確保元。 */
    IAllocator* m_ResourceAllocator = nullptr;
    /** 現Init試行のfactory呼出数。 */
    u32 m_FactoryCallCount = 0u;
    /** 失敗させるfactory呼出位置。0は失敗なし。 */
    u32 m_FailCall = 0u;
    /** 次に払い出す一意な資源ID。 */
    u32 m_NextId = 1u;
    /** 現在生存する資源数。 */
    u32 m_LiveCount = 0u;
    /** 現Init試行で生成した資源数。 */
    u32 m_CreatedCount = 0u;
    /** 現Init試行中に破棄された資源数。 */
    u32 m_DestroyedCount = 0u;
    /** 資源IDごとの生存状態。 */
    bool m_Live[kMaximumResources] = {};
    /** 資源IDごとの種類。 */
    EFakeResourceKind m_Kinds[kMaximumResources] = {};
    /** 現Init試行の生成順ID。 */
    u32 m_CreatedIds[kMaximumResources] = {};
    /** 現Init試行の破棄順ID。 */
    u32 m_DestroyedIds[kMaximumResources] = {};
    /** 現Init試行で生成した頂点シェーダ。 */
    IRhiShader* m_VertexShader = nullptr;
    /** 現Init試行で生成したピクセルシェーダ。 */
    IRhiShader* m_PixelShader = nullptr;
    /** pipeline factoryへ渡された記述。 */
    FPipelineDesc m_PipelineDesc{};
    /** 頂点バッファfactoryへ渡された記述。 */
    FBufferDesc m_VertexBufferDesc{};
    /** 定数バッファfactoryへ渡された記述。 */
    FBufferDesc m_ConstantBufferDesc{};
};

/** 専用test process内だけで共有するfactory状態。 */
FFakeFactoryState g_FactoryState;

/** 生成・破棄を固定長状態へ通知する偽シェーダ。 */
class CFakeShader final : public IRhiShader {
public:
    CFakeShader(u32 id, EShaderStage stage, EFakeResourceKind kind) noexcept : m_Id(id), m_Stage(stage)
    {
        g_FactoryState.OnCreated(m_Id, kind);
    }

    ~CFakeShader() noexcept override
    {
        g_FactoryState.OnDestroyed(m_Id);
    }

    EShaderStage Stage() const noexcept override
    {
        return m_Stage;
    }
    const byte* Bytecode() const noexcept override
    {
        return nullptr;
    }
    usize BytecodeSize() const noexcept override
    {
        return 0u;
    }

private:
    /** 寿命追跡に使う資源ID。 */
    u32 m_Id = 0u;
    /** factoryで指定されたシェーダ段階。 */
    EShaderStage m_Stage = EShaderStage::Vertex;
};

/** 生成・破棄を固定長状態へ通知する偽パイプライン。 */
class CFakePipeline final : public IRhiPipeline {
public:
    explicit CFakePipeline(u32 id) noexcept : m_Id(id)
    {
        g_FactoryState.OnCreated(m_Id, EFakeResourceKind::Pipeline);
    }

    ~CFakePipeline() noexcept override
    {
        g_FactoryState.OnDestroyed(m_Id);
    }

private:
    /** 寿命追跡に使う資源ID。 */
    u32 m_Id = 0u;
};

/** 生成・破棄を固定長状態へ通知する偽バッファ。 */
class CFakeBuffer final : public IRhiBuffer {
public:
    CFakeBuffer(u32 id, const FBufferDesc& desc, EFakeResourceKind kind) noexcept
        : m_Id(id), m_Size(desc.size), m_Usage(desc.usage)
    {
        g_FactoryState.OnCreated(m_Id, kind);
    }

    ~CFakeBuffer() noexcept override
    {
        g_FactoryState.OnDestroyed(m_Id);
    }

    usize Size() const noexcept override
    {
        return m_Size;
    }
    EBufferUsage Usage() const noexcept override
    {
        return m_Usage;
    }
    void Update(const void*, usize, usize) noexcept override
    {
    }

private:
    /** 寿命追跡に使う資源ID。 */
    u32 m_Id = 0u;
    /** factoryで指定されたバッファサイズ。 */
    usize m_Size = 0u;
    /** factoryで指定されたバッファ用途。 */
    EBufferUsage m_Usage = EBufferUsage::Vertex;
};

/** 生成関数へ渡す最小の偽RHIデバイス。 */
class CFakeRhiDevice final : public IRhiDevice {
public:
    const char* BackendName() const noexcept override
    {
        return "DebugDrawTest";
    }
    const char* AdapterName() const noexcept override
    {
        return "DebugDrawTest";
    }
    void WaitIdle() noexcept override
    {
    }
};

/** 通常確保後にCPU頂点確保だけを決定論的に失敗させるallocator。 */
class CSwitchableFailAllocator final : public IAllocator {
public:
    explicit CSwitchableFailAllocator(IAllocator& backing) noexcept : m_Backing(&backing)
    {
    }

    /** 以後の確保を失敗させるかを設定する。 */
    void SetFailing(bool failing) noexcept
    {
        m_Failing = failing;
    }

    void* Alloc(usize size, usize alignment, FSourceLoc location) noexcept override
    {
        return m_Failing ? nullptr : m_Backing->Alloc(size, alignment, location);
    }

    void Free(void* pointer) noexcept override
    {
        m_Backing->Free(pointer);
    }

private:
    /** 通常時の確保と全解放を委譲するallocator。 */
    IAllocator* m_Backing = nullptr;
    /** trueならAllocだけを失敗させる。 */
    bool m_Failing = false;
};

/** scope末尾で既定allocatorを元へ戻す。後から宣言した対象を先に破棄する。 */
class CDefaultAllocatorScope {
public:
    explicit CDefaultAllocatorScope(IAllocator& replacement) noexcept : m_Previous(&DefaultAllocator())
    {
        SetDefaultAllocator(&replacement);
    }

    ~CDefaultAllocatorScope() noexcept
    {
        SetDefaultAllocator(m_Previous);
    }

private:
    /** scope開始前の既定allocator。 */
    IAllocator* m_Previous = nullptr;
};

/** 5個の基準資源IDを保持する。 */
struct FResourceIds {
    /** VS、PS、Pipeline、VB、CBの生成順ID。 */
    u32 values[5] = {};
};

/** 現試行で生成された5資源のIDを取得する。 */
FResourceIds CaptureFiveCreatedIds() noexcept
{
    FResourceIds ids{};
    if (g_FactoryState.CreatedCount() != 5u) return ids;
    for (u32 i = 0u; i < 5u; ++i)
        ids.values[i] = g_FactoryState.CreatedId(i);
    return ids;
}

/** 基準5資源だけが生存しているかを返す。 */
bool AreOnlyBaselineResourcesLive(const FResourceIds& ids) noexcept
{
    if (g_FactoryState.LiveCount() != 5u) return false;
    for (u32 i = 0u; i < 5u; ++i) {
        if (!g_FactoryState.IsLive(ids.values[i])) return false;
    }
    return true;
}

/** 2つのnull終端文字列が一致するかを返す。 */
bool TextEquals(const char* left, const char* right) noexcept
{
    if (!left || !right) return left == right;
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

using FLineSignature = void (FDebugDraw3D::*)(FVec3, FVec3, FVec4) noexcept;
using FAabbSignature = void (FDebugDraw3D::*)(const FAabb3&, FVec4) noexcept;
using FWireframeSignature = void (FDebugDraw3D::*)(const FVec3*, u32, const u32*, u32, FVec4) noexcept;
using FTryLineSignature = bool (FDebugDraw3D::*)(FVec3, FVec3, FVec4) noexcept;
using FTryAabbSignature = bool (FDebugDraw3D::*)(const FAabb3&, FVec4) noexcept;
using FTryWireframeSignature = bool (FDebugDraw3D::*)(const FVec3*, u32, const u32*, u32, FVec4) noexcept;

static_assert(std::is_default_constructible_v<FDebugDraw3D>);
static_assert(!std::is_copy_constructible_v<FDebugDraw3D>);
static_assert(!std::is_copy_assignable_v<FDebugDraw3D>);
static_assert(std::is_same_v<decltype(&FDebugDraw3D::Line), FLineSignature>);
static_assert(std::is_same_v<decltype(&FDebugDraw3D::Aabb), FAabbSignature>);
static_assert(std::is_same_v<decltype(&FDebugDraw3D::Wireframe), FWireframeSignature>);
static_assert(std::is_same_v<decltype(&FDebugDraw3D::TryLine), FTryLineSignature>);
static_assert(std::is_same_v<decltype(&FDebugDraw3D::TryAabb), FTryAabbSignature>);
static_assert(std::is_same_v<decltype(&FDebugDraw3D::TryWireframe), FTryWireframeSignature>);
static_assert(sizeof(FDebugDraw3D) == 120u);
static_assert(alignof(FDebugDraw3D) == 8u);

} // namespace

/** 専用テストの失敗位置に従って偽シェーダを生成する。 */
TResult<TUniquePtr<IRhiShader>> CreateRhiShader(IRhiDevice&, const FShaderDesc& desc) noexcept
{
    if (g_FactoryState.ShouldFail()) return ACS_ERR(Render, 990, "debug draw test shader failure");

    const EFakeResourceKind kind = desc.stage == EShaderStage::Vertex ? EFakeResourceKind::VertexShader
                                                                      : EFakeResourceKind::PixelShader;
    auto concrete = MakeUniqueIn<CFakeShader>(g_FactoryState.ResourceAllocator(), g_FactoryState.AllocateId(),
                                              desc.stage, kind);
    if (!concrete) return ACS_ERR(Memory, 993, "debug draw test shader allocation failure");
    TUniquePtr<IRhiShader> resource(concrete.Release(), concrete.GetAllocator());
    g_FactoryState.RecordShader(desc.stage, resource.Get());
    return TResult<TUniquePtr<IRhiShader>>(OkInit, Move(resource));
}

/** 専用テストの失敗位置に従って偽パイプラインを生成する。 */
TResult<TUniquePtr<IRhiPipeline>> CreateRhiPipeline(IRhiDevice&, const FPipelineDesc& desc) noexcept
{
    if (g_FactoryState.ShouldFail()) return ACS_ERR(Render, 991, "debug draw test pipeline failure");

    g_FactoryState.RecordPipeline(desc);
    auto concrete = MakeUniqueIn<CFakePipeline>(g_FactoryState.ResourceAllocator(), g_FactoryState.AllocateId());
    if (!concrete) return ACS_ERR(Memory, 994, "debug draw test pipeline allocation failure");
    TUniquePtr<IRhiPipeline> resource(concrete.Release(), concrete.GetAllocator());
    return TResult<TUniquePtr<IRhiPipeline>>(OkInit, Move(resource));
}

/** 専用テストの失敗位置に従って偽バッファを生成する。 */
TResult<TUniquePtr<IRhiBuffer>> CreateRhiBuffer(IRhiDevice&, const FBufferDesc& desc) noexcept
{
    if (g_FactoryState.ShouldFail()) return ACS_ERR(Render, 992, "debug draw test buffer failure");

    g_FactoryState.RecordBuffer(desc);
    const EFakeResourceKind kind = desc.usage == EBufferUsage::Vertex ? EFakeResourceKind::VertexBuffer
                                                                      : EFakeResourceKind::ConstantBuffer;
    auto concrete = MakeUniqueIn<CFakeBuffer>(g_FactoryState.ResourceAllocator(), g_FactoryState.AllocateId(), desc,
                                              kind);
    if (!concrete) return ACS_ERR(Memory, 995, "debug draw test buffer allocation failure");
    TUniquePtr<IRhiBuffer> resource(concrete.Release(), concrete.GetAllocator());
    return TResult<TUniquePtr<IRhiBuffer>>(OkInit, Move(resource));
}

ACS_TEST(DebugDraw3DSafety, FactoryFailuresPreserveResourcesVerticesAndCapacity)
{
    IAllocator& resource_allocator = DefaultAllocator();
    EXPECT_EQ(g_FactoryState.LiveCount(), 0u);
    g_FactoryState.Reset(resource_allocator);

    CFakeRhiDevice device;
    {
        FDebugDraw3D debug_draw;
        g_FactoryState.BeginAttempt(0u);
        EXPECT_TRUE(debug_draw.Init(device, EFormat::B8G8R8A8_UNorm, 1u).IsOk());
        EXPECT_EQ(g_FactoryState.CreatedCount(), 5u);
        const FResourceIds baseline_ids = CaptureFiveCreatedIds();
        EXPECT_TRUE(AreOnlyBaselineResourcesLive(baseline_ids));

        for (u32 fail_call = 1u; fail_call <= 5u; ++fail_call) {
            debug_draw.Begin();
            EXPECT_TRUE(debug_draw.TryLine({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}));
            g_FactoryState.BeginAttempt(fail_call);

            const TResult<void> result = debug_draw.Init(device, EFormat::B8G8R8A8_UNorm, 16u);
            EXPECT_TRUE(result.IsErr());
            if (result.IsErr()) {
                const u16 expected_subcode = fail_call <= 2u ? 990u : (fail_call == 3u ? 991u : 992u);
                EXPECT_EQ(result.Error().subcode, expected_subcode);
            }
            EXPECT_EQ(g_FactoryState.FactoryCallCount(), fail_call);
            EXPECT_EQ(debug_draw.LineCount(), 1u);
            EXPECT_TRUE(AreOnlyBaselineResourcesLive(baseline_ids));
            EXPECT_EQ(g_FactoryState.CreatedCount(), fail_call - 1u);
            EXPECT_EQ(g_FactoryState.DestroyedCount(), fail_call - 1u);
            for (u32 i = 0u; i < g_FactoryState.CreatedCount(); ++i) {
                EXPECT_FALSE(g_FactoryState.IsLive(g_FactoryState.CreatedId(i)));
                EXPECT_EQ(g_FactoryState.DestroyedId(i),
                          g_FactoryState.CreatedId(g_FactoryState.CreatedCount() - 1u - i));
            }

            EXPECT_FALSE(debug_draw.TryLine({0.0f, 2.0f, 0.0f}, {1.0f, 2.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}));
            debug_draw.Line({0.0f, 3.0f, 0.0f}, {1.0f, 3.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f});
            EXPECT_EQ(debug_draw.LineCount(), 1u);
        }
    }
    EXPECT_EQ(g_FactoryState.LiveCount(), 0u);
}

ACS_TEST(DebugDraw3DSafety, CpuAllocationFailurePreservesOldStateBeforeFactories)
{
    IAllocator& resource_allocator = DefaultAllocator();
    EXPECT_EQ(g_FactoryState.LiveCount(), 0u);
    g_FactoryState.Reset(resource_allocator);
    CSwitchableFailAllocator failing_allocator(resource_allocator);
    CFakeRhiDevice device;

    {
        CDefaultAllocatorScope allocator_scope(failing_allocator);
        FDebugDraw3D debug_draw;
        g_FactoryState.BeginAttempt(0u);
        EXPECT_TRUE(debug_draw.Init(device, EFormat::B8G8R8A8_UNorm, 1u).IsOk());
        const FResourceIds baseline_ids = CaptureFiveCreatedIds();
        EXPECT_TRUE(debug_draw.TryLine({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}));

        failing_allocator.SetFailing(true);
        g_FactoryState.BeginAttempt(0u);
        const TResult<void> result = debug_draw.Init(device, EFormat::B8G8R8A8_UNorm, 16u);
        EXPECT_TRUE(result.IsErr());
        if (result.IsErr()) {
            EXPECT_EQ(result.Error().category, EErrCategory::Memory);
            EXPECT_EQ(result.Error().subcode, static_cast<u16>(302u));
        }
        EXPECT_EQ(g_FactoryState.FactoryCallCount(), 0u);
        EXPECT_EQ(g_FactoryState.CreatedCount(), 0u);
        EXPECT_EQ(g_FactoryState.DestroyedCount(), 0u);
        EXPECT_EQ(debug_draw.LineCount(), 1u);
        EXPECT_TRUE(AreOnlyBaselineResourcesLive(baseline_ids));
        EXPECT_FALSE(debug_draw.TryLine({0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}));
        failing_allocator.SetFailing(false);
    }
    EXPECT_EQ(g_FactoryState.LiveCount(), 0u);
}

ACS_TEST(DebugDraw3DSafety, OverflowFailurePreservesOldStateBeforeFactories)
{
    IAllocator& resource_allocator = DefaultAllocator();
    EXPECT_EQ(g_FactoryState.LiveCount(), 0u);
    g_FactoryState.Reset(resource_allocator);
    CFakeRhiDevice device;

    {
        FDebugDraw3D debug_draw;
        g_FactoryState.BeginAttempt(0u);
        EXPECT_TRUE(debug_draw.Init(device, EFormat::B8G8R8A8_UNorm, 1u).IsOk());
        const FResourceIds baseline_ids = CaptureFiveCreatedIds();
        EXPECT_TRUE(debug_draw.TryLine({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}));

        g_FactoryState.BeginAttempt(0u);
        const TResult<void> result = debug_draw.Init(device, EFormat::B8G8R8A8_UNorm, TNumLimits<u32>::Max());
        EXPECT_TRUE(result.IsErr());
        if (result.IsErr()) {
            EXPECT_EQ(result.Error().category, EErrCategory::Render);
            EXPECT_EQ(result.Error().subcode, static_cast<u16>(300u));
        }
        EXPECT_EQ(g_FactoryState.FactoryCallCount(), 0u);
        EXPECT_EQ(debug_draw.LineCount(), 1u);
        EXPECT_TRUE(AreOnlyBaselineResourcesLive(baseline_ids));
        EXPECT_FALSE(debug_draw.TryLine({0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}));
    }
    EXPECT_EQ(g_FactoryState.LiveCount(), 0u);
}

ACS_TEST(DebugDraw3DSafety, SuccessfulReinitAndShutdownUseDefinedReleaseOrder)
{
    IAllocator& resource_allocator = DefaultAllocator();
    EXPECT_EQ(g_FactoryState.LiveCount(), 0u);
    g_FactoryState.Reset(resource_allocator);
    CFakeRhiDevice device;

    FDebugDraw3D debug_draw;
    g_FactoryState.BeginAttempt(0u);
    EXPECT_TRUE(debug_draw.Init(device, EFormat::B8G8R8A8_UNorm, 2u).IsOk());
    const FResourceIds old_ids = CaptureFiveCreatedIds();
    EXPECT_TRUE(debug_draw.TryLine({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}));

    g_FactoryState.BeginAttempt(0u);
    EXPECT_TRUE(debug_draw.Init(device, EFormat::B8G8R8A8_UNorm, 3u).IsOk());
    const FResourceIds new_ids = CaptureFiveCreatedIds();
    EXPECT_EQ(debug_draw.LineCount(), 0u);
    EXPECT_EQ(g_FactoryState.LiveCount(), 5u);
    EXPECT_EQ(g_FactoryState.DestroyedCount(), 5u);
    EXPECT_EQ(g_FactoryState.DestroyedId(0u), old_ids.values[2]);
    EXPECT_EQ(g_FactoryState.DestroyedId(1u), old_ids.values[4]);
    EXPECT_EQ(g_FactoryState.DestroyedId(2u), old_ids.values[3]);
    EXPECT_EQ(g_FactoryState.DestroyedId(3u), old_ids.values[1]);
    EXPECT_EQ(g_FactoryState.DestroyedId(4u), old_ids.values[0]);
    EXPECT_TRUE(AreOnlyBaselineResourcesLive(new_ids));

    g_FactoryState.BeginAttempt(0u);
    debug_draw.Shutdown();
    EXPECT_EQ(g_FactoryState.DestroyedCount(), 5u);
    EXPECT_EQ(g_FactoryState.DestroyedId(0u), new_ids.values[2]);
    EXPECT_EQ(g_FactoryState.DestroyedId(1u), new_ids.values[4]);
    EXPECT_EQ(g_FactoryState.DestroyedId(2u), new_ids.values[3]);
    EXPECT_EQ(g_FactoryState.DestroyedId(3u), new_ids.values[1]);
    EXPECT_EQ(g_FactoryState.DestroyedId(4u), new_ids.values[0]);
    EXPECT_EQ(g_FactoryState.LiveCount(), 0u);
    EXPECT_FALSE(debug_draw.TryLine({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}));
}

ACS_TEST(DebugDraw3DSafety, ZeroLineLimitNormalizesAndUsesStagedShaderDescriptors)
{
    IAllocator& resource_allocator = DefaultAllocator();
    EXPECT_EQ(g_FactoryState.LiveCount(), 0u);
    g_FactoryState.Reset(resource_allocator);
    CFakeRhiDevice device;

    {
        FDebugDraw3D debug_draw;
        g_FactoryState.BeginAttempt(0u);
        EXPECT_TRUE(debug_draw.Init(device, EFormat::B8G8R8A8_UNorm, 0u).IsOk());
        EXPECT_EQ(g_FactoryState.CreatedCount(), 5u);
        EXPECT_EQ(g_FactoryState.Kind(g_FactoryState.CreatedId(0u)), EFakeResourceKind::VertexShader);
        EXPECT_EQ(g_FactoryState.Kind(g_FactoryState.CreatedId(1u)), EFakeResourceKind::PixelShader);
        EXPECT_EQ(g_FactoryState.Kind(g_FactoryState.CreatedId(2u)), EFakeResourceKind::Pipeline);
        EXPECT_EQ(g_FactoryState.Kind(g_FactoryState.CreatedId(3u)), EFakeResourceKind::VertexBuffer);
        EXPECT_EQ(g_FactoryState.Kind(g_FactoryState.CreatedId(4u)), EFakeResourceKind::ConstantBuffer);

        const FPipelineDesc& pipeline = g_FactoryState.PipelineDesc();
        EXPECT_TRUE(pipeline.vs == g_FactoryState.VertexShader());
        EXPECT_TRUE(pipeline.ps == g_FactoryState.PixelShader());
        EXPECT_EQ(pipeline.topology, EPrimitiveTopology::LineList);
        EXPECT_EQ(pipeline.rt_format, EFormat::B8G8R8A8_UNorm);
        EXPECT_EQ(pipeline.depth_format, EFormat::Unknown);
        EXPECT_FALSE(pipeline.depth_test);
        EXPECT_FALSE(pipeline.depth_write);
        EXPECT_EQ(pipeline.cull_mode, ECullMode::None);
        EXPECT_EQ(pipeline.blend_mode, EBlendMode::AlphaBlend);
        EXPECT_EQ(pipeline.cbuffer_slots, 1u);
        EXPECT_EQ(pipeline.texture_slots, 0u);
        EXPECT_TRUE(TextEquals(pipeline.cbuffer_names[0], "DebugCb"));
        EXPECT_EQ(pipeline.vertex_stride, 28u);
        EXPECT_EQ(pipeline.layout_count, 2u);
        EXPECT_EQ(pipeline.layout[0].offset, 0u);
        EXPECT_EQ(pipeline.layout[1].offset, 12u);
        EXPECT_EQ(pipeline.layout[0].format, EFormat::R32G32B32_Float);
        EXPECT_EQ(pipeline.layout[1].format, EFormat::R32G32B32A32_Float);

        const FBufferDesc& vertex_buffer = g_FactoryState.VertexBufferDesc();
        EXPECT_EQ(vertex_buffer.size, static_cast<usize>(56u));
        EXPECT_EQ(vertex_buffer.usage, EBufferUsage::Vertex);
        EXPECT_TRUE(vertex_buffer.cpu_writable);
        const FBufferDesc& constant_buffer = g_FactoryState.ConstantBufferDesc();
        EXPECT_EQ(constant_buffer.size, static_cast<usize>(256u));
        EXPECT_EQ(constant_buffer.usage, EBufferUsage::Uniform);
        EXPECT_TRUE(constant_buffer.cpu_writable);

        EXPECT_TRUE(debug_draw.TryLine({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}));
        EXPECT_FALSE(debug_draw.TryLine({0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}));
        EXPECT_EQ(debug_draw.LineCount(), 1u);
    }
    EXPECT_EQ(g_FactoryState.LiveCount(), 0u);
}

ACS_TEST(DebugDraw3DSafety, PrimitiveWritesAreZeroOrComplete)
{
    IAllocator& resource_allocator = DefaultAllocator();
    EXPECT_EQ(g_FactoryState.LiveCount(), 0u);
    g_FactoryState.Reset(resource_allocator);
    CFakeRhiDevice device;
    FDebugDraw3D debug_draw;
    const FVec4 color{1.0f, 0.5f, 0.25f, 1.0f};
    const FAabb3 box({0.0f, 0.0f, 0.0f}, {1.0f, 2.0f, 3.0f});

    g_FactoryState.BeginAttempt(0u);
    EXPECT_TRUE(debug_draw.Init(device, EFormat::B8G8R8A8_UNorm, 1u).IsOk());
    EXPECT_TRUE(debug_draw.TryLine({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, color));
    EXPECT_EQ(debug_draw.LineCount(), 1u);
    EXPECT_FALSE(debug_draw.TryLine({0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, color));
    debug_draw.Line({0.0f, 2.0f, 0.0f}, {1.0f, 2.0f, 0.0f}, color);
    EXPECT_EQ(debug_draw.LineCount(), 1u);

    g_FactoryState.BeginAttempt(0u);
    EXPECT_TRUE(debug_draw.Init(device, EFormat::B8G8R8A8_UNorm, 11u).IsOk());
    EXPECT_FALSE(debug_draw.TryAabb(box, color));
    EXPECT_EQ(debug_draw.LineCount(), 0u);
    debug_draw.Aabb(box, color);
    EXPECT_EQ(debug_draw.LineCount(), 0u);

    g_FactoryState.BeginAttempt(0u);
    EXPECT_TRUE(debug_draw.Init(device, EFormat::B8G8R8A8_UNorm, 12u).IsOk());
    EXPECT_TRUE(debug_draw.TryAabb(box, color));
    EXPECT_EQ(debug_draw.LineCount(), 12u);
}

ACS_TEST(DebugDraw3DSafety, WireframePreservesSkippedTrianglesAndRemainder)
{
    IAllocator& resource_allocator = DefaultAllocator();
    EXPECT_EQ(g_FactoryState.LiveCount(), 0u);
    g_FactoryState.Reset(resource_allocator);
    CFakeRhiDevice device;
    FDebugDraw3D debug_draw;
    const FVec4 color{0.25f, 1.0f, 0.5f, 1.0f};
    const FVec3 positions[4] = {
        {0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {1.0f, 1.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
    };
    const u32 indices[11] = {0u, 1u, 2u, 0u, 4u, 2u, 0u, 2u, 3u, 1u, 2u};

    g_FactoryState.BeginAttempt(0u);
    EXPECT_TRUE(debug_draw.Init(device, EFormat::B8G8R8A8_UNorm, 5u).IsOk());
    EXPECT_FALSE(debug_draw.TryWireframe(nullptr, 0u, nullptr, 0u, color));
    EXPECT_FALSE(debug_draw.TryWireframe(nullptr, 0u, nullptr, 2u, color));
    EXPECT_TRUE(debug_draw.TryWireframe(positions, 4u, indices, 0u, color));
    EXPECT_TRUE(debug_draw.TryWireframe(positions, 4u, indices, 2u, color));
    EXPECT_FALSE(debug_draw.TryWireframe(nullptr, 0u, indices, 3u, color));
    EXPECT_EQ(debug_draw.LineCount(), 0u);
    EXPECT_FALSE(debug_draw.TryWireframe(positions, 4u, indices, 11u, color));
    EXPECT_EQ(debug_draw.LineCount(), 0u);
    debug_draw.Wireframe(positions, 4u, indices, 11u, color);
    EXPECT_EQ(debug_draw.LineCount(), 0u);

    g_FactoryState.BeginAttempt(0u);
    EXPECT_TRUE(debug_draw.Init(device, EFormat::B8G8R8A8_UNorm, 6u).IsOk());
    EXPECT_TRUE(debug_draw.TryWireframe(positions, 4u, indices, 11u, color));
    EXPECT_EQ(debug_draw.LineCount(), 6u);
    debug_draw.Begin();
    debug_draw.Wireframe(positions, 4u, indices, 11u, color);
    EXPECT_EQ(debug_draw.LineCount(), 6u);
}

} // namespace acs
