// SPDX-License-Identifier: Apache-2.0
// Asset path、package snapshot、batch I/O の所有権と失敗時状態を検証する。
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
#include "gameframework/SceneNodeGraph.h"
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

/** 二つの byte 列が完全一致するか調べる。 */
bool EqualBytes(const u8* Left, const u8* Right, usize Size) noexcept
{
    /** 比較する byte の添字。 */
    for (usize Index = 0u; Index < Size; ++Index) {
        if (Left[Index] != Right[Index]) return false;
    }
    return true;
}

/** byte 列の全要素が指定値か調べる。 */
bool AllBytesEqual(const u8* Bytes, usize Size, u8 Expected) noexcept
{
    /** 比較する byte の添字。 */
    for (usize Index = 0u; Index < Size; ++Index) {
        if (Bytes[Index] != Expected) return false;
    }
    return true;
}

/** 再現可能な疑似乱数 payload を作る。 */
void FillPayload(TArray<u8>& Bytes, usize Size, u32 Seed) noexcept
{
    if (!Bytes.TrySetNum(Size)) return;
    /** xorshift32 の現在状態。 */
    u32 State = Seed;
    /** payload を埋める添字。 */
    for (usize Index = 0u; Index < Size; ++Index) {
        State ^= State << 13u;
        State ^= State >> 17u;
        State ^= State << 5u;
        Bytes[Index] = static_cast<u8>(State >> 11u);
    }
}

/** raw payload 二件を持つ package を書き出す。 */
bool WriteTwoRawFiles(const wchar_t* Path, const TArray<u8>& First, const TArray<u8>& Second) noexcept
{
    /** 検証 package の writer。 */
    assetpack::CAcpakWriter Writer;
    if (Writer.Open(Path, assetpack::AcpakFlagNone).IsErr()) return false;
    if (Writer.AddFile(L"stream/first.bin", First.GetData(), First.Num()).IsErr()) {
        return false;
    }
    if (Writer.AddFile(L"stream/second.bin", Second.GetData(), Second.Num()).IsErr()) {
        return false;
    }
    /** package finalize の成否。 */
    const bool Succeeded = Writer.Finalize().IsOk();
    Writer.Close();
    return Succeeded;
}

/** 非同期 ownership 境界を制御できる停止型 loader。 */
class CBlockingWaveKLoader final : public IAssetLoader {
public:
    /** loader が生成する asset 型を返す。 */
    AssetType TypeId() const noexcept override
    {
        return ABinaryAsset::StaticType();
    }

    /** loader が担当する検証用拡張子を返す。 */
    const char* Extension() const noexcept override
    {
        return "wvk";
    }

    /** release 指示まで待って binary asset を生成する。 */
    TResult<TSharedPtr<AAsset>> LoadFromBytes(FAssetId, const TArray<byte>&) noexcept override
    {
        Entered.FetchAdd(1u);
        while (Release.Load(EMemoryOrder::Acquire) == 0u) Yield();
        /** 新規 binary asset の確保結果。 */
        auto Binary = MakeShared<ABinaryAsset>();
        if (!Binary.Get()) {
            return ACS_ERR(Memory, 1950u, "CBlockingWaveKLoader: allocation failed");
        }
        /** interface 型へ昇格した asset。 */
        TSharedPtr<AAsset> Asset(Move(Binary));
        return TResult<TSharedPtr<AAsset>>(OkInit, Move(Asset));
    }

    /** loader へ進入した回数。 */
    TAtomic<u32> Entered{0u};
    /** 停止中 loader の解放指示。 */
    TAtomic<u32> Release{0u};
};

/** registry shutdown thread と共有する状態。 */
struct FRegistryShutdownContext {
    /** shutdown 対象 registry。 */
    CAssetRegistry* Registry = nullptr;
    /** shutdown 完了時に 1 となる flag。 */
    TAtomic<u32> Finished{0u};
};

/** 別 thread から registry を shutdown する。 */
void ShutdownRegistry(void* User) noexcept
{
    /** 呼び出し元と共有する shutdown 状態。 */
    auto& Context = *static_cast<FRegistryShutdownContext*>(User);
    Context.Registry->Shutdown();
    Context.Finished.Store(1u, EMemoryOrder::Release);
}

/** 並行圧縮 read worker と共有する状態。 */
struct FCompressedReadContext {
    /** 読み取り対象 reader。 */
    assetpack::CAcpakReader* Reader = nullptr;
    /** 比較元 payload。 */
    const TArray<u8>* Expected = nullptr;
    /** worker 固有の出力先。 */
    TArray<u8>* Output = nullptr;
    /** start barrier 到着数。 */
    TAtomic<u32>* Ready = nullptr;
    /** 全 worker の開始指示。 */
    TAtomic<u32>* Start = nullptr;
    /** read または比較失敗数。 */
    TAtomic<u32>* Failures = nullptr;
};

