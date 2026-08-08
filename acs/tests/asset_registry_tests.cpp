// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "asset/AssetRegistry.h"
#include "asset/BinaryAsset.h"
#include "foundation/Limits.h"
#include "memory/Memory.h"
#include "memory/SystemAllocator.h"
#include "platform/FileSystem.h"
#include "threading/Atomic.h"
#include "threading/Thread.h"
#include "threading/ThreadPool.h"

using namespace acs;

namespace {

constexpr const wchar_t* kAsyncAssetPath = L"acs_asset_registry_lifetime_test.tmp";

/** cache公開失敗と並行公開を検証する一時asset path。 */
constexpr const wchar_t* kTransactionalAssetPath = L"acs_asset_registry_transactional_test.tmp";

class CBlockingAssetLoader final : public IAssetLoader {
public:
    AssetType TypeId() const noexcept override
    {
        return ABinaryAsset::StaticType();
    }
    const char* Extension() const noexcept override
    {
        return "tmp";
    }

    TResult<TSharedPtr<AAsset>> LoadFromBytes(FAssetId, const TArray<byte>&) noexcept override
    {
        entered.FetchAdd(1);
        while (release.Load(EMemoryOrder::Acquire) == 0)
            SleepMs(1);

        auto binary = MakeShared<ABinaryAsset>();
        if (!binary.Get()) return ACS_ERR(Memory, 220, "BlockingAssetLoader allocation failed");
        TSharedPtr<AAsset> asset(Move(binary));
        return TResult<TSharedPtr<AAsset>>(OkInit, Move(asset));
    }

    TAtomic<u32> entered{0};
    TAtomic<u32> release{0};
};

/** 任意時点から新規確保だけを拒否できるregistry用allocator。 */
class CSwitchableFailAllocator final : public IAllocator {
public:
    /** 新規確保の拒否状態を切り替える。 */
    void SetFail(bool Fail) noexcept
    {
        m_Fail.Store(Fail ? 1u : 0u, EMemoryOrder::Release);
    }

    /** 拒否中はnull、それ以外はsystem allocatorの確保結果を返す。 */
    void* Alloc(usize Size, usize Alignment, FSourceLoc Location) noexcept override
    {
        if (m_Fail.Load(EMemoryOrder::Acquire) != 0u) return nullptr;
        return m_Backing.Alloc(Size, Alignment, Location);
    }

    /** 既存領域は拒否状態によらず元のallocatorへ返す。 */
    void Free(void* Pointer) noexcept override
    {
        m_Backing.Free(Pointer);
    }

private:
    /** 実際の確保と解放を担当するallocator。 */
    CSystemAllocator m_Backing;

    /** 1なら新規確保を拒否する。 */
    TAtomic<u32> m_Fail{0u};
};

/** test区間だけdefault allocatorを差し替えて終了時に必ず戻すguard。 */
class CDefaultAllocatorScope final {
public:
    /** 差し替え前allocatorを保持して指定allocatorを公開する。 */
    explicit CDefaultAllocatorScope(IAllocator& Allocator) noexcept : m_Previous(&DefaultAllocator())
    {
        SetDefaultAllocator(&Allocator);
    }

    /** 差し替え前allocatorを復元する。 */
    ~CDefaultAllocatorScope() noexcept
    {
        SetDefaultAllocator(m_Previous);
    }

    /** 二重復元を避けるためcopyを禁止する。 */
    CDefaultAllocatorScope(const CDefaultAllocatorScope&) = delete;

    /** guardのcopy代入を禁止する。 */
    CDefaultAllocatorScope& operator=(const CDefaultAllocatorScope&) = delete;

private:
    /** 復元するdefault allocator。 */
    IAllocator* m_Previous = nullptr;
};

/** loader外でも保持した同一assetを返し、必要時は公開直前で停止するloader。 */
class CRetainedAssetLoader final : public IAssetLoader {
public:
    /** 返却するretained assetを共有して構築する。 */
    explicit CRetainedAssetLoader(TSharedPtr<AAsset> Asset) noexcept : m_Asset(Move(Asset))
    {
    }

