// SPDX-License-Identifier: Apache-2.0
// Foundation Optimization Wave K の ownership / I/O / batching 契約テスト
#include "test/Expect.h"
#include "test/Test.h"

#include "asset/AssetPathInterner.h"
#include "asset/AssetRegistry.h"
#include "asset/BinaryAsset.h"
#include "assetpack/AcpakGameBridge.h"
#include "assetpack/AcpakReadDiagnostics.h"
#include "assetpack/AcpakReader.h"
#include "assetpack/AcpakWriter.h"
#include "container/Array.h"
#include "foundation/Error.h"
#include "foundation/Move.h"
#include "gameframework/AssetPack.h"
#include "gameframework/Scene3D.h"
#include "gameframework/Scene3DSerialize.h"
#include "platform/FileSystem.h"
#include "threading/Atomic.h"
#include "threading/Thread.h"
#include "threading/ThreadPool.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>

using namespace acs;
using namespace acs::game;

namespace {

bool EqualBytes(const u8* Left, const u8* Right, usize Size) noexcept
{
    for (usize Index = 0u; Index < Size; ++Index) {
        if (Left[Index] != Right[Index]) return false;
    }
    return true;
}

bool AllBytesEqual(const u8* Bytes, usize Size, u8 Expected) noexcept
{
    for (usize Index = 0u; Index < Size; ++Index) {
        if (Bytes[Index] != Expected) return false;
    }
    return true;
}

void FillPayload(TArray<u8>& Bytes, usize Size, u32 Seed) noexcept
{
    if (!Bytes.TryResize(Size)) return;
    u32 State = Seed;
    for (usize Index = 0u; Index < Size; ++Index) {
        State ^= State << 13u;
        State ^= State >> 17u;
        State ^= State << 5u;
        Bytes[Index] = static_cast<u8>(State >> 11u);
    }
}

bool WriteTwoRawFiles(const wchar_t* Path, const TArray<u8>& First, const TArray<u8>& Second) noexcept
{
    assetpack::FAcpakWriter Writer;
    if (Writer.Open(Path, assetpack::AcpakFlagNone).IsErr()) return false;
    if (Writer.AddFile(L"stream/first.bin", First.Data(), First.Size()).IsErr()) {
        return false;
    }
    if (Writer.AddFile(L"stream/second.bin", Second.Data(), Second.Size()).IsErr()) {
        return false;
    }
    const bool Succeeded = Writer.Finalize().IsOk();
    Writer.Close();
    return Succeeded;
}

class FBlockingWaveKLoader final : public IAssetLoader {
public:
    AssetType TypeId() const noexcept override
    {
        return FBinaryAsset::StaticType();
    }

    const char* Extension() const noexcept override
    {
        return "wvk";
    }

    TResult<TSharedPtr<FAsset>> LoadFromBytes(FAssetId, const TArray<byte>&) noexcept override
    {
        Entered.FetchAdd(1u);
        while (Release.Load(EMemoryOrder::Acquire) == 0u) Yield();
        auto Binary = MakeShared<FBinaryAsset>();
        if (!Binary.Get()) {
            return ACS_ERR(Memory, 1950u, "FBlockingWaveKLoader: allocation failed");
        }
        TSharedPtr<FAsset> Asset(Move(Binary));
        return TResult<TSharedPtr<FAsset>>(OkInit, Move(Asset));
    }

    TAtomic<u32> Entered{0u};
    TAtomic<u32> Release{0u};
};

struct FRegistryShutdownContext {
    FAssetRegistry* Registry = nullptr;
    TAtomic<u32> Finished{0u};
};

void ShutdownRegistry(void* User) noexcept
{
    auto& Context = *static_cast<FRegistryShutdownContext*>(User);
    Context.Registry->Shutdown();
    Context.Finished.Store(1u, EMemoryOrder::Release);
}

struct FCompressedReadContext {
    assetpack::FAcpakReader* Reader = nullptr;
    const TArray<u8>* Expected = nullptr;
    TArray<u8>* Output = nullptr;
    TAtomic<u32>* Ready = nullptr;
    TAtomic<u32>* Start = nullptr;
    TAtomic<u32>* Failures = nullptr;
};

void ReadCompressedAsset(void* User) noexcept
{
    auto& Context = *static_cast<FCompressedReadContext*>(User);
    Context.Ready->FetchAdd(1u);
    while (Context.Start->Load(EMemoryOrder::Acquire) == 0u) Yield();
    const auto Result = Context.Reader->ReadFile(L"compressed/payload.bin", Context.Output->Data(), Context.Output->Size());
    if (Result.IsErr() || !EqualBytes(Context.Output->Data(), Context.Expected->Data(), Context.Expected->Size())) {
        Context.Failures->FetchAdd(1u);
    }
}

class FLegacyAssetPackReader final : public IAssetPackReader {
public:
    TResult<void> Mount(const char*) noexcept override
    {
        return Ok();
    }