/** 圧縮 payload を一回読み戻して検証する。 */
void ReadCompressedAsset(void* User) noexcept
{
    /** worker 固有の read 状態。 */
    auto& Context = *static_cast<FCompressedReadContext*>(User);
    Context.Ready->FetchAdd(1u);
    while (Context.Start->Load(EMemoryOrder::Acquire) == 0u) Yield();
    /** 圧縮 payload の read 結果。 */
    const auto Result = Context.Reader->ReadFile(L"compressed/payload.bin", Context.Output->GetData(), Context.Output->Num());
    if (Result.IsErr() || !EqualBytes(Context.Output->GetData(), Context.Expected->GetData(), Context.Expected->Num())) {
        Context.Failures->FetchAdd(1u);
    }
}

/** 既定 batch fallback の互換性を検証する legacy reader。 */
class CLegacyAssetPackReader final : public IAssetPackReader {
public:
    /** 検証 stub を mount 済みにする。 */
    TResult<void> Mount(const char*) noexcept override
    {
        return Ok();
    }

    /** 検証 stub を unmount する。 */
    void Unmount() noexcept override
    {
    }
    /** 検証 stub は常に mount 済みと返す。 */
    bool IsMounted() const noexcept override
    {
        return true;
    }

    /** 検証用 file 数を返す。 */
    TResult<u32> FileCount() noexcept override
    {
        return TResult<u32>(OkInit, 2u);
    }

    /** 検証用 file 名を返す。 */
    TResult<const char*> FileName(u32) noexcept override
    {
        return TResult<const char*>(OkInit, "legacy.bin");
    }

    /** 検証用 file size を返す。 */
    TResult<u64> FileSize(const char*) noexcept override
    {
        return TResult<u64>(OkInit, 1u);
    }

    /** 呼び出し順を byte 値へ記録して一件読む。 */
    TResult<void> ReadFile(const char* Name, u8* OutBuffer, u64 BufferSize) noexcept override
    {
        ++ReadCalls;
        if (std::strcmp(Name, "missing.bin") == 0) {
            return ACS_ERR(IO, 1951u, "CLegacyAssetPackReader: missing");
        }
        if (OutBuffer == nullptr || BufferSize < 1u) {
            return ACS_ERR(IO, 1952u, "CLegacyAssetPackReader: bad buffer");
        }
        OutBuffer[0] = static_cast<u8>(ReadCalls);
        return Ok();
    }

    /** 単一 file read の呼び出し数。 */
    u32 ReadCalls = 0u;
};

/** Scene dependency 検証 reader の動作種別。 */
enum class EScenePackMode : u8 {
    /** 同じ material を二 node で共有する。 */
    SharedDependency,
    /** 先行 decode 失敗と後続 read 失敗を発生させる。 */
    EarlierDecodeFailure,
};

/** Scene dependency の dedup と error 順を観測する reader。 */
class CScenePackReader final : public IAssetPackReader {
public:
    /** 検証動作種別を指定して構築する。 */
    explicit CScenePackReader(EScenePackMode Mode) noexcept : m_Mode(Mode)
    {
    }

    /** 検証 stub を mount 済みにする。 */
    TResult<void> Mount(const char*) noexcept override
    {
        return Ok();
    }
    /** 検証 stub を unmount する。 */
    void Unmount() noexcept override
    {
    }
    /** 検証 stub は常に mount 済みと返す。 */
    bool IsMounted() const noexcept override
    {
        return true;
    }

    /** 検証用 file 数を返す。 */
    TResult<u32> FileCount() noexcept override
    {
        return TResult<u32>(OkInit, 4u);
    }

    /** 検証用 scene 名を返す。 */
    TResult<const char*> FileName(u32) noexcept override
    {
        return TResult<const char*>(OkInit, "main.acscene");
    }

    /** 指定検証 asset の byte 数を返す。 */
    TResult<u64> FileSize(const char* Name) noexcept override
    {
        /** asset 名へ対応する検証 text。 */
        const char* Text = TextFor(Name);
        if (Text == nullptr) {
            return ACS_ERR(IO, 1953u, "CScenePackReader: missing size");
        }
        return TResult<u64>(OkInit, static_cast<u64>(std::strlen(Text)));
    }

    /** 指定検証 asset の text を読み出す。 */
    TResult<void> ReadFile(const char* Name, u8* OutBuffer, u64 BufferSize) noexcept override
    {
        if (std::strcmp(Name, "materials/missing.acsmat") == 0) {
            return ACS_ERR(IO, 1954u, "CScenePackReader: missing data");
        }
        /** asset 名へ対応する検証 text。 */
        const char* Text = TextFor(Name);
        if (Text == nullptr) {
            return ACS_ERR(IO, 1955u, "CScenePackReader: unknown path");
        }
        /** NUL を除く検証 text の byte 数。 */
        const usize Size = std::strlen(Text);
        if (OutBuffer == nullptr || BufferSize < Size) {
            return ACS_ERR(IO, 1956u, "CScenePackReader: bad buffer");
        }
        if (std::strcmp(Name, "main.acscene") != 0) ++DependencyReadCalls;
        MemCopy(OutBuffer, Text, Size);
        return Ok();
    }