    /** binary asset型を返す。 */
    AssetType TypeId() const noexcept override
    {
        return ABinaryAsset::StaticType();
    }

    /** test専用拡張子を返す。 */
    const char* Extension() const noexcept override
    {
        return "tmp";
    }

    /** retained assetを返し、block中はreleaseまで待つ。 */
    TResult<TSharedPtr<AAsset>> LoadFromBytes(FAssetId, const TArray<byte>&) noexcept override
    {
        entered.FetchAdd(1u);
        while (block.Load(EMemoryOrder::Acquire) != 0u && release.Load(EMemoryOrder::Acquire) == 0u) {
            SleepMs(1u);
        }
        return TResult<TSharedPtr<AAsset>>(OkInit, m_Asset);
    }

    /** loaderへ入った回数。 */
    TAtomic<u32> entered{0u};

    /** 1ならreleaseまで返却を止める。 */
    TAtomic<u32> block{0u};

    /** 1なら停止中loaderを再開する。 */
    TAtomic<u32> release{0u};

private:
    /** 呼出側にも別aliasが残る返却対象。 */
    TSharedPtr<AAsset> m_Asset;
};

/** 登録結果だけを検証する任意拡張子loader。 */
class CExtensionOnlyLoader final : public IAssetLoader {
public:
    /** 担当拡張子を保持して構築する。 */
    explicit CExtensionOnlyLoader(const char* Extension) noexcept : m_Extension(Extension)
    {
    }

    /** binary asset型を返す。 */
    AssetType TypeId() const noexcept override
    {
        return ABinaryAsset::StaticType();
    }

    /** 構築時に指定した拡張子を返す。 */
    const char* Extension() const noexcept override
    {
        return m_Extension;
    }

    /** このtestではloadを行わないため固定errorを返す。 */
    TResult<TSharedPtr<AAsset>> LoadFromBytes(FAssetId, const TArray<byte>&) noexcept override
    {
        return ACS_ERR(Asset, 221u, "CExtensionOnlyLoader is registration-only");
    }

private:
    /** 登録対象の拡張子。 */
    const char* m_Extension = nullptr;
};

/** 別threadで行うsync loadの入力と結果。 */
struct FSyncLoadContext {
    /** load対象registry。 */
    CAssetRegistry* registry = nullptr;

    /** load対象path。 */
    const wchar_t* path = nullptr;

    /** load成功時のcanonical asset。 */
    TSharedPtr<AAsset> result;

    /** load失敗時に1。 */
    TAtomic<u32> failed{0u};
};

/** sync loadを別threadで実行して結果をcontextへ保存する。 */
void LoadRegistrySync(void* User) noexcept
{
    auto* const Context = static_cast<FSyncLoadContext*>(User);
    auto Result = Context->registry->Load(Context->path);
    if (Result.IsErr()) {
        Context->failed.Store(1u, EMemoryOrder::Release);
        return;
    }
    Context->result = Move(Result.Value());
}

struct FRegistryDeleteContext {
    CAssetRegistry* registry = nullptr;
    TAtomic<u32> finished{0};
};

void DeleteRegistry(void* user) noexcept
{
    auto* context = static_cast<FRegistryDeleteContext*>(user);
    delete context->registry;
    context->finished.Store(1, EMemoryOrder::Release);
}

} // namespace