    void Unmount() noexcept override
    {
    }
    bool IsMounted() const noexcept override
    {
        return true;
    }

    TResult<u32> FileCount() noexcept override
    {
        return TResult<u32>(OkInit, 2u);
    }

    TResult<const char*> FileName(u32) noexcept override
    {
        return TResult<const char*>(OkInit, "legacy.bin");
    }

    TResult<u64> FileSize(const char*) noexcept override
    {
        return TResult<u64>(OkInit, 1u);
    }

    TResult<void> ReadFile(const char* Name, u8* OutBuffer, u64 BufferSize) noexcept override
    {
        ++ReadCalls;
        if (std::strcmp(Name, "missing.bin") == 0) {
            return ACS_ERR(IO, 1951u, "FLegacyAssetPackReader: missing");
        }
        if (OutBuffer == nullptr || BufferSize < 1u) {
            return ACS_ERR(IO, 1952u, "FLegacyAssetPackReader: bad buffer");
        }
        OutBuffer[0] = static_cast<u8>(ReadCalls);
        return Ok();
    }

    u32 ReadCalls = 0u;
};

enum class EScenePackMode : u8 {
    SharedDependency,
    EarlierDecodeFailure,
};

class FScenePackReader final : public IAssetPackReader {
public:
    explicit FScenePackReader(EScenePackMode Mode) noexcept : m_Mode(Mode)
    {
    }

    TResult<void> Mount(const char*) noexcept override
    {
        return Ok();
    }
    void Unmount() noexcept override
    {
    }
    bool IsMounted() const noexcept override
    {
        return true;
    }

    TResult<u32> FileCount() noexcept override
    {
        return TResult<u32>(OkInit, 4u);
    }

    TResult<const char*> FileName(u32) noexcept override
    {
        return TResult<const char*>(OkInit, "main.acscene");
    }

    TResult<u64> FileSize(const char* Name) noexcept override
    {
        const char* Text = TextFor(Name);
        if (Text == nullptr) {
            return ACS_ERR(IO, 1953u, "FScenePackReader: missing size");
        }
        return TResult<u64>(OkInit, static_cast<u64>(std::strlen(Text)));
    }

    TResult<void> ReadFile(const char* Name, u8* OutBuffer, u64 BufferSize) noexcept override
    {
        if (std::strcmp(Name, "materials/missing.acsmat") == 0) {
            return ACS_ERR(IO, 1954u, "FScenePackReader: missing data");
        }
        const char* Text = TextFor(Name);
        if (Text == nullptr) {
            return ACS_ERR(IO, 1955u, "FScenePackReader: unknown path");
        }
        const usize Size = std::strlen(Text);
        if (OutBuffer == nullptr || BufferSize < Size) {
            return ACS_ERR(IO, 1956u, "FScenePackReader: bad buffer");
        }
        if (std::strcmp(Name, "main.acscene") != 0) ++DependencyReadCalls;
        MemCopy(OutBuffer, Text, Size);
        return Ok();
    }

    TResult<void> ReadFiles(const FAssetPackReadRequest* Requests, u32 Count, u32* CompletedCount) noexcept override
    {
        ++BatchCalls;
        LastBatchCount = Count;
        if (Count >= 2u) {
            StableFirstSeen = std::strcmp(Requests[0].Name, "materials/bad.acsmat") == 0 && std::strcmp(Requests[1].Name, "materials/missing.acsmat") == 0;
        }
        return IAssetPackReader::ReadFiles(Requests, Count, CompletedCount);
    }