    /** batch 順序を記録して既定 fallback へ委譲する。 */
    TResult<void> ReadFiles(const FAssetPackReadRequest* Requests, u32 Count, u32* CompletedCount) noexcept override
    {
        ++BatchCalls;
        LastBatchCount = Count;
        if (Count >= 2u) {
            StableFirstSeen = std::strcmp(Requests[0].Name, "materials/bad.acsmat") == 0 && std::strcmp(Requests[1].Name, "materials/missing.acsmat") == 0;
        }
        return IAssetPackReader::ReadFiles(Requests, Count, CompletedCount);
    }

    /** batch API の呼び出し数。 */
    u32 BatchCalls = 0u;
    /** 最後の batch request 数。 */
    u32 LastBatchCount = 0u;
    /** scene 本体を除く単一 read 数。 */
    u32 DependencyReadCalls = 0u;
    /** 期待する安定順序を観測したか。 */
    bool StableFirstSeen = false;

private:
    /** asset 名へ対応する検証 text を返す。 */
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

    /** material 一件を二 node が共有する scene。 */
    static constexpr char kSharedScene[] = "ACS3D v2\n"
                                           "N3D 1 -1 0 0 0 0 0 0 0 1 1 1 1 1 1 1 First\n"
                                           "MAT3D 1 materials/shared.acsmat\n"
                                           "N3D 2 -1 0 0 0 0 0 0 0 1 1 1 1 1 1 1 Second\n"
                                           "MAT3D 2 materials/shared.acsmat\n";

    /** 先行 decode と後続 read を失敗させる scene。 */
    static constexpr char kFailureScene[] = "ACS3D v2\n"
                                            "N3D 1 -1 0 0 0 0 0 0 0 1 1 1 1 1 1 1 First\n"
                                            "MAT3D 1 materials/bad.acsmat\n"
                                            "N3D 2 -1 0 0 0 0 0 0 0 1 1 1 1 1 1 1 Second\n"
                                            "MAT3D 2 materials/missing.acsmat\n";

    /** decode に成功する material text。 */
    static constexpr char kValidMaterial[] = "ACSMAT 1\nkind pbr\nmetallic 0.25\nroughness 0.75\n";
    /** decode に失敗する material text。 */
    static constexpr char kInvalidMaterial[] = "invalid\n";
    /** FileSize だけ成功させる placeholder。 */
    static constexpr char kMissingPlaceholder[] = "none";

    /** reader の検証動作種別。 */
    EScenePackMode m_Mode;
};

} // namespace