ACS_TEST(FAssetRegistry, DestructorWaitsForAsyncLoad)
{
    CThreadPool::Shutdown();
    EXPECT_TRUE(CThreadPool::Init(1).IsOk());

    const byte file_data[] = {1, 2, 3, 4};
    EXPECT_TRUE(CFileSystem::WriteAllBytes(kAsyncAssetPath, file_data, sizeof(file_data)).IsOk());

    CBlockingAssetLoader loader;
    auto* registry = new CAssetRegistry();
    EXPECT_TRUE(registry != nullptr);
    if (!registry) {
        CThreadPool::Shutdown();
        (void)CFileSystem::Delete(kAsyncAssetPath);
        return;
    }
    registry->RegisterLoader(&loader);
    FAssetFuture future = registry->LoadAsync(kAsyncAssetPath);
    EXPECT_TRUE(future.Valid());

    for (u32 i = 0; i < 2000 && loader.entered.Load(EMemoryOrder::Acquire) == 0; ++i)
        SleepMs(1);
    EXPECT_EQ(loader.entered.Load(EMemoryOrder::Acquire), 1u);

    FRegistryDeleteContext delete_context{registry};
    auto destroyer = FThread::Spawn(&DeleteRegistry, &delete_context);
    EXPECT_TRUE(destroyer.IsOk());
    SleepMs(20);
    EXPECT_EQ(delete_context.finished.Load(EMemoryOrder::Acquire), 0u);

    loader.release.Store(1, EMemoryOrder::Release);
    if (destroyer.IsOk())
        destroyer.Value().Join();
    else
        DeleteRegistry(&delete_context);

    EXPECT_EQ(delete_context.finished.Load(EMemoryOrder::Acquire), 1u);
    EXPECT_TRUE(future.Get().IsOk());

    CThreadPool::Shutdown();
    (void)CFileSystem::Delete(kAsyncAssetPath);
}

ACS_TEST(FAssetRegistry, ConcurrentSamePathAsyncLoadsShareOneJobAndIdentity)
{
    CThreadPool::Shutdown();
    EXPECT_TRUE(CThreadPool::Init(2).IsOk());

    const byte file_data[] = {9, 8, 7, 6};
    EXPECT_TRUE(CFileSystem::WriteAllBytes(kAsyncAssetPath, file_data, sizeof(file_data)).IsOk());

    CBlockingAssetLoader loader;
    CAssetRegistry registry;
    registry.RegisterLoader(&loader);

    FAssetFuture first = registry.LoadAsync(kAsyncAssetPath);
    for (u32 i = 0; i < 2000 && loader.entered.Load(EMemoryOrder::Acquire) == 0; ++i) {
        SleepMs(1);
    }
    FAssetFuture second = registry.LoadAsync(kAsyncAssetPath);
    EXPECT_TRUE(first.Valid());
    EXPECT_TRUE(second.Valid());
    EXPECT_EQ(loader.entered.Load(EMemoryOrder::Acquire), 1u);

    loader.release.Store(1, EMemoryOrder::Release);
    const auto first_result = first.Get();
    const auto second_result = second.Get();
    EXPECT_TRUE(first_result.IsOk());
    EXPECT_TRUE(second_result.IsOk());
    if (first_result.IsOk() && second_result.IsOk()) {
        EXPECT_TRUE(first_result.Value().Get() == second_result.Value().Get());
    }
    EXPECT_EQ(loader.entered.Load(EMemoryOrder::Acquire), 1u);

    registry.Shutdown();
    CThreadPool::Shutdown();
    (void)CFileSystem::Delete(kAsyncAssetPath);
}

ACS_TEST(FAssetRegistry, ShutdownClosesLoadGate)
{
    CBlockingAssetLoader loader;
    CAssetRegistry registry;
    registry.RegisterLoader(&loader);
    registry.Shutdown();

    EXPECT_TRUE(registry.Load(kAsyncAssetPath).IsErr());
    FAssetFuture future = registry.LoadAsync(kAsyncAssetPath);
    EXPECT_TRUE(future.Valid());
    EXPECT_TRUE(future.IsReady());
    EXPECT_TRUE(future.Get().IsErr());
}