    u32 BatchCalls = 0u;
    u32 LastBatchCount = 0u;
    u32 DependencyReadCalls = 0u;
    bool StableFirstSeen = false;

private:
    const char* TextFor(const char* Name) const noexcept
    {
        if (std::strcmp(Name, "main.acscene") == 0) {
            return m_Mode == EScenePackMode::SharedDependency ? kSharedScene : kFailureScene;
        }
        if (std::strcmp(Name, "materials/shared.acsmat") == 0) {
            return kValidMaterial;
        }
        if (std::strcmp(Name, "materials/bad.acsmat") == 0) {
            return kInvalidMaterial;
        }
        if (std::strcmp(Name, "materials/missing.acsmat") == 0) {
            // FileSize は成功させ、batch の二番目の ReadFile で失敗させる。
            return kMissingPlaceholder;
        }
        return nullptr;
    }

    static constexpr char kSharedScene[] = "ACS3D v2\n"
                                           "N3D 1 -1 0 0 0 0 0 0 0 1 1 1 1 1 1 1 First\n"
                                           "MAT3D 1 materials/shared.acsmat\n"
                                           "N3D 2 -1 0 0 0 0 0 0 0 1 1 1 1 1 1 1 Second\n"
                                           "MAT3D 2 materials/shared.acsmat\n";

    static constexpr char kFailureScene[] = "ACS3D v2\n"
                                            "N3D 1 -1 0 0 0 0 0 0 0 1 1 1 1 1 1 1 First\n"
                                            "MAT3D 1 materials/bad.acsmat\n"
                                            "N3D 2 -1 0 0 0 0 0 0 0 1 1 1 1 1 1 1 Second\n"
                                            "MAT3D 2 materials/missing.acsmat\n";

    static constexpr char kValidMaterial[] = "ACSMAT 1\nkind pbr\nmetallic 0.25\nroughness 0.75\n";
    static constexpr char kInvalidMaterial[] = "invalid\n";
    static constexpr char kMissingPlaceholder[] = "none";

    EScenePackMode m_Mode;
};

} // namespace

ACS_TEST(FoundationOptimizationWaveK, InternerIsBoundedAndPinnedPathsSurviveReset)
{
    constexpr const wchar_t* kFirstPath = L"Assets/Materials/SharedCloud.acsmat";
    constexpr usize kFirstLength = 35u;
    static_assert(kFirstPath[kFirstLength] == L'\0');

    FAssetPathInterner Interner;
    auto FirstResult = Interner.Intern(kFirstPath, kFirstLength);
    auto SecondResult = Interner.Intern(kFirstPath, kFirstLength);
    EXPECT_TRUE(FirstResult.IsOk());
    EXPECT_TRUE(SecondResult.IsOk());
    if (FirstResult.IsErr() || SecondResult.IsErr()) return;

    TSharedPtr<FInternedAssetPath> First = FirstResult.Value();
    TSharedPtr<FInternedAssetPath> Second = SecondResult.Value();
    EXPECT_TRUE(First.Get() == Second.Get());

    TArray<TSharedPtr<FInternedAssetPath>> Pins;
    EXPECT_TRUE(Pins.TryReserve(kAssetPathInternerMaxEntries));
    EXPECT_TRUE(Pins.TryPushBack(First));
    for (usize Index = 1u; Index < kAssetPathInternerMaxEntries; ++Index) {
        wchar_t Path[64]{};
        const int Length = std::swprintf(Path, sizeof(Path) / sizeof(Path[0]), L"Assets/Interned/%03zu.asset", Index);
        EXPECT_TRUE(Length > 0);
        if (Length <= 0) return;
        auto Result = Interner.Intern(Path, static_cast<usize>(Length));
        EXPECT_TRUE(Result.IsOk());
        if (Result.IsErr()) return;
        EXPECT_TRUE(Pins.TryPushBack(Result.Value()));
    }

    auto OverflowResult = Interner.Intern(L"Assets/Interned/overflow.asset", 30u);
    EXPECT_TRUE(OverflowResult.IsOk());
    if (OverflowResult.IsErr()) return;
    TSharedPtr<FInternedAssetPath> Overflow = OverflowResult.Value();

    const FAssetPathInternerDiagnostics Diagnostics = Interner.Diagnostics();
    EXPECT_EQ(Diagnostics.retained_path_count, static_cast<u64>(kAssetPathInternerMaxEntries));
    EXPECT_TRUE(Diagnostics.retained_code_units <= static_cast<u64>(kAssetPathInternerMaxCodeUnits));
    EXPECT_EQ(Diagnostics.hit_count, 1ull);
    EXPECT_EQ(Diagnostics.bypass_count, 1ull);

    Interner.Reset();
    EXPECT_TRUE(std::wcscmp(First->Path(), kFirstPath) == 0);
    EXPECT_TRUE(std::wcscmp(Overflow->Path(), L"Assets/Interned/overflow.asset") == 0);
}