ACS_TEST(FoundationOptimizationWaveK, InternerIsBoundedAndPinnedPathsSurviveReset)
{
    /** 同一 path 共有を検証する先頭 path。 */
    constexpr const wchar_t* kFirstPath = L"Assets/Materials/SharedCloud.acsmat";
    /** 先頭 path の NUL を除く長さ。 */
    constexpr usize kFirstLength = 35u;
    static_assert(kFirstPath[kFirstLength] == L'\0');

    /** 有界保持と参照寿命を検証する interner。 */
    CAssetPathInterner Interner;
    /** 先頭 path の初回 intern 結果。 */
    auto FirstResult = Interner.Intern(kFirstPath, kFirstLength);
    /** 先頭 path の再 intern 結果。 */
    auto SecondResult = Interner.Intern(kFirstPath, kFirstLength);
    EXPECT_TRUE(FirstResult.IsOk());
    EXPECT_TRUE(SecondResult.IsOk());
    if (FirstResult.IsErr() || SecondResult.IsErr()) return;

    /** 初回 intern で得た共有 path。 */
    TSharedPtr<FInternedAssetPath> First = FirstResult.Value();
    /** 再 intern で得た共有 path。 */
    TSharedPtr<FInternedAssetPath> Second = SecondResult.Value();
    EXPECT_TRUE(First.Get() == Second.Get());

    /** eviction を防ぐ参照保持配列。 */
    TArray<TSharedPtr<FInternedAssetPath>> Pins;
    EXPECT_TRUE(Pins.TryReserve(kAssetPathInternerMaxEntries));
    EXPECT_TRUE(Pins.TryAdd(First));
    /** interner 上限まで異なる path を追加する添字。 */
    for (usize Index = 1u; Index < kAssetPathInternerMaxEntries; ++Index) {
        /** 現在追加する path の整形先。 */
        wchar_t Path[64]{};
        /** 現在 path の整形後文字数。 */
        const int Length = std::swprintf(Path, sizeof(Path) / sizeof(Path[0]), L"Assets/Interned/%03zu.asset", Index);
        EXPECT_TRUE(Length > 0);
        if (Length <= 0) return;
        /** 現在 path の intern 結果。 */
        auto Result = Interner.Intern(Path, static_cast<usize>(Length));
        EXPECT_TRUE(Result.IsOk());
        if (Result.IsErr()) return;
        EXPECT_TRUE(Pins.TryAdd(Result.Value()));
    }

    /** 上限到達後に非保持で返す path の intern 結果。 */
    auto OverflowResult = Interner.Intern(L"Assets/Interned/overflow.asset", 30u);
    EXPECT_TRUE(OverflowResult.IsOk());
    if (OverflowResult.IsErr()) return;
    /** interner reset 後も生存させる非保持 path。 */
    TSharedPtr<FInternedAssetPath> Overflow = OverflowResult.Value();

    /** 上限到達時の interner 診断値。 */
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
    /** 非同期 registry 検証用 file path。 */
    constexpr const wchar_t* kPath = L"acs_wave_k_registry.wvk";
    /** loader へ渡す検証 payload。 */
    constexpr u8 kPayload[] = {9u, 7u, 5u, 3u};
    (void)CFileSystem::Delete(kPath);
    EXPECT_TRUE(CFileSystem::WriteAllBytes(kPath, kPayload, sizeof(kPayload)).IsOk());

    CThreadPool::Shutdown();
    EXPECT_TRUE(CThreadPool::Init(2u).IsOk());

    /** 非同期 job の完了境界を制御する loader。 */
    CBlockingWaveKLoader Loader;
    /** path ownership と coalescing を検証する registry。 */
    CAssetRegistry Registry;
    Registry.RegisterLoader(&Loader);

    /** physical read を開始する最初の future。 */
    FAssetFuture First = Registry.LoadAsync(kPath);
    /** loader 進入を待つ最大反復。 */
    for (u32 Wait = 0u; Wait < 2000u && Loader.Entered.Load(EMemoryOrder::Acquire) < 1u; ++Wait) {
        SleepMs(1u);
    }
    /** 進行中 job へ合流する future。 */
    FAssetFuture Coalesced = Registry.LoadAsync(kPath);
    /** job 完了前の coalescing 診断値。 */
    const FAssetRegistryDiagnostics Pending = Registry.Diagnostics();
    EXPECT_EQ(Pending.async_request_count, 2ull);
    EXPECT_EQ(Pending.async_coalesced_count, 1ull);
    EXPECT_EQ(Pending.async_job_count, 1ull);
    EXPECT_EQ(Pending.physical_file_read_count, 1ull);
    EXPECT_EQ(Pending.path_interner.request_count, 1ull);
    EXPECT_EQ(Pending.path_interner.miss_count, 1ull);

    Loader.Release.Store(1u, EMemoryOrder::Release);
    /** 最初の非同期 load 結果。 */
    const auto FirstResult = First.Get();
    /** 合流した非同期 load 結果。 */
    const auto CoalescedResult = Coalesced.Get();
    EXPECT_TRUE(FirstResult.IsOk());
    EXPECT_TRUE(CoalescedResult.IsOk());
    if (FirstResult.IsErr() || CoalescedResult.IsErr()) return;
    EXPECT_TRUE(FirstResult.Value().Get() == CoalescedResult.Value().Get());

    /** cache 解除に使う asset ID。 */
    const FAssetId Id = FirstResult.Value()->Id();
    Registry.Unload(Id);
    /** intern 済み path を再利用する future。 */
    FAssetFuture Repeated = Registry.LoadAsync(kPath);
    EXPECT_TRUE(Repeated.Get().IsOk());

    /** 再 load 後の path hit 診断値。 */
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
    /** shutdown と競合させる進行中 future。 */
    FAssetFuture DuringShutdown = Registry.LoadAsync(kPath);
    /** 三回目の loader 進入を待つ最大反復。 */
    for (u32 Wait = 0u; Wait < 2000u && Loader.Entered.Load(EMemoryOrder::Acquire) < 3u; ++Wait) {
        SleepMs(1u);
    }
    EXPECT_EQ(Loader.Entered.Load(EMemoryOrder::Acquire), 3u);

    /** shutdown thread と共有する完了状態。 */
    FRegistryShutdownContext ShutdownContext;
    ShutdownContext.Registry = &Registry;
    /** registry shutdown を実行する thread の生成結果。 */
    auto ShutdownThread = FThread::Spawn(&ShutdownRegistry, &ShutdownContext);
    EXPECT_TRUE(ShutdownThread.IsOk());
    SleepMs(10u);
    EXPECT_EQ(ShutdownContext.Finished.Load(EMemoryOrder::Acquire), 0u);

    Loader.Release.Store(1u, EMemoryOrder::Release);
    EXPECT_TRUE(DuringShutdown.Get().IsOk());
    if (ShutdownThread.IsOk()) ShutdownThread.Value().Join();
    EXPECT_EQ(ShutdownContext.Finished.Load(EMemoryOrder::Acquire), 1u);

    CThreadPool::Shutdown();
    (void)CFileSystem::Delete(kPath);
}