ACS_TEST(FAssetRegistry, RestartPreservesInjectedAllocatorAndReopensGate)
{
    CSystemAllocator allocator;
    CBlockingAssetLoader loader;
    CAssetRegistry registry(allocator);

    registry.RegisterLoader(&loader);
    EXPECT_TRUE(allocator.BytesAllocated() > 0u);

    registry.Shutdown();
    EXPECT_EQ(allocator.BytesAllocated(), 0ull);
    EXPECT_TRUE(registry.Load(kAsyncAssetPath).IsErr());

    registry.Restart();
    registry.RegisterLoader(&loader);
    EXPECT_TRUE(allocator.BytesAllocated() > 0u);

    registry.Shutdown();
    EXPECT_EQ(allocator.BytesAllocated(), 0ull);
}

ACS_TEST(CAssetRegistry, RejectsInternerLengthOverflowBeforeHashOrAllocation)
{
    CSystemAllocator Allocator;
    CAssetPathInterner Interner(Allocator);
    /** overflow入力で参照されてはならない最小dummy文字。 */
    const wchar_t Dummy = L'x';
    /** Length加算とbyte数乗算の両方をoverflowさせる入力長。 */
    constexpr usize OverflowLength = TNumLimits<usize>::Max();

    const auto InternResult = Interner.Intern(&Dummy, OverflowLength);
    EXPECT_TRUE(InternResult.IsErr());
    if (InternResult.IsErr()) {
        EXPECT_EQ(InternResult.Error().subcode, kAssetPathInternerSubInvalidPath);
    }

    FInternedAssetPath OwnedPath(Allocator, FAssetId{1u});
    EXPECT_FALSE(OwnedPath.TryInitialize(&Dummy, OverflowLength));
    EXPECT_EQ(Allocator.BytesAllocated(), 0ull);
}

ACS_TEST(CAssetRegistry, CachePublicationFailurePreservesRetainedAssetState)
{
    CThreadPool::Shutdown();
    const auto PoolInit = CThreadPool::Init(1u);
    EXPECT_TRUE(PoolInit.IsOk());
    if (PoolInit.IsErr()) return;

    const byte FileData[] = {4u, 3u, 2u, 1u};
    (void)CFileSystem::Delete(kTransactionalAssetPath);
    EXPECT_TRUE(CFileSystem::WriteAllBytes(kTransactionalAssetPath, FileData, sizeof(FileData)).IsOk());

    auto Binary = MakeShared<ABinaryAsset>();
    EXPECT_TRUE(Binary.Get() != nullptr);
    if (!Binary.Get()) {
        (void)CFileSystem::Delete(kTransactionalAssetPath);
        CThreadPool::Shutdown();
        return;
    }
    TSharedPtr<AAsset> Retained(Move(Binary));
    /** 失敗後も維持されるloader所有側の既存ID。 */
    const FAssetId OriginalId{0xA55E7u};
    Retained->SetId(OriginalId);
    Retained->SetState(EAssetState::Loading);

    CSwitchableFailAllocator Allocator;
    CRetainedAssetLoader Loader(Retained);
    CAssetRegistry Registry(Allocator);
    EXPECT_TRUE(Registry.TryRegisterLoader(&Loader).IsOk());

    Allocator.SetFail(true);
    const auto SyncResult = Registry.Load(kTransactionalAssetPath);
    EXPECT_TRUE(SyncResult.IsErr());
    if (SyncResult.IsErr()) {
        EXPECT_EQ(SyncResult.Error().subcode, kAssetRegistrySubOutOfMemory);
    }
    EXPECT_EQ(Retained->Id(), OriginalId);
    EXPECT_EQ(Retained->State(), EAssetState::Loading);

    Allocator.SetFail(false);
    Loader.block.Store(1u, EMemoryOrder::Release);
    Loader.release.Store(0u, EMemoryOrder::Release);
    FAssetFuture Future = Registry.LoadAsync(kTransactionalAssetPath);
    EXPECT_TRUE(Future.Valid());
    for (u32 Index = 0u; Index < 2000u && Loader.entered.Load(EMemoryOrder::Acquire) < 2u; ++Index) {
        SleepMs(1u);
    }
    EXPECT_EQ(Loader.entered.Load(EMemoryOrder::Acquire), 2u);

    Allocator.SetFail(true);
    Loader.release.Store(1u, EMemoryOrder::Release);
    const auto AsyncResult = Future.Get();
    EXPECT_TRUE(AsyncResult.IsErr());
    if (AsyncResult.IsErr()) {
        EXPECT_EQ(AsyncResult.Error().subcode, kAssetRegistrySubOutOfMemory);
    }
    EXPECT_EQ(Retained->Id(), OriginalId);
    EXPECT_EQ(Retained->State(), EAssetState::Loading);

    Allocator.SetFail(false);
    Registry.Shutdown();
    CThreadPool::Shutdown();
    (void)CFileSystem::Delete(kTransactionalAssetPath);
}