ACS_TEST(FoundationOptimizationWaveK, AsyncPathOwnershipSkipsHotHitsAndSurvivesShutdown)
{
    constexpr const wchar_t* kPath = L"acs_wave_k_registry.wvk";
    constexpr u8 kPayload[] = {9u, 7u, 5u, 3u};
    (void)FFileSystem::Delete(kPath);
    EXPECT_TRUE(FFileSystem::WriteAllBytes(kPath, kPayload, sizeof(kPayload)).IsOk());

    FThreadPool::Shutdown();
    EXPECT_TRUE(FThreadPool::Init(2u).IsOk());

    FBlockingWaveKLoader Loader;
    FAssetRegistry Registry;
    Registry.RegisterLoader(&Loader);

    FAssetFuture First = Registry.LoadAsync(kPath);
    for (u32 Wait = 0u; Wait < 2000u && Loader.Entered.Load(EMemoryOrder::Acquire) < 1u; ++Wait) {
        SleepMs(1u);
    }
    FAssetFuture Coalesced = Registry.LoadAsync(kPath);
    const FAssetRegistryDiagnostics Pending = Registry.Diagnostics();
    EXPECT_EQ(Pending.async_request_count, 2ull);
    EXPECT_EQ(Pending.async_coalesced_count, 1ull);
    EXPECT_EQ(Pending.async_job_count, 1ull);
    EXPECT_EQ(Pending.physical_file_read_count, 1ull);
    EXPECT_EQ(Pending.path_interner.request_count, 1ull);
    EXPECT_EQ(Pending.path_interner.miss_count, 1ull);

    Loader.Release.Store(1u, EMemoryOrder::Release);
    const auto FirstResult = First.Get();
    const auto CoalescedResult = Coalesced.Get();
    EXPECT_TRUE(FirstResult.IsOk());
    EXPECT_TRUE(CoalescedResult.IsOk());
    if (FirstResult.IsErr() || CoalescedResult.IsErr()) return;
    EXPECT_TRUE(FirstResult.Value().Get() == CoalescedResult.Value().Get());

    const FAssetId Id = FirstResult.Value()->Id();
    Registry.Unload(Id);
    FAssetFuture Repeated = Registry.LoadAsync(kPath);
    EXPECT_TRUE(Repeated.Get().IsOk());

    const FAssetRegistryDiagnostics RepeatedDiagnostics = Registry.Diagnostics();
    EXPECT_EQ(RepeatedDiagnostics.async_job_count, 2ull);
    EXPECT_EQ(RepeatedDiagnostics.path_interner.request_count, 2ull);
    EXPECT_EQ(RepeatedDiagnostics.path_interner.hit_count, 1ull);
    EXPECT_TRUE(RepeatedDiagnostics.path_interner.retained_code_units < static_cast<u64>(kAssetRegistryMaxPathLength + 1u));

    // sync cache hit には interner lock / owned copy を追加しない。
    EXPECT_TRUE(Registry.Load(kPath).IsOk());
    EXPECT_EQ(Registry.Diagnostics().path_interner.request_count, 2ull);

    Registry.Unload(Id);
    Loader.Release.Store(0u, EMemoryOrder::Release);
    FAssetFuture DuringShutdown = Registry.LoadAsync(kPath);
    for (u32 Wait = 0u; Wait < 2000u && Loader.Entered.Load(EMemoryOrder::Acquire) < 3u; ++Wait) {
        SleepMs(1u);
    }
    EXPECT_EQ(Loader.Entered.Load(EMemoryOrder::Acquire), 3u);

    FRegistryShutdownContext ShutdownContext;
    ShutdownContext.Registry = &Registry;
    auto ShutdownThread = FThread::Spawn(&ShutdownRegistry, &ShutdownContext);
    EXPECT_TRUE(ShutdownThread.IsOk());
    SleepMs(10u);
    EXPECT_EQ(ShutdownContext.Finished.Load(EMemoryOrder::Acquire), 0u);

    Loader.Release.Store(1u, EMemoryOrder::Release);
    EXPECT_TRUE(DuringShutdown.Get().IsOk());
    if (ShutdownThread.IsOk()) ShutdownThread.Value().Join();
    EXPECT_EQ(ShutdownContext.Finished.Load(EMemoryOrder::Acquire), 1u);

    FThreadPool::Shutdown();
    (void)FFileSystem::Delete(kPath);
}