ACS_TEST(FoundationOptimizationWaveK, LegacyBatchFallbackReportsPartialCommit)
{
    /** 既定 batch fallback を使う legacy reader。 */
    CLegacyAssetPackReader Reader;
    /** 先頭成功 request の出力。 */
    u8 First = 0xCCu;
    /** 失敗 request の未変更出力。 */
    u8 Missing = 0xCCu;
    /** 未実行 request の未変更出力。 */
    u8 Last = 0xCCu;
    /** 部分失敗を起こす request 配列。 */
    FAssetPackReadRequest Requests[] = {{"first.bin", &First, 1u}, {"missing.bin", &Missing, 1u}, {"last.bin", &Last, 1u}};
    /** reader が書き戻す成功済み request 数。 */
    u32 Completed = 99u;
    /** legacy fallback の batch 結果。 */
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
    /** mapping snapshot 検証用 package path。 */
    constexpr const wchar_t* kPath = L"acs_wave_k_mapped.acpak";
    (void)CFileSystem::Delete(kPath);

    /** 置換前 package の先頭 payload。 */
    TArray<u8> OldFirst;
    /** 置換前 package の二番目 payload。 */
    TArray<u8> OldSecond;
    /** 置換後 package の先頭 payload。 */
    TArray<u8> NewFirst;
    /** 置換後 package の二番目 payload。 */
    TArray<u8> NewSecond;
    FillPayload(OldFirst, 192u * 1024u, 0x12345678u);
    FillPayload(OldSecond, 128u * 1024u, 0x87654321u);
    FillPayload(NewFirst, OldFirst.Num(), 0xCAFEBABEu);
    FillPayload(NewSecond, OldSecond.Num(), 0x10203040u);
    EXPECT_EQ(OldFirst.Num(), 192u * 1024u);
    EXPECT_EQ(OldSecond.Num(), 128u * 1024u);
    EXPECT_TRUE(WriteTwoRawFiles(kPath, OldFirst, OldSecond));

    /** 置換前 snapshot を保持する reader。 */
    assetpack::CAcpakReader OldReader;
    EXPECT_TRUE(OldReader.Open(kPath).IsOk());
    EXPECT_TRUE(OldReader.ReadDiagnostics().mapped_view_active);
    EXPECT_TRUE(WriteTwoRawFiles(kPath, NewFirst, NewSecond));

    /** 先頭 payload の読み戻し先。 */
    TArray<u8> FirstOut;
    /** 二番目 payload の読み戻し先。 */
    TArray<u8> SecondOut;
    EXPECT_TRUE(FirstOut.TrySetNum(OldFirst.Num()));
    EXPECT_TRUE(SecondOut.TrySetNum(OldSecond.Num()));
    /** batch で読む path 配列。 */
    const wchar_t* Paths[] = {L"stream/first.bin", L"stream/second.bin"};
    /** batch の出力先配列。 */
    void* Buffers[] = {FirstOut.GetData(), SecondOut.GetData()};
    /** batch の出力容量配列。 */
    const u64 Sizes[] = {static_cast<u64>(FirstOut.Num()), static_cast<u64>(SecondOut.Num())};
    /** batch が完了した先頭 request 数。 */
    u32 Completed = 99u;
    /** 置換前 snapshot からの batch read 結果。 */
    const auto BatchResult = OldReader.ReadFiles(Paths, Buffers, Sizes, 2u, &Completed);
    EXPECT_TRUE(BatchResult.IsOk());
    EXPECT_EQ(Completed, 2u);
    EXPECT_TRUE(EqualBytes(FirstOut.GetData(), OldFirst.GetData(), OldFirst.Num()));
    EXPECT_TRUE(EqualBytes(SecondOut.GetData(), OldSecond.GetData(), OldSecond.Num()));

    /** mapping batch 後の read 診断値。 */
    const assetpack::FAcpakReadDiagnostics BatchDiagnostics = OldReader.ReadDiagnostics();
    EXPECT_EQ(BatchDiagnostics.batch_count, 1ull);
    EXPECT_EQ(BatchDiagnostics.batch_entry_count, 2ull);
    EXPECT_EQ(BatchDiagnostics.mapped_read_count, 2ull);
    EXPECT_EQ(BatchDiagnostics.buffered_read_count, 0ull);

    /** 置換後 snapshot を読む reader。 */
    assetpack::CAcpakReader NewReader;
    EXPECT_TRUE(NewReader.Open(kPath).IsOk());
    EXPECT_TRUE(NewReader.ReadFile(L"stream/first.bin", FirstOut.GetData(), FirstOut.Num()).IsOk());
    EXPECT_TRUE(EqualBytes(FirstOut.GetData(), NewFirst.GetData(), NewFirst.Num()));

    // bridge も後続 UTF-8 エラーより先行 read を優先し、部分完了数を維持する。
    /** UTF-8 batch 変換を検証する bridge reader。 */
    assetpack::CAcpakGameReader BridgeReader;
    EXPECT_TRUE(BridgeReader.Mount("acs_wave_k_mapped.acpak").IsOk());
    MemSet(FirstOut.GetData(), 0xCCu, FirstOut.Num());
    /** 二番目 request で拒否する不正 UTF-8。 */
    char InvalidUtf8[] = {static_cast<char>(0xFFu), '\0'};
    /** 変換失敗 request の未変更出力。 */
    u8 BridgeMissingOut = 0xCCu;
    /** 先行 read 後に UTF-8 変換失敗する request 群。 */
    FAssetPackReadRequest BridgeRequests[] = {{"stream/first.bin", FirstOut.GetData(), FirstOut.Num()}, {InvalidUtf8, &BridgeMissingOut, 1u}};
    /** bridge が書き戻す成功済み request 数。 */
    u32 BridgeCompleted = 99u;
    /** bridge の部分完了 batch 結果。 */
    const auto BridgeResult = BridgeReader.ReadFiles(BridgeRequests, 2u, &BridgeCompleted);
    EXPECT_TRUE(BridgeResult.IsErr());
    EXPECT_EQ(BridgeCompleted, 1u);
    EXPECT_TRUE(EqualBytes(FirstOut.GetData(), NewFirst.GetData(), NewFirst.Num()));
    EXPECT_EQ(BridgeMissingOut, 0xCCu);
    BridgeReader.Unmount();

    // 後続失敗では先行出力だけ commit され、完了数が明示される。
    MemSet(FirstOut.GetData(), 0xCCu, FirstOut.Num());
    MemSet(SecondOut.GetData(), 0xCCu, SecondOut.Num());
    /** 見つからない request の未変更出力。 */
    u8 MissingOut = 0xCCu;
    /** 二番目が見つからない batch path 群。 */
    const wchar_t* PartialPaths[] = {L"stream/first.bin", L"missing.bin", L"stream/second.bin"};
    /** 部分失敗 batch の出力先群。 */
    void* PartialBuffers[] = {FirstOut.GetData(), &MissingOut, SecondOut.GetData()};
    /** 部分失敗 batch の出力容量群。 */
    const u64 PartialSizes[] = {static_cast<u64>(FirstOut.Num()), 1u, static_cast<u64>(SecondOut.Num())};
    Completed = 99u;
    /** 見つからない二番目 request を含む batch 結果。 */
    const auto PartialResult = NewReader.ReadFiles(PartialPaths, PartialBuffers, PartialSizes, 3u, &Completed);
    EXPECT_TRUE(PartialResult.IsErr());
    EXPECT_EQ(Completed, 1u);
    EXPECT_TRUE(EqualBytes(FirstOut.GetData(), NewFirst.GetData(), NewFirst.Num()));
    EXPECT_EQ(MissingOut, 0xCCu);
    EXPECT_TRUE(AllBytesEqual(SecondOut.GetData(), SecondOut.Num(), 0xCCu));

    // Release 参考値: 同一 bytes/CRC workload で lock 境界だけを比較する。
    /** sequential と batch を比較する反復数。 */
    constexpr u32 kMeasureIterations = 24u;
    /** sequential 計測の開始時刻。 */
    auto SequentialBegin = std::chrono::steady_clock::now();
    /** sequential read を繰り返す添字。 */
    for (u32 Index = 0u; Index < kMeasureIterations; ++Index) {
        (void)NewReader.ReadFile(Paths[0], FirstOut.GetData(), FirstOut.Num());
        (void)NewReader.ReadFile(Paths[1], SecondOut.GetData(), SecondOut.Num());
    }
    /** sequential 計測の終了時刻。 */
    auto SequentialEnd = std::chrono::steady_clock::now();
    /** batch 計測の開始時刻。 */
    auto BatchBegin = std::chrono::steady_clock::now();
    /** batch read を繰り返す添字。 */
    for (u32 Index = 0u; Index < kMeasureIterations; ++Index) {
        (void)NewReader.ReadFiles(Paths, Buffers, Sizes, 2u);
    }
    /** batch 計測の終了時刻。 */
    auto BatchEnd = std::chrono::steady_clock::now();
    /** sequential read の経過 microsecond。 */
    const auto SequentialUs = std::chrono::duration_cast<std::chrono::microseconds>(SequentialEnd - SequentialBegin).count();
    /** batch read の経過 microsecond。 */
    const auto BatchUs = std::chrono::duration_cast<std::chrono::microseconds>(BatchEnd - BatchBegin).count();
    std::printf("[WaveK] mapped-read iterations=%u sequential_us=%lld batch_us=%lld bytes_per_iteration=%zu\n", kMeasureIterations, static_cast<long long>(SequentialUs), static_cast<long long>(BatchUs), OldFirst.Num() + OldSecond.Num());

    OldReader.Close();
    NewReader.Close();

    // raw mapped 経路でも CRC 成功前に caller buffer を変更しない。
    /** payload offset を得るための reader。 */
    assetpack::CAcpakReader OffsetReader;
    EXPECT_TRUE(OffsetReader.Open(kPath).IsOk());
    /** 破損させる先頭 manifest entry。 */
    const assetpack::FAcpakFileEntry* Entry = OffsetReader.FindEntry(L"stream/first.bin");
    EXPECT_TRUE(Entry != nullptr);
    /** package 内で破損させる payload offset。 */
    const u64 DataOffset = Entry != nullptr ? Entry->offset : 0u;
    OffsetReader.Close();

    /** package 全体の読み込み結果。 */
    auto ArchiveResult = CFileSystem::ReadAllBytes(kPath);
    EXPECT_TRUE(ArchiveResult.IsOk());
    if (ArchiveResult.IsOk() && DataOffset < ArchiveResult.Value().Num()) {
        ArchiveResult.Value()[static_cast<usize>(DataOffset)] ^= 0x5Au;
        EXPECT_TRUE(CFileSystem::WriteAllBytes(kPath, ArchiveResult.Value().GetData(), ArchiveResult.Value().Num()).IsOk());
    }

    /** CRC 失敗時の transaction 出力を検証する reader。 */
    assetpack::CAcpakReader CorruptReader;
    EXPECT_TRUE(CorruptReader.Open(kPath).IsOk());
    MemSet(FirstOut.GetData(), 0xA5u, FirstOut.Num());
    /** 破損 payload の read 結果。 */
    const auto CorruptResult = CorruptReader.ReadFile(L"stream/first.bin", FirstOut.GetData(), FirstOut.Num());
    EXPECT_TRUE(CorruptResult.IsErr());
    EXPECT_TRUE(AllBytesEqual(FirstOut.GetData(), FirstOut.Num(), 0xA5u));
    CorruptReader.Close();
    (void)CFileSystem::Delete(kPath);
}