ACS_TEST(CAssetRegistry, MixedSyncAndAsyncLoadsPublishOneCanonicalIdentity)
{
    CThreadPool::Shutdown();
    const auto PoolInit = CThreadPool::Init(1u);
    EXPECT_TRUE(PoolInit.IsOk());
    if (PoolInit.IsErr()) return;

    const byte FileData[] = {7u, 5u, 3u, 1u};
    (void)CFileSystem::Delete(kTransactionalAssetPath);
    EXPECT_TRUE(CFileSystem::WriteAllBytes(kTransactionalAssetPath, FileData, sizeof(FileData)).IsOk());

    CBlockingAssetLoader Loader;
    CAssetRegistry Registry;
    EXPECT_TRUE(Registry.TryRegisterLoader(&Loader).IsOk());
    FAssetFuture AsyncFuture = Registry.LoadAsync(kTransactionalAssetPath);
    for (u32 Index = 0u; Index < 2000u && Loader.entered.Load(EMemoryOrder::Acquire) < 1u; ++Index) {
        SleepMs(1u);
    }

    FSyncLoadContext SyncContext{&Registry, kTransactionalAssetPath};
    auto SyncThread = FThread::Spawn(&LoadRegistrySync, &SyncContext);
    EXPECT_TRUE(SyncThread.IsOk());
    if (SyncThread.IsErr()) {
        Loader.release.Store(1u, EMemoryOrder::Release);
        (void)AsyncFuture.Get();
        Registry.Shutdown();
        CThreadPool::Shutdown();
        (void)CFileSystem::Delete(kTransactionalAssetPath);
        return;
    }
    for (u32 Index = 0u; Index < 2000u && Loader.entered.Load(EMemoryOrder::Acquire) < 2u; ++Index) {
        SleepMs(1u);
    }
    EXPECT_EQ(Loader.entered.Load(EMemoryOrder::Acquire), 2u);

    Loader.release.Store(1u, EMemoryOrder::Release);
    SyncThread.Value().Join();
    const auto AsyncResult = AsyncFuture.Get();
    EXPECT_TRUE(AsyncResult.IsOk());
    EXPECT_EQ(SyncContext.failed.Load(EMemoryOrder::Acquire), 0u);
    if (AsyncResult.IsOk() && SyncContext.result.Get()) {
        EXPECT_TRUE(AsyncResult.Value().Get() == SyncContext.result.Get());
        EXPECT_EQ(AsyncResult.Value()->State(), EAssetState::Ready);
        EXPECT_EQ(AsyncResult.Value()->Id(), SyncContext.result->Id());
    }

    Registry.Shutdown();
    CThreadPool::Shutdown();
    (void)CFileSystem::Delete(kTransactionalAssetPath);
}