ACS_TEST(FoundationOptimizationWaveK, LegacyBatchFallbackReportsPartialCommit)
{
    FLegacyAssetPackReader Reader;
    u8 First = 0xCCu;
    u8 Missing = 0xCCu;
    u8 Last = 0xCCu;
    FAssetPackReadRequest Requests[] = {{"first.bin", &First, 1u}, {"missing.bin", &Missing, 1u}, {"last.bin", &Last, 1u}};
    u32 Completed = 99u;
    const auto Result = Reader.ReadFiles(Requests, 3u, &Completed);
    EXPECT_TRUE(Result.IsErr());
    EXPECT_EQ(Completed, 1u);
    EXPECT_EQ(Reader.ReadCalls, 2u);
    EXPECT_EQ(First, 1u);
    EXPECT_EQ(Missing, 0xCCu);
    EXPECT_EQ(Last, 0xCCu);
}

ACS_TEST(FoundationOptimizationWaveK, MappedBatchKeepsAtomicSnapshotAndCrcCommit)
{
    constexpr const wchar_t* kPath = L"acs_wave_k_mapped.acpak";
    (void)FFileSystem::Delete(kPath);

    TArray<u8> OldFirst;
    TArray<u8> OldSecond;
    TArray<u8> NewFirst;
    TArray<u8> NewSecond;
    FillPayload(OldFirst, 192u * 1024u, 0x12345678u);
    FillPayload(OldSecond, 128u * 1024u, 0x87654321u);
    FillPayload(NewFirst, OldFirst.Size(), 0xCAFEBABEu);
    FillPayload(NewSecond, OldSecond.Size(), 0x10203040u);
    EXPECT_EQ(OldFirst.Size(), 192u * 1024u);
    EXPECT_EQ(OldSecond.Size(), 128u * 1024u);
    EXPECT_TRUE(WriteTwoRawFiles(kPath, OldFirst, OldSecond));

    assetpack::FAcpakReader OldReader;
    EXPECT_TRUE(OldReader.Open(kPath).IsOk());
    EXPECT_TRUE(OldReader.ReadDiagnostics().mapped_view_active);
    EXPECT_TRUE(WriteTwoRawFiles(kPath, NewFirst, NewSecond));

    TArray<u8> FirstOut;
    TArray<u8> SecondOut;
    EXPECT_TRUE(FirstOut.TryResize(OldFirst.Size()));
    EXPECT_TRUE(SecondOut.TryResize(OldSecond.Size()));
    const wchar_t* Paths[] = {L"stream/first.bin", L"stream/second.bin"};
    void* Buffers[] = {FirstOut.Data(), SecondOut.Data()};
    const u64 Sizes[] = {static_cast<u64>(FirstOut.Size()), static_cast<u64>(SecondOut.Size())};
    u32 Completed = 99u;
    const auto BatchResult = OldReader.ReadFiles(Paths, Buffers, Sizes, 2u, &Completed);
    EXPECT_TRUE(BatchResult.IsOk());
    EXPECT_EQ(Completed, 2u);
    EXPECT_TRUE(EqualBytes(FirstOut.Data(), OldFirst.Data(), OldFirst.Size()));
    EXPECT_TRUE(EqualBytes(SecondOut.Data(), OldSecond.Data(), OldSecond.Size()));

    const assetpack::FAcpakReadDiagnostics BatchDiagnostics = OldReader.ReadDiagnostics();
    EXPECT_EQ(BatchDiagnostics.batch_count, 1ull);
    EXPECT_EQ(BatchDiagnostics.batch_entry_count, 2ull);
    EXPECT_EQ(BatchDiagnostics.mapped_read_count, 2ull);
    EXPECT_EQ(BatchDiagnostics.buffered_read_count, 0ull);

    assetpack::FAcpakReader NewReader;
    EXPECT_TRUE(NewReader.Open(kPath).IsOk());
    EXPECT_TRUE(NewReader.ReadFile(L"stream/first.bin", FirstOut.Data(), FirstOut.Size()).IsOk());
    EXPECT_TRUE(EqualBytes(FirstOut.Data(), NewFirst.Data(), NewFirst.Size()));

    // bridge も後続 UTF-8 エラーより先行 read を優先し、部分完了数を維持する。
    assetpack::FAcpakGameReader BridgeReader;
    EXPECT_TRUE(BridgeReader.Mount("acs_wave_k_mapped.acpak").IsOk());
    MemSet(FirstOut.Data(), 0xCCu, FirstOut.Size());
    char InvalidUtf8[] = {static_cast<char>(0xFFu), '\0'};
    u8 BridgeMissingOut = 0xCCu;
    FAssetPackReadRequest BridgeRequests[] = {{"stream/first.bin", FirstOut.Data(), FirstOut.Size()}, {InvalidUtf8, &BridgeMissingOut, 1u}};
    u32 BridgeCompleted = 99u;
    const auto BridgeResult = BridgeReader.ReadFiles(BridgeRequests, 2u, &BridgeCompleted);
    EXPECT_TRUE(BridgeResult.IsErr());
    EXPECT_EQ(BridgeCompleted, 1u);
    EXPECT_TRUE(EqualBytes(FirstOut.Data(), NewFirst.Data(), NewFirst.Size()));
    EXPECT_EQ(BridgeMissingOut, 0xCCu);
    BridgeReader.Unmount();

    // 後続失敗では先行出力だけ commit され、完了数が明示される。
    MemSet(FirstOut.Data(), 0xCCu, FirstOut.Size());
    MemSet(SecondOut.Data(), 0xCCu, SecondOut.Size());
    u8 MissingOut = 0xCCu;
    const wchar_t* PartialPaths[] = {L"stream/first.bin", L"missing.bin", L"stream/second.bin"};
    void* PartialBuffers[] = {FirstOut.Data(), &MissingOut, SecondOut.Data()};
    const u64 PartialSizes[] = {static_cast<u64>(FirstOut.Size()), 1u, static_cast<u64>(SecondOut.Size())};
    Completed = 99u;
    const auto PartialResult = NewReader.ReadFiles(PartialPaths, PartialBuffers, PartialSizes, 3u, &Completed);
    EXPECT_TRUE(PartialResult.IsErr());
    EXPECT_EQ(Completed, 1u);
    EXPECT_TRUE(EqualBytes(FirstOut.Data(), NewFirst.Data(), NewFirst.Size()));
    EXPECT_EQ(MissingOut, 0xCCu);
    EXPECT_TRUE(AllBytesEqual(SecondOut.Data(), SecondOut.Size(), 0xCCu));

    // Release 参考値: 同一 bytes/CRC workload で lock 境界だけを比較する。
    constexpr u32 kMeasureIterations = 24u;
    auto SequentialBegin = std::chrono::steady_clock::now();
    for (u32 Index = 0u; Index < kMeasureIterations; ++Index) {
        (void)NewReader.ReadFile(Paths[0], FirstOut.Data(), FirstOut.Size());
        (void)NewReader.ReadFile(Paths[1], SecondOut.Data(), SecondOut.Size());
    }
    auto SequentialEnd = std::chrono::steady_clock::now();
    auto BatchBegin = std::chrono::steady_clock::now();
    for (u32 Index = 0u; Index < kMeasureIterations; ++Index) {
        (void)NewReader.ReadFiles(Paths, Buffers, Sizes, 2u);
    }
    auto BatchEnd = std::chrono::steady_clock::now();
    const auto SequentialUs = std::chrono::duration_cast<std::chrono::microseconds>(SequentialEnd - SequentialBegin).count();
    const auto BatchUs = std::chrono::duration_cast<std::chrono::microseconds>(BatchEnd - BatchBegin).count();
    std::printf("[WaveK] mapped-read iterations=%u sequential_us=%lld batch_us=%lld bytes_per_iteration=%zu\n", kMeasureIterations, static_cast<long long>(SequentialUs), static_cast<long long>(BatchUs), OldFirst.Size() + OldSecond.Size());

    OldReader.Close();
    NewReader.Close();

    // raw mapped 経路でも CRC 成功前に caller buffer を変更しない。
    assetpack::FAcpakReader OffsetReader;
    EXPECT_TRUE(OffsetReader.Open(kPath).IsOk());
    const assetpack::FAcpakFileEntry* Entry = OffsetReader.FindEntry(L"stream/first.bin");
    EXPECT_TRUE(Entry != nullptr);
    const u64 DataOffset = Entry != nullptr ? Entry->offset : 0u;
    OffsetReader.Close();

    auto ArchiveResult = FFileSystem::ReadAllBytes(kPath);
    EXPECT_TRUE(ArchiveResult.IsOk());
    if (ArchiveResult.IsOk() && DataOffset < ArchiveResult.Value().Size()) {
        ArchiveResult.Value()[static_cast<usize>(DataOffset)] ^= 0x5Au;
        EXPECT_TRUE(FFileSystem::WriteAllBytes(kPath, ArchiveResult.Value().Data(), ArchiveResult.Value().Size()).IsOk());
    }

    assetpack::FAcpakReader CorruptReader;
    EXPECT_TRUE(CorruptReader.Open(kPath).IsOk());
    MemSet(FirstOut.Data(), 0xA5u, FirstOut.Size());
    const auto CorruptResult = CorruptReader.ReadFile(L"stream/first.bin", FirstOut.Data(), FirstOut.Size());
    EXPECT_TRUE(CorruptResult.IsErr());
    EXPECT_TRUE(AllBytesEqual(FirstOut.Data(), FirstOut.Size(), 0xA5u));
    CorruptReader.Close();
    (void)FFileSystem::Delete(kPath);
}