ACS_TEST(FoundationOptimizationWaveK, CompressedScratchIsBoundedAndConcurrentSafe)
{
    /** 圧縮 scratch 検証用 package path。 */
    constexpr const wchar_t* kPath = L"acs_wave_k_compressed.acpak";
    /** 同時 read worker 数。 */
    constexpr u32 kWorkerCount = 4u;
    (void)CFileSystem::Delete(kPath);

    /** 圧縮して格納する検証 payload。 */
    TArray<u8> Payload;
    FillPayload(Payload, 2u * 1024u * 1024u, 0x31415926u);
    EXPECT_EQ(Payload.Num(), 2u * 1024u * 1024u);

    /** 圧縮 package の writer。 */
    assetpack::CAcpakWriter Writer;
    EXPECT_TRUE(Writer.Open(kPath, assetpack::AcpakFlagCompressed).IsOk());
    EXPECT_TRUE(Writer.AddFile(L"compressed/payload.bin", Payload.GetData(), Payload.Num()).IsOk());
    EXPECT_TRUE(Writer.Finalize().IsOk());
    Writer.Close();

    /** scratch 再利用と並行 fallback を検証する reader。 */
    assetpack::CAcpakReader Reader;
    EXPECT_TRUE(Reader.Open(kPath).IsOk());
    /** 保持 scratch を warmup する出力先。 */
    TArray<u8> WarmOutput;
    EXPECT_TRUE(WarmOutput.TrySetNum(Payload.Num()));
    EXPECT_TRUE(Reader.ReadFile(L"compressed/payload.bin", WarmOutput.GetData(), WarmOutput.Num()).IsOk());
    EXPECT_TRUE(EqualBytes(WarmOutput.GetData(), Payload.GetData(), Payload.Num()));

    Reader.ResetReadDiagnostics();
    /** 各 worker 固有の出力配列。 */
    TArray<u8> Outputs[kWorkerCount];
    /** 各 worker へ渡す read context。 */
    FCompressedReadContext Contexts[kWorkerCount]{};
    /** 生成済み worker thread 群。 */
    FThread Workers[kWorkerCount];
    /** start barrier へ到着した worker 数。 */
    TAtomic<u32> Ready{0u};
    /** 全 worker の同時開始指示。 */
    TAtomic<u32> Start{0u};
    /** read または payload 比較失敗数。 */
    TAtomic<u32> Failures{0u};
    /** 生成に成功した worker 数。 */
    u32 StartedWorkers = 0u;
    /** worker を構築する添字。 */
    for (u32 Index = 0u; Index < kWorkerCount; ++Index) {
        EXPECT_TRUE(Outputs[Index].TrySetNum(Payload.Num()));
        Contexts[Index].Reader = &Reader;
        Contexts[Index].Expected = &Payload;
        Contexts[Index].Output = &Outputs[Index];
        Contexts[Index].Ready = &Ready;
        Contexts[Index].Start = &Start;
        Contexts[Index].Failures = &Failures;
        /** 現在 worker thread の生成結果。 */
        auto SpawnResult = FThread::Spawn(&ReadCompressedAsset, &Contexts[Index]);
        if (SpawnResult.IsOk()) {
            Workers[StartedWorkers++] = Move(SpawnResult.Value());
        }
    }
    EXPECT_EQ(StartedWorkers, kWorkerCount);
    while (Ready.Load(EMemoryOrder::Acquire) < StartedWorkers) Yield();
    Start.Store(1u, EMemoryOrder::Release);
    /** 生成済み worker を join する添字。 */
    for (u32 Index = 0u; Index < StartedWorkers; ++Index) {
        Workers[Index].Join();
    }

    EXPECT_EQ(Failures.Load(EMemoryOrder::Acquire), 0u);
    /** 並行 read 完了後の scratch 診断値。 */
    const assetpack::FAcpakReadDiagnostics Diagnostics = Reader.ReadDiagnostics();
    EXPECT_EQ(Diagnostics.mapped_read_count + Diagnostics.buffered_read_count, static_cast<u64>(kWorkerCount));
    EXPECT_EQ(Diagnostics.scratch_reuse_count + Diagnostics.scratch_fallback_count, static_cast<u64>(kWorkerCount));
    EXPECT_TRUE(Diagnostics.retained_scratch_bytes <= static_cast<u64>(assetpack::kAcpakRetainedScratchMaxBytes * 2u));
    std::printf("[WaveK] compressed-read workers=%u reuse=%llu fallback=%llu retained_bytes=%llu mapped=%d\n", kWorkerCount, static_cast<unsigned long long>(Diagnostics.scratch_reuse_count), static_cast<unsigned long long>(Diagnostics.scratch_fallback_count), static_cast<unsigned long long>(Diagnostics.retained_scratch_bytes), Diagnostics.mapped_view_active ? 1 : 0);

    Reader.ResetReadDiagnostics();
    /** 明示 reset 後の診断値。 */
    const assetpack::FAcpakReadDiagnostics Reset = Reader.ReadDiagnostics();
    EXPECT_EQ(Reset.mapped_read_count, 0ull);
    EXPECT_EQ(Reset.buffered_read_count, 0ull);
    EXPECT_EQ(Reset.scratch_reuse_count, 0ull);
    EXPECT_EQ(Reset.scratch_fallback_count, 0ull);

    Reader.Close();
    (void)CFileSystem::Delete(kPath);
}