ACS_TEST(CAssetRegistry, PendingAsyncLoadJoinsWhileDefaultAllocatorRejectsAllocation)
{
    CThreadPool::Shutdown();
    const auto PoolInit = CThreadPool::Init(1u);
    EXPECT_TRUE(PoolInit.IsOk());
    if (PoolInit.IsErr()) return;

    const byte FileData[] = {2u, 4u, 6u, 8u};
    (void)CFileSystem::Delete(kTransactionalAssetPath);
    EXPECT_TRUE(CFileSystem::WriteAllBytes(kTransactionalAssetPath, FileData, sizeof(FileData)).IsOk());

    CBlockingAssetLoader Loader;
    CAssetRegistry Registry;
    EXPECT_TRUE(Registry.TryRegisterLoader(&Loader).IsOk());
    FAssetFuture First = Registry.LoadAsync(kTransactionalAssetPath);
    for (u32 Index = 0u; Index < 2000u && Loader.entered.Load(EMemoryOrder::Acquire) == 0u; ++Index) {
        SleepMs(1u);
    }
    EXPECT_EQ(Loader.entered.Load(EMemoryOrder::Acquire), 1u);

    CSwitchableFailAllocator RejectingAllocator;
    RejectingAllocator.SetFail(true);
    FAssetFuture Second;
    {
        CDefaultAllocatorScope AllocatorScope(RejectingAllocator);
        Second = Registry.LoadAsync(kTransactionalAssetPath);
    }
    EXPECT_TRUE(First.Valid());
    EXPECT_TRUE(Second.Valid());

    Loader.release.Store(1u, EMemoryOrder::Release);
    const auto FirstResult = First.Get();
    const auto SecondResult = Second.Get();
    EXPECT_TRUE(FirstResult.IsOk());
    EXPECT_TRUE(SecondResult.IsOk());
    if (FirstResult.IsOk() && SecondResult.IsOk()) {
        EXPECT_TRUE(FirstResult.Value().Get() == SecondResult.Value().Get());
    }
    const FAssetRegistryDiagnostics Diagnostics = Registry.Diagnostics();
    EXPECT_EQ(Diagnostics.async_request_count, 2ull);
    EXPECT_EQ(Diagnostics.async_coalesced_count, 1ull);
    EXPECT_EQ(Diagnostics.async_job_count, 1ull);

    Registry.Shutdown();
    CThreadPool::Shutdown();
    (void)CFileSystem::Delete(kTransactionalAssetPath);
}

ACS_TEST(CAssetRegistry, DefaultLoaderBatchIsTransactionalAndIdempotent)
{
    CExtensionOnlyLoader ConflictingWav("wav");
    CExtensionOnlyLoader CustomPng("png");
    CExtensionOnlyLoader CustomFallback("*");

    {
        CAssetRegistry Registry;
        EXPECT_TRUE(Registry.TryRegisterLoader(&ConflictingWav).IsOk());
        const auto DuplicateResult = Registry.TryRegisterDefaultLoaders();
        EXPECT_TRUE(DuplicateResult.IsErr());
        if (DuplicateResult.IsErr()) {
            EXPECT_EQ(DuplicateResult.Error().subcode, kAssetRegistrySubDuplicateLoader);
        }
        EXPECT_TRUE(Registry.TryRegisterLoader(&CustomPng).IsOk());
        EXPECT_TRUE(Registry.TryRegisterLoader(&CustomFallback).IsOk());
    }

    {
        CSwitchableFailAllocator Allocator;
        CAssetRegistry Registry(Allocator);
        Allocator.SetFail(true);
        const auto OomResult = Registry.TryRegisterDefaultLoaders();
        EXPECT_TRUE(OomResult.IsErr());
        if (OomResult.IsErr()) {
            EXPECT_EQ(OomResult.Error().subcode, kAssetRegistrySubOutOfMemory);
        }
        Allocator.SetFail(false);
        EXPECT_TRUE(Registry.TryRegisterLoader(&CustomPng).IsOk());
        EXPECT_TRUE(Registry.TryRegisterLoader(&CustomFallback).IsOk());
    }

    {
        CAssetRegistry Registry;
        EXPECT_TRUE(Registry.TryRegisterDefaultLoaders().IsOk());
        EXPECT_TRUE(Registry.TryRegisterDefaultLoaders().IsOk());
    }
}