ACS_TEST(FoundationOptimizationWaveK, CompressedScratchIsBoundedAndConcurrentSafe)
{
    constexpr const wchar_t* kPath = L"acs_wave_k_compressed.acpak";
    constexpr u32 kWorkerCount = 4u;
    (void)FFileSystem::Delete(kPath);

    TArray<u8> Payload;
    FillPayload(Payload, 2u * 1024u * 1024u, 0x31415926u);
    EXPECT_EQ(Payload.Size(), 2u * 1024u * 1024u);

    assetpack::FAcpakWriter Writer;
    EXPECT_TRUE(Writer.Open(kPath, assetpack::AcpakFlagCompressed).IsOk());
    EXPECT_TRUE(Writer.AddFile(L"compressed/payload.bin", Payload.Data(), Payload.Size()).IsOk());
    EXPECT_TRUE(Writer.Finalize().IsOk());
    Writer.Close();

    assetpack::FAcpakReader Reader;
    EXPECT_TRUE(Reader.Open(kPath).IsOk());
    TArray<u8> WarmOutput;
    EXPECT_TRUE(WarmOutput.TryResize(Payload.Size()));
    EXPECT_TRUE(Reader.ReadFile(L"compressed/payload.bin", WarmOutput.Data(), WarmOutput.Size()).IsOk());
    EXPECT_TRUE(EqualBytes(WarmOutput.Data(), Payload.Data(), Payload.Size()));

    Reader.ResetReadDiagnostics();
    TArray<u8> Outputs[kWorkerCount];
    FCompressedReadContext Contexts[kWorkerCount]{};
    FThread Workers[kWorkerCount];
    TAtomic<u32> Ready{0u};
    TAtomic<u32> Start{0u};
    TAtomic<u32> Failures{0u};
    u32 StartedWorkers = 0u;
    for (u32 Index = 0u; Index < kWorkerCount; ++Index) {
        EXPECT_TRUE(Outputs[Index].TryResize(Payload.Size()));
        Contexts[Index].Reader = &Reader;
        Contexts[Index].Expected = &Payload;
        Contexts[Index].Output = &Outputs[Index];
        Contexts[Index].Ready = &Ready;
        Contexts[Index].Start = &Start;
        Contexts[Index].Failures = &Failures;
        auto SpawnResult = FThread::Spawn(&ReadCompressedAsset, &Contexts[Index]);
        if (SpawnResult.IsOk()) {
            Workers[StartedWorkers++] = Move(SpawnResult.Value());
        }
    }
    EXPECT_EQ(StartedWorkers, kWorkerCount);
    while (Ready.Load(EMemoryOrder::Acquire) < StartedWorkers) Yield();
    Start.Store(1u, EMemoryOrder::Release);
    for (u32 Index = 0u; Index < StartedWorkers; ++Index) {
        Workers[Index].Join();
    }

    EXPECT_EQ(Failures.Load(EMemoryOrder::Acquire), 0u);
    const assetpack::FAcpakReadDiagnostics Diagnostics = Reader.ReadDiagnostics();
    EXPECT_EQ(Diagnostics.mapped_read_count + Diagnostics.buffered_read_count, static_cast<u64>(kWorkerCount));
    EXPECT_EQ(Diagnostics.scratch_reuse_count + Diagnostics.scratch_fallback_count, static_cast<u64>(kWorkerCount));
    EXPECT_TRUE(Diagnostics.retained_scratch_bytes <= static_cast<u64>(assetpack::kAcpakRetainedScratchMaxBytes * 2u));
    std::printf("[WaveK] compressed-read workers=%u reuse=%llu fallback=%llu retained_bytes=%llu mapped=%d\n", kWorkerCount, static_cast<unsigned long long>(Diagnostics.scratch_reuse_count), static_cast<unsigned long long>(Diagnostics.scratch_fallback_count), static_cast<unsigned long long>(Diagnostics.retained_scratch_bytes), Diagnostics.mapped_view_active ? 1 : 0);

    Reader.ResetReadDiagnostics();
    const assetpack::FAcpakReadDiagnostics Reset = Reader.ReadDiagnostics();
    EXPECT_EQ(Reset.mapped_read_count, 0ull);
    EXPECT_EQ(Reset.buffered_read_count, 0ull);
    EXPECT_EQ(Reset.scratch_reuse_count, 0ull);
    EXPECT_EQ(Reset.scratch_fallback_count, 0ull);

    Reader.Close();
    (void)FFileSystem::Delete(kPath);
}