ACS_TEST(FoundationOptimizationWaveK, SceneDependenciesDeduplicateAndKeepErrorOrder)
{
    /** 同一 material の共有を返す reader。 */
    CScenePackReader SharedReader(EScenePackMode::SharedDependency);
    /** shared dependency の load 先 scene。 */
    CSceneNodeGraph SharedScene;
    /** shared dependency scene の load 結果。 */
    const FScene3DLoadResult SharedResult = TryLoadScene3DAssetPack(SharedScene, SharedReader, "main.acscene");
    EXPECT_TRUE(SharedResult.Succeeded());
    EXPECT_EQ(SharedResult.DependenciesLoaded, 2u);
    EXPECT_EQ(SharedReader.BatchCalls, 1u);
    EXPECT_EQ(SharedReader.LastBatchCount, 1u);
    EXPECT_EQ(SharedReader.DependencyReadCalls, 1u);

    /** 先行 decode と後続 read を失敗させる reader。 */
    CScenePackReader FailureReader(EScenePackMode::EarlierDecodeFailure);
    /** transaction 不変性を確認する既存 node 付き scene。 */
    CSceneNodeGraph FailureScene;
    FailureScene.Spawn(FStringView("Keep"));
    /** 複合失敗 scene の load 結果。 */
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
    std::printf("[WaveK] layout-v2 FAssetPathInternerDiagnostics=%zu/%zu FAssetRegistryDiagnostics=%zu/%zu FAcpakReadDiagnostics=%zu/%zu FAssetPackReadRequest=%zu/%zu CAssetPathInterner=%zu/%zu CAcpakReader=%zu/%zu\n", sizeof(FAssetPathInternerDiagnostics), alignof(FAssetPathInternerDiagnostics), sizeof(FAssetRegistryDiagnostics), alignof(FAssetRegistryDiagnostics), sizeof(assetpack::FAcpakReadDiagnostics), alignof(assetpack::FAcpakReadDiagnostics), sizeof(FAssetPackReadRequest), alignof(FAssetPackReadRequest), sizeof(CAssetPathInterner), alignof(CAssetPathInterner), sizeof(assetpack::CAcpakReader), alignof(assetpack::CAcpakReader));
    EXPECT_EQ(alignof(FAssetPathInternerDiagnostics), alignof(u64));
    EXPECT_EQ(alignof(FAssetRegistryDiagnostics), alignof(u64));
    EXPECT_EQ(alignof(assetpack::FAcpakReadDiagnostics), alignof(u64));
    EXPECT_EQ(alignof(FAssetPackReadRequest), alignof(void*));
}
