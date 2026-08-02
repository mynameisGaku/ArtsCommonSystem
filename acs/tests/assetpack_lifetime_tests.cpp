// SPDX-License-Identifier: Apache-2.0
#include "test/Expect.h"
#include "test/Test.h"

#include "assetpack/AcpakFormat.h"
#include "assetpack/AcpakGameBridge.h"
#include "assetpack/AcpakReader.h"
#include "assetpack/AcpakWriter.h"
#include "foundation/Move.h"
#include "memory/Memory.h"
#include "platform/FileSystem.h"
#include "threading/Atomic.h"
#include "threading/Thread.h"

using namespace acs;

namespace {

/** allocation 前の破損入力拒否と OOM 伝播を区別する失敗 allocator。 */
class CCountingFailAllocator final : public IAllocator
{
public:
    void* Alloc(usize /*Size*/, usize /*Alignment*/, FSourceLoc /*Location*/) noexcept override
    {
        AllocationAttempts.FetchAdd(1u);
        return nullptr;
    }

    void Free(void* /*Pointer*/) noexcept override
    {
    }

    TAtomic<u32> AllocationAttempts{0};
};

void WriteU32(u8* Destination, u32 Value) noexcept
{
    MemCopy(Destination, &Value, sizeof(Value));
}

void WriteU64(u8* Destination, u64 Value) noexcept
{
    MemCopy(Destination, &Value, sizeof(Value));
}

bool EqualBytes(const u8* Left, const u8* Right, usize Size) noexcept
{
    for (usize Index = 0; Index < Size; ++Index)
    {
        if (Left[Index] != Right[Index])
        {
            return false;
        }
    }
    return true;
}

bool EqualText(const char* Left, const char* Right) noexcept
{
    if (Left == nullptr || Right == nullptr)
    {
        return Left == Right;
    }
    while (*Left != 0 && *Left == *Right)
    {
        ++Left;
        ++Right;
    }
    return *Left == *Right;
}

void CopyText(char* Destination, usize Capacity, const char* Source) noexcept
{
    usize Index = 0;
    while (Index + 1u < Capacity && Source[Index] != 0)
    {
        Destination[Index] = Source[Index];
        ++Index;
    }
    Destination[Index] = 0;
}

struct FConcurrentReadContext
{
    assetpack::CAcpakGameReader* Reader = nullptr;
    TAtomic<u32> Ready{0};
    TAtomic<u32> Start{0};
    TAtomic<u32> Stop{0};
    TAtomic<u32> Successes{0};
    TAtomic<u32> Failures{0};
    u8 Expected[8] = {};
};

void ConcurrentReadWorker(void* User) noexcept
{
    auto& Context = *static_cast<FConcurrentReadContext*>(User);
    Context.Ready.FetchAdd(1);
    while (Context.Start.Load() == 0)
    {
        Yield();
    }

    while (Context.Stop.Load() == 0)
    {
        u8 Buffer[sizeof(Context.Expected)] = {};
        const auto ReadResult = Context.Reader->ReadFile(
            "concurrent/data.bin", Buffer, sizeof(Buffer));
        if (ReadResult.IsOk())
        {
            if (!EqualBytes(Buffer, Context.Expected, sizeof(Buffer)))
            {
                Context.Failures.FetchAdd(1);
            }
            Context.Successes.FetchAdd(1);
        }
    }
}

} // namespace

ACS_TEST(AcpakLifetime, RejectsHugeFileCountBeforeAllocation)
{
    constexpr const wchar_t* kPath = L"acs_acpak_huge_count_test.acpak";
    (void)CFileSystem::Delete(kPath);

    u8 Bytes[assetpack::kAcpakHeaderDiskSize] = {};
    MemCopy(Bytes, assetpack::kAcpakMagic, sizeof(assetpack::kAcpakMagic));
    WriteU32(Bytes + 8, assetpack::kAcpakVersion);
    WriteU32(Bytes + 12, assetpack::AcpakFlagNone);
    WriteU32(Bytes + 16, 0xFFFFFFFFu);
    WriteU64(Bytes + 24, assetpack::kAcpakHeaderDiskSize);
    EXPECT_TRUE(CFileSystem::WriteAllBytes(kPath, Bytes, sizeof(Bytes)).IsOk());

    CCountingFailAllocator Allocator;
    assetpack::CAcpakReader Reader(Allocator);
    const auto OpenResult = Reader.Open(kPath);
    EXPECT_TRUE(OpenResult.IsErr());
    if (OpenResult.IsErr())
    {
        EXPECT_EQ(OpenResult.Error().subcode, assetpack::kAcpakSubBadSize);
    }
    EXPECT_EQ(Allocator.AllocationAttempts.Load(), 0u);
    (void)CFileSystem::Delete(kPath);
}