ACS_TEST(FoundationOptimizationWaveK, SceneDependenciesDeduplicateAndKeepErrorOrder)
{
    FScenePackReader SharedReader(EScenePackMode::SharedDependency);
    FScene3D SharedScene;
    const FScene3DLoadResult SharedResult = TryLoadScene3DAssetPack(SharedScene, SharedReader, "main.acscene");
    EXPECT_TRUE(SharedResult.Succeeded());
    EXPECT_EQ(SharedResult.DependenciesLoaded, 2u);
    EXPECT_EQ(SharedReader.BatchCalls, 1u);
    EXPECT_EQ(SharedReader.LastBatchCount, 1u);
    EXPECT_EQ(SharedReader.DependencyReadCalls, 1u);

    FScenePackReader FailureReader(EScenePackMode::EarlierDecodeFailure);
    FScene3D FailureScene;
    FailureScene.Spawn(FStringView("Keep"));
    const FScene3DLoadResult FailureResult = TryLoadScene3DAssetPack(FailureScene, FailureReader, "main.acscene");
    EXPECT_EQ(static_cast<u32>(FailureResult.Error), static_cast<u32>(EScene3DSerializeError::MaterialDecodeFailed));
    EXPECT_TRUE(FailureReader.StableFirstSeen);
    EXPECT_EQ(FailureReader.BatchCalls, 1u);
    EXPECT_EQ(FailureReader.LastBatchCount, 2u);
    EXPECT_EQ(FailureReader.DependencyReadCalls, 1u);
    EXPECT_EQ(FailureScene.NodeCount(), 2u);
    EXPECT_TRUE(FailureScene.FindByName(FStringView("Keep")) != nullptr);
}

ACS_TEST(FoundationOptimizationWaveK, PublicLayoutReport)
{
    std::printf("[WaveK] layout-v2 FAssetPathInternerDiagnostics=%zu/%zu FAssetRegistryDiagnostics=%zu/%zu FAcpakReadDiagnostics=%zu/%zu FAssetPackReadRequest=%zu/%zu FAssetPathInterner=%zu/%zu FAcpakReader=%zu/%zu\n", sizeof(FAssetPathInternerDiagnostics), alignof(FAssetPathInternerDiagnostics), sizeof(FAssetRegistryDiagnostics), alignof(FAssetRegistryDiagnostics), sizeof(assetpack::FAcpakReadDiagnostics), alignof(assetpack::FAcpakReadDiagnostics), sizeof(FAssetPackReadRequest), alignof(FAssetPackReadRequest), sizeof(FAssetPathInterner), alignof(FAssetPathInterner), sizeof(assetpack::FAcpakReader), alignof(assetpack::FAcpakReader));
    EXPECT_EQ(alignof(FAssetPathInternerDiagnostics), alignof(u64));
    EXPECT_EQ(alignof(FAssetRegistryDiagnostics), alignof(u64));
    EXPECT_EQ(alignof(assetpack::FAcpakReadDiagnostics), alignof(u64));
    EXPECT_EQ(alignof(FAssetPackReadRequest), alignof(void*));
}