ACS_TEST(AcpakLifetime, PropagatesReaderAllocationFailure)
{
    constexpr const wchar_t* kPath = L"acs_acpak_reader_oom_test.acpak";
    constexpr const wchar_t* kVirtualPath = L"oom/data.bin";
    constexpr u8 kPayload[] = {4, 2, 1};
    (void)CFileSystem::Delete(kPath);

    assetpack::CAcpakWriter Writer;
    EXPECT_TRUE(Writer.Open(kPath, assetpack::AcpakFlagNone).IsOk());
    EXPECT_TRUE(Writer.AddFile(kVirtualPath, kPayload, sizeof(kPayload)).IsOk());
    EXPECT_TRUE(Writer.Finalize().IsOk());
    Writer.Close();

    CCountingFailAllocator Allocator;
    assetpack::CAcpakReader Reader(Allocator);
    const auto OpenResult = Reader.Open(kPath);
    EXPECT_TRUE(OpenResult.IsErr());
    if (OpenResult.IsErr())
    {
        EXPECT_EQ(static_cast<u16>(OpenResult.Error().category),
                  static_cast<u16>(EErrCategory::Memory));
        EXPECT_EQ(OpenResult.Error().subcode, assetpack::kAcpakSubOutOfMemory);
    }
    EXPECT_TRUE(Allocator.AllocationAttempts.Load() > 0u);
    (void)CFileSystem::Delete(kPath);
}

ACS_TEST(AcpakLifetime, PropagatesWriterInputCopyAllocationFailure)
{
    constexpr const wchar_t* kPath = L"acs_acpak_writer_oom_test.acpak";
    constexpr u8 kPayload[] = {9, 8, 7};
    (void)CFileSystem::Delete(kPath);

    CCountingFailAllocator Allocator;
    assetpack::CAcpakWriter Writer(Allocator);
    EXPECT_TRUE(Writer.Open(kPath, assetpack::AcpakFlagNone).IsOk());
    const auto AddResult = Writer.AddFile(L"oom/data.bin", kPayload, sizeof(kPayload));
    EXPECT_TRUE(AddResult.IsErr());
    if (AddResult.IsErr())
    {
        EXPECT_EQ(static_cast<u16>(AddResult.Error().category),
                  static_cast<u16>(EErrCategory::Memory));
        EXPECT_EQ(AddResult.Error().subcode, assetpack::kAcpakSubOutOfMemory);
    }
    EXPECT_TRUE(Allocator.AllocationAttempts.Load() > 0u);
    Writer.Close();
    (void)CFileSystem::Delete(kPath);
}

ACS_TEST(AcpakLifetime, WriterOwnsTemporaryNamesAndPayloads)
{
    constexpr const char* kPath = "acs_acpak_owned_inputs_test.acpak";
    constexpr const wchar_t* kWidePath = L"acs_acpak_owned_inputs_test.acpak";
    constexpr u8 kFirstPayload[] = {1, 2, 3, 4};
    constexpr u8 kSecondPayload[] = {8, 7, 6, 5};
    (void)CFileSystem::Delete(kWidePath);

    assetpack::CAcpakGameWriter Writer;
    EXPECT_TRUE(Writer.BeginPack(kPath).IsOk());

    char Name[64] = {};
    u8 Payload[4] = {};
    CopyText(Name, sizeof(Name), "first/data.bin");
    MemCopy(Payload, kFirstPayload, sizeof(Payload));
    EXPECT_TRUE(Writer.AddFile(Name, Payload, sizeof(Payload)).IsOk());

    CopyText(Name, sizeof(Name), "second/data.bin");
    MemCopy(Payload, kSecondPayload, sizeof(Payload));
    EXPECT_TRUE(Writer.AddFile(Name, Payload, sizeof(Payload)).IsOk());

    CopyText(Name, sizeof(Name), "overwritten");
    MemSet(Payload, 0, sizeof(Payload));
    EXPECT_TRUE(Writer.FinishPack().IsOk());

    assetpack::CAcpakGameReader Reader;
    EXPECT_TRUE(Reader.Mount(kPath).IsOk());
    const auto FirstName = Reader.FileName(0);
    const auto SecondName = Reader.FileName(1);
    EXPECT_TRUE(FirstName.IsOk());
    EXPECT_TRUE(SecondName.IsOk());
    if (FirstName.IsOk() && SecondName.IsOk())
    {
        EXPECT_TRUE(EqualText(FirstName.Value(), "first/data.bin"));
        EXPECT_TRUE(EqualText(SecondName.Value(), "second/data.bin"));
        EXPECT_TRUE(FirstName.Value() != SecondName.Value());
    }

    u8 FirstRead[sizeof(kFirstPayload)] = {};
    u8 SecondRead[sizeof(kSecondPayload)] = {};
    EXPECT_TRUE(Reader.ReadFile("first/data.bin", FirstRead, sizeof(FirstRead)).IsOk());
    EXPECT_TRUE(Reader.ReadFile("second/data.bin", SecondRead, sizeof(SecondRead)).IsOk());
    EXPECT_TRUE(EqualBytes(FirstRead, kFirstPayload, sizeof(FirstRead)));
    EXPECT_TRUE(EqualBytes(SecondRead, kSecondPayload, sizeof(SecondRead)));

    Reader.Unmount();
    (void)CFileSystem::Delete(kWidePath);
}

ACS_TEST(AcpakLifetime, ConcurrentReadAndUnmountAreSerialized)
{
    constexpr const char* kPath = "acs_acpak_concurrent_unmount_test.acpak";
    constexpr const wchar_t* kWidePath = L"acs_acpak_concurrent_unmount_test.acpak";
    constexpr u8 kPayload[] = {11, 22, 33, 44, 55, 66, 77, 88};
    constexpr u32 kWorkerCount = 4;
    (void)CFileSystem::Delete(kWidePath);

    assetpack::CAcpakGameWriter Writer;
    EXPECT_TRUE(Writer.BeginPack(kPath).IsOk());
    EXPECT_TRUE(Writer.AddFile("concurrent/data.bin", kPayload, sizeof(kPayload)).IsOk());
    EXPECT_TRUE(Writer.FinishPack().IsOk());

    assetpack::CAcpakGameReader Reader;
    EXPECT_TRUE(Reader.Mount(kPath).IsOk());

    FConcurrentReadContext Context;
    Context.Reader = &Reader;
    MemCopy(Context.Expected, kPayload, sizeof(kPayload));

    FThread Workers[kWorkerCount];
    u32 StartedWorkers = 0;
    for (u32 Index = 0; Index < kWorkerCount; ++Index)
    {
        auto SpawnResult = FThread::Spawn(&ConcurrentReadWorker, &Context);
        if (SpawnResult.IsOk())
        {
            Workers[StartedWorkers++] = Move(SpawnResult.Value());
        }
    }
    EXPECT_EQ(StartedWorkers, kWorkerCount);
    while (Context.Ready.Load() != StartedWorkers)
    {
        Yield();
    }

    Context.Start.Store(1);
    for (u32 Wait = 0; Wait < 2000 && Context.Successes.Load() < 32u; ++Wait)
    {
        SleepMs(1);
    }
    Reader.Unmount();
    Context.Stop.Store(1);

    for (u32 Index = 0; Index < StartedWorkers; ++Index)
    {
        Workers[Index].Join();
    }

    EXPECT_TRUE(Context.Successes.Load() > 0u);
    EXPECT_EQ(Context.Failures.Load(), 0u);
    EXPECT_FALSE(Reader.IsMounted());
    (void)CFileSystem::Delete(kWidePath);
}
