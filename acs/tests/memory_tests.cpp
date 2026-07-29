// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "memory/SystemAllocator.h"
#include "memory/LinearAllocator.h"
#include "memory/PoolAllocator.h"
#include "memory/TypedPoolAllocator.h"
#include "memory/ArenaAllocator.h"
#include "memory/UniquePtr.h"
#include "memory/SharedPtr.h"
#include "memory/New.h"
#include "memory/Memory.h"
#include "foundation/Platform.h"
#include "foundation/Move.h"
#include "threading/Atomic.h"
#include "threading/Thread.h"

using namespace acs;

namespace {

/** 同一アドレス再構築の隔離プローブを識別する環境変数。 */
constexpr wchar_t kSystemAllocatorGenerationProbeEnvironment[] = L"ACS_INTERNAL_SYSTEM_ALLOCATOR_GENERATION_PROBE";

/** Arena のページ確保を任意の 1 回だけ停止し、Reset との順序を決定的に検証する backing。 */
class FBlockingArenaBacking final : public FAllocator {
public:
    /** 次の Alloc を ResumeBlockedAllocation が呼ばれるまで停止する。 */
    void BlockNextAllocation() noexcept
    {
        m_Entered.Store(0u, EMemoryOrder::Release);
        m_Resume.Store(0u, EMemoryOrder::Release);
        m_BlockNext.Store(1u, EMemoryOrder::Release);
    }

    /** 停止対象の Alloc が backing へ到達したかを返す。 */
    bool HasBlockedAllocationEntered() const noexcept
    {
        return m_Entered.Load(EMemoryOrder::Acquire) != 0u;
    }

    /** 停止中の Alloc を再開する。 */
    void ResumeBlockedAllocation() noexcept
    {
        m_Resume.Store(1u, EMemoryOrder::Release);
    }

    void* Alloc(usize Size, usize Alignment, FSourceLoc Location) noexcept override
    {
        if (m_BlockNext.Exchange(0u) != 0u) {
            m_Entered.Store(1u, EMemoryOrder::Release);
            while (m_Resume.Load(EMemoryOrder::Acquire) == 0u) {
                Yield();
            }
        }
        return m_Allocator.Alloc(Size, Alignment, Location);
    }

    void Free(void* Pointer) noexcept override
    {
        m_Allocator.Free(Pointer);
    }

    u64 BytesAllocated() const noexcept override
    {
        return m_Allocator.BytesAllocated();
    }

    u64 AllocationCount() const noexcept override
    {
        return m_Allocator.AllocationCount();
    }

private:
    FSystemAllocator m_Allocator;
    TAtomic<u32> m_BlockNext{0};
    TAtomic<u32> m_Entered{0};
    TAtomic<u32> m_Resume{0};
};

/** 現在のプロセスが構築世代プローブとして起動されたかを返す。 */
bool IsSystemAllocatorGenerationProbeProcess() noexcept
{
    wchar_t Value[2] = {};
    const DWORD Length = ::GetEnvironmentVariableW(kSystemAllocatorGenerationProbeEnvironment, Value, 2u);
    return Length == 1u && Value[0] == L'1';
}

/**
 * 同じアドレスへアロケータを再構築し、旧世代の確保が拒否されるかを検証する。
 *
 * @details 旧世代の確保を意図的に残すため、この関数は隔離子プロセス内だけで実行する。
 * @return 契約を満たせば 0、それ以外は原因別の終了コード。
 */
int RunSystemAllocatorGenerationProbe() noexcept
{
    alignas(FSystemAllocator) u8 AllocatorStorage[sizeof(FSystemAllocator)] = {};
    auto* FirstAllocator = ::new (static_cast<void*>(AllocatorStorage)) FSystemAllocator();
    void* const OldGenerationAllocation = FirstAllocator->Alloc(96u, 32u, FSourceLoc::Current());
    if (!OldGenerationAllocation) {
        FirstAllocator->~FSystemAllocator();
        return 81;
    }

    FirstAllocator->~FSystemAllocator();
    auto* SecondAllocator = ::new (static_cast<void*>(AllocatorStorage)) FSystemAllocator();
    SecondAllocator->Free(OldGenerationAllocation);

    const bool bStatisticsPreserved = SecondAllocator->BytesAllocated() == 0u &&
                                      SecondAllocator->AllocationCount() == 0u;
    SecondAllocator->~FSystemAllocator();
    return bStatisticsPreserved ? 0 : 82;
}

struct FDefaultAllocatorPublicationContext {
    FAllocator* First = nullptr;
    FAllocator* Second = nullptr;
    TAtomic<u32> Start{0u};
    TAtomic<u32> FailureCount{0u};
};

void ToggleAndReadDefaultAllocator(void* User) noexcept
{
    auto* const Context = static_cast<FDefaultAllocatorPublicationContext*>(User);
    while (Context->Start.Load(EMemoryOrder::Acquire) == 0u) {
        Yield();
    }

    for (u32 Iteration = 0u; Iteration < 50000u; ++Iteration) {
        SetDefaultAllocator((Iteration & 1u) == 0u ? Context->First : Context->Second);
        FAllocator* const Observed = &DefaultAllocator();
        if (Observed != Context->First && Observed != Context->Second) {
            Context->FailureCount.FetchAdd(1u);
        }
    }
}

} // namespace

ACS_TEST(Memory, SystemAllocatorRoundtrips)
{
    FSystemAllocator a;
    EXPECT_EQ(a.AllocationCount(), 0ull);
    EXPECT_EQ(a.Alloc(64, 0, FSourceLoc::Current()), nullptr);
    EXPECT_EQ(a.Alloc(64, 3, FSourceLoc::Current()), nullptr);
    EXPECT_EQ(a.Alloc(~usize(0), 16, FSourceLoc::Current()), nullptr);
    EXPECT_EQ(a.AllocationCount(), 0ull);
    void* p = a.Alloc(128, 16, FSourceLoc::Current());
    EXPECT_TRUE(p != nullptr);
    EXPECT_TRUE(((uptr)p & 15ull) == 0);
    EXPECT_EQ(a.AllocationCount(), 1ull);

    void* SecondAllocation = a.Alloc(64, 32, FSourceLoc::Current());
    EXPECT_TRUE(SecondAllocation != nullptr);
    EXPECT_EQ(a.AllocationCount(), 2ull);

    void* GrownAllocation = a.Realloc(p, 128, 256, 16, FSourceLoc::Current());
    EXPECT_TRUE(GrownAllocation != nullptr);
    if (GrownAllocation) p = GrownAllocation;
    EXPECT_EQ(a.AllocationCount(), 2ull);

    a.Free(p);
    EXPECT_EQ(a.AllocationCount(), 1ull);
    a.Free(SecondAllocation);
    EXPECT_EQ(a.BytesAllocated(), (u64)0);
    EXPECT_EQ(a.AllocationCount(), 0ull);

    // 基底インターフェイスからも件数を取得できること。
    FAllocator& AllocatorReference = a;
    EXPECT_EQ(AllocatorReference.AllocationCount(), 0ull);
}

ACS_TEST(Memory, DefaultAllocatorPublicationIsThreadSafe)
{
    constexpr u32 kWorkerCount = 8u;
    FAllocator* const Original = &DefaultAllocator();
    FSystemAllocator First;
    FSystemAllocator Second;
    FDefaultAllocatorPublicationContext Context{};
    Context.First = &First;
    Context.Second = &Second;
    SetDefaultAllocator(&First);

    FThread Workers[kWorkerCount];
    u32 SpawnedWorkerCount = 0u;
    for (u32 Index = 0u; Index < kWorkerCount; ++Index) {
        auto Worker = FThread::Spawn(&ToggleAndReadDefaultAllocator, &Context);
        EXPECT_TRUE(Worker.IsOk());
        if (Worker.IsOk()) {
            Workers[SpawnedWorkerCount++] = Move(Worker.Value());
        }
    }

    Context.Start.Store(1u, EMemoryOrder::Release);
    for (u32 Index = 0u; Index < SpawnedWorkerCount; ++Index) {
        Workers[Index].Join();
    }
    SetDefaultAllocator(Original);

    EXPECT_EQ(Context.FailureCount.Load(EMemoryOrder::Acquire), 0u);
    EXPECT_TRUE(&DefaultAllocator() == Original);
}

ACS_TEST(Memory, SystemAllocatorProcessStatisticsTrackLiveInstances)
{
    const FSystemAllocatorProcessStatistics Baseline = FSystemAllocator::CaptureProcessStatistics();

    {
        FSystemAllocator FirstAllocator;
        FSystemAllocator SecondAllocator;
        FSystemAllocatorProcessStatistics Statistics = FSystemAllocator::CaptureProcessStatistics();
        EXPECT_EQ(Statistics.live_allocator_count, Baseline.live_allocator_count + 2u);
        EXPECT_EQ(Statistics.outstanding_bytes, Baseline.outstanding_bytes);
        EXPECT_EQ(Statistics.outstanding_allocation_count, Baseline.outstanding_allocation_count);

        void* const FirstAllocation = FirstAllocator.Alloc(160u, 16u, FSourceLoc::Current());
        void* const SecondAllocation = SecondAllocator.Alloc(320u, 64u, FSourceLoc::Current());
        EXPECT_TRUE(FirstAllocation != nullptr);
        EXPECT_TRUE(SecondAllocation != nullptr);

        Statistics = FSystemAllocator::CaptureProcessStatistics();
        EXPECT_EQ(Statistics.outstanding_bytes,
                  Baseline.outstanding_bytes + FirstAllocator.BytesAllocated() + SecondAllocator.BytesAllocated());
        EXPECT_EQ(Statistics.outstanding_allocation_count, Baseline.outstanding_allocation_count + 2u);

        FirstAllocator.Free(FirstAllocation);
        SecondAllocator.Free(SecondAllocation);
        Statistics = FSystemAllocator::CaptureProcessStatistics();
        EXPECT_EQ(Statistics.outstanding_bytes, Baseline.outstanding_bytes);
        EXPECT_EQ(Statistics.outstanding_allocation_count, Baseline.outstanding_allocation_count);
    }

    const FSystemAllocatorProcessStatistics After = FSystemAllocator::CaptureProcessStatistics();
    EXPECT_EQ(After.live_allocator_count, Baseline.live_allocator_count);
    EXPECT_EQ(After.destroyed_with_live_allocations_count, Baseline.destroyed_with_live_allocations_count);
}

ACS_TEST(Memory, SystemAllocatorRejectsCrossInstanceFree)
{
    FSystemAllocator FirstAllocator;
    FSystemAllocator SecondAllocator;
    void* const Allocation = FirstAllocator.Alloc(256u, 64u, FSourceLoc::Current());
    EXPECT_TRUE(Allocation != nullptr);
    if (!Allocation) {
        return;
    }

    const u64 FirstBytes = FirstAllocator.BytesAllocated();
    SecondAllocator.Free(Allocation);
    EXPECT_EQ(FirstAllocator.BytesAllocated(), FirstBytes);
    EXPECT_EQ(FirstAllocator.AllocationCount(), 1ull);
    EXPECT_EQ(SecondAllocator.BytesAllocated(), 0ull);
    EXPECT_EQ(SecondAllocator.AllocationCount(), 0ull);

    FirstAllocator.Free(Allocation);
    EXPECT_EQ(FirstAllocator.BytesAllocated(), 0ull);
    EXPECT_EQ(FirstAllocator.AllocationCount(), 0ull);
}

ACS_TEST(Memory, SystemAllocatorRejectsCrossInstanceReallocation)
{
    FSystemAllocator FirstAllocator;
    FSystemAllocator SecondAllocator;
    auto* const Allocation = static_cast<u8*>(FirstAllocator.Alloc(64u, 16u, FSourceLoc::Current()));
    EXPECT_TRUE(Allocation != nullptr);
    if (!Allocation) {
        return;
    }
    Allocation[0] = 0x5Au;

    void* const Replacement = SecondAllocator.Realloc(Allocation, 64u, 128u, 16u, FSourceLoc::Current());
    EXPECT_TRUE(Replacement == nullptr);
    EXPECT_EQ(FirstAllocator.AllocationCount(), 1ull);
    EXPECT_EQ(SecondAllocator.AllocationCount(), 0ull);
    EXPECT_EQ(Allocation[0], static_cast<u8>(0x5Au));

    void* const RejectedRelease = SecondAllocator.Realloc(Allocation, 64u, 0u, 16u, FSourceLoc::Current());
    EXPECT_TRUE(RejectedRelease == Allocation);
    EXPECT_EQ(FirstAllocator.AllocationCount(), 1ull);

    FirstAllocator.Free(Allocation);
}

ACS_TEST(Memory, SystemAllocatorRejectsAllocationFromPreviousGenerationAtSameAddress)
{
    if (IsSystemAllocatorGenerationProbeProcess()) {
        ::ExitProcess(static_cast<UINT>(RunSystemAllocatorGenerationProbe()));
    }

    wchar_t ExecutablePath[MAX_PATH] = {};
    const DWORD ExecutablePathLength = ::GetModuleFileNameW(nullptr, ExecutablePath, MAX_PATH);
    EXPECT_TRUE(ExecutablePathLength > 0u && ExecutablePathLength < MAX_PATH);
    if (ExecutablePathLength == 0u || ExecutablePathLength >= MAX_PATH) {
        return;
    }

    wchar_t CommandLine[MAX_PATH + 3u] = {};
    usize CommandLength = 0;
    CommandLine[CommandLength++] = L'"';
    for (DWORD Index = 0; Index < ExecutablePathLength; ++Index) {
        CommandLine[CommandLength++] = ExecutablePath[Index];
    }
    CommandLine[CommandLength++] = L'"';
    CommandLine[CommandLength] = L'\0';

    wchar_t PreviousEnvironmentValue[64] = {};
    const DWORD PreviousEnvironmentLength = ::GetEnvironmentVariableW(
        kSystemAllocatorGenerationProbeEnvironment, PreviousEnvironmentValue,
        static_cast<DWORD>(sizeof(PreviousEnvironmentValue) / sizeof(PreviousEnvironmentValue[0])));
    EXPECT_TRUE(PreviousEnvironmentLength < sizeof(PreviousEnvironmentValue) / sizeof(PreviousEnvironmentValue[0]));
    if (PreviousEnvironmentLength >= sizeof(PreviousEnvironmentValue) / sizeof(PreviousEnvironmentValue[0])) {
        return;
    }

    const BOOL bEnvironmentSet = ::SetEnvironmentVariableW(kSystemAllocatorGenerationProbeEnvironment, L"1");
    EXPECT_TRUE(bEnvironmentSet != FALSE);
    if (!bEnvironmentSet) {
        return;
    }
    STARTUPINFOW StartupInformation = {};
    StartupInformation.cb = sizeof(StartupInformation);
    PROCESS_INFORMATION ProcessInformation = {};
    const BOOL bCreated = ::CreateProcessW(ExecutablePath, CommandLine, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                           nullptr, nullptr, &StartupInformation, &ProcessInformation);

    const wchar_t* const RestoredEnvironmentValue = PreviousEnvironmentLength == 0u ? nullptr
                                                                                    : PreviousEnvironmentValue;
    EXPECT_TRUE(::SetEnvironmentVariableW(kSystemAllocatorGenerationProbeEnvironment, RestoredEnvironmentValue) !=
                FALSE);
    EXPECT_TRUE(bCreated != FALSE);
    if (!bCreated) {
        return;
    }

    const DWORD WaitResult = ::WaitForSingleObject(ProcessInformation.hProcess, 30000u);
    EXPECT_EQ(WaitResult, WAIT_OBJECT_0);
    if (WaitResult != WAIT_OBJECT_0) {
        (void)::TerminateProcess(ProcessInformation.hProcess, 83u);
        (void)::WaitForSingleObject(ProcessInformation.hProcess, 10000u);
    }

    DWORD ExitCode = ~DWORD(0);
    EXPECT_TRUE(::GetExitCodeProcess(ProcessInformation.hProcess, &ExitCode) != FALSE);
    ::CloseHandle(ProcessInformation.hThread);
    ::CloseHandle(ProcessInformation.hProcess);
    EXPECT_EQ(ExitCode, 0u);
}

ACS_TEST(Memory, SystemAllocatorReallocationUsesRecordedRequestSize)
{
    FSystemAllocator Allocator;
    auto* Allocation = static_cast<u8*>(Allocator.Alloc(32u, 16u, FSourceLoc::Current()));
    EXPECT_TRUE(Allocation != nullptr);
    if (!Allocation) {
        return;
    }
    for (u32 Index = 0; Index < 32u; ++Index) {
        Allocation[Index] = static_cast<u8>(Index ^ 0xA5u);
    }

    // 過小な old_size ではなく、ヘッダに記録した 32 bytes をコピーする。
    auto* ResizedAllocation = static_cast<u8*>(Allocator.Realloc(Allocation, 1u, 64u, 64u, FSourceLoc::Current()));
    EXPECT_TRUE(ResizedAllocation != nullptr);
    EXPECT_EQ(Allocator.AllocationCount(), 1ull);
    if (ResizedAllocation) {
        Allocation = ResizedAllocation;
        for (u32 Index = 0; Index < 32u; ++Index) {
            EXPECT_EQ(Allocation[Index], static_cast<u8>(Index ^ 0xA5u));
        }
    } else {
        Allocator.Free(Allocation);
        return;
    }

    for (u32 Index = 0; Index < 64u; ++Index) {
        Allocation[Index] = static_cast<u8>(Index ^ 0x3Cu);
    }

    // 過大な old_size でも、記録済みの 64 bytes を超えて読み取らない。
    ResizedAllocation = static_cast<u8*>(Allocator.Realloc(Allocation, 4096u, 128u, 32u, FSourceLoc::Current()));
    EXPECT_TRUE(ResizedAllocation != nullptr);
    EXPECT_EQ(Allocator.AllocationCount(), 1ull);
    if (ResizedAllocation) {
        Allocation = ResizedAllocation;
        for (u32 Index = 0; Index < 64u; ++Index) {
            EXPECT_EQ(Allocation[Index], static_cast<u8>(Index ^ 0x3Cu));
        }
    }
    Allocator.Free(Allocation);
    EXPECT_EQ(Allocator.BytesAllocated(), 0ull);
    EXPECT_EQ(Allocator.AllocationCount(), 0ull);
}

ACS_TEST(Memory, SystemAllocatorReallocationFailurePreservesOriginalAllocation)
{
    FSystemAllocator Allocator;
    auto* Allocation = static_cast<u8*>(Allocator.Alloc(64u, 16u, FSourceLoc::Current()));
    EXPECT_TRUE(Allocation != nullptr);
    if (!Allocation) {
        return;
    }
    for (u32 Index = 0; Index < 64u; ++Index) {
        Allocation[Index] = static_cast<u8>(Index ^ 0x96u);
    }

    const u64 BytesBeforeFailure = Allocator.BytesAllocated();
    const u64 CountBeforeFailure = Allocator.AllocationCount();
    const u64 PeakBeforeFailure = Allocator.PeakBytes();
    void* const FailedReallocation = Allocator.Realloc(Allocation, 64u, 128u, 3u, FSourceLoc::Current());
    EXPECT_TRUE(FailedReallocation == nullptr);
    EXPECT_EQ(Allocator.BytesAllocated(), BytesBeforeFailure);
    EXPECT_EQ(Allocator.AllocationCount(), CountBeforeFailure);
    EXPECT_EQ(Allocator.PeakBytes(), PeakBeforeFailure);
    for (u32 Index = 0; Index < 64u; ++Index) {
        EXPECT_EQ(Allocation[Index], static_cast<u8>(Index ^ 0x96u));
    }

    void* const ReleasedAllocation = Allocator.Realloc(Allocation, 1u, 0u, 0u, FSourceLoc::Current());
    EXPECT_TRUE(ReleasedAllocation == nullptr);
    EXPECT_EQ(Allocator.BytesAllocated(), 0ull);
    EXPECT_EQ(Allocator.AllocationCount(), 0ull);
}

ACS_TEST(Memory, SystemAllocatorReportsDestructionWithLiveAllocations)
{
    const FSystemAllocatorProcessStatistics Baseline = FSystemAllocator::CaptureProcessStatistics();

    SECURITY_ATTRIBUTES PipeAttributes{};
    PipeAttributes.nLength = sizeof(PipeAttributes);
    PipeAttributes.bInheritHandle = TRUE;
    HANDLE PipeRead = nullptr;
    HANDLE PipeWrite = nullptr;
    EXPECT_TRUE(::CreatePipe(&PipeRead, &PipeWrite, &PipeAttributes, 0) != FALSE);
    if (!PipeRead || !PipeWrite) {
        if (PipeRead) ::CloseHandle(PipeRead);
        if (PipeWrite) ::CloseHandle(PipeWrite);
        return;
    }
    EXPECT_TRUE(::SetHandleInformation(PipeRead, HANDLE_FLAG_INHERIT, 0) != FALSE);

    wchar_t ExecutablePath[MAX_PATH] = {};
    const DWORD ExecutablePathLength = ::GetModuleFileNameW(nullptr, ExecutablePath, MAX_PATH);
    EXPECT_TRUE(ExecutablePathLength > 0 && ExecutablePathLength < MAX_PATH);
    if (ExecutablePathLength == 0 || ExecutablePathLength >= MAX_PATH) {
        ::CloseHandle(PipeWrite);
        ::CloseHandle(PipeRead);
        return;
    }

    wchar_t CommandLine[MAX_PATH + 64] = {};
    usize CommandLength = 0;
    CommandLine[CommandLength++] = L'"';
    for (DWORD i = 0; i < ExecutablePathLength; ++i) {
        CommandLine[CommandLength++] = ExecutablePath[i];
    }
    constexpr wchar_t kProbeArgument[] = L"\" --system-allocator-destruction-probe";
    for (usize i = 0; kProbeArgument[i] != L'\0'; ++i) {
        CommandLine[CommandLength++] = kProbeArgument[i];
    }
    CommandLine[CommandLength] = L'\0';

    HANDLE NullInput = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &PipeAttributes,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_TRUE(NullInput != INVALID_HANDLE_VALUE);
    if (NullInput == INVALID_HANDLE_VALUE) {
        ::CloseHandle(PipeWrite);
        ::CloseHandle(PipeRead);
        return;
    }

    STARTUPINFOW StartupInformation{};
    StartupInformation.cb = sizeof(StartupInformation);
    StartupInformation.dwFlags = STARTF_USESTDHANDLES;
    StartupInformation.hStdInput = NullInput;
    StartupInformation.hStdOutput = PipeWrite;
    StartupInformation.hStdError = PipeWrite;
    PROCESS_INFORMATION ProcessInformation{};
    const BOOL bCreated = ::CreateProcessW(ExecutablePath, CommandLine, nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                           nullptr, nullptr, &StartupInformation, &ProcessInformation);
    EXPECT_TRUE(bCreated != FALSE);

    ::CloseHandle(PipeWrite);
    PipeWrite = nullptr;
    ::CloseHandle(NullInput);

    char Diagnostic[1024] = {};
    DWORD DiagnosticBytes = 0;
    if (bCreated) {
        const DWORD WaitResult = ::WaitForSingleObject(ProcessInformation.hProcess, 10000u);
        EXPECT_EQ(WaitResult, WAIT_OBJECT_0);
        if (WaitResult != WAIT_OBJECT_0) {
            (void)::TerminateProcess(ProcessInformation.hProcess, 73u);
            (void)::WaitForSingleObject(ProcessInformation.hProcess, 10000u);
        }
        EXPECT_TRUE(::ReadFile(PipeRead, Diagnostic, sizeof(Diagnostic) - 1u, &DiagnosticBytes, nullptr) != FALSE);
    }
    Diagnostic[DiagnosticBytes] = '\0';
    ::CloseHandle(PipeRead);
    PipeRead = nullptr;

    DWORD ExitCode = ~DWORD(0);
    if (bCreated) {
        EXPECT_TRUE(::GetExitCodeProcess(ProcessInformation.hProcess, &ExitCode) != FALSE);
        ::CloseHandle(ProcessInformation.hThread);
        ::CloseHandle(ProcessInformation.hProcess);
    }
    EXPECT_EQ(ExitCode, 0u);

    auto Contains = [](const char* Text, const char* Pattern) noexcept {
        if (!Text || !Pattern || *Pattern == '\0') return false;
        for (const char* Start = Text; *Start != '\0'; ++Start) {
            const char* Left = Start;
            const char* Right = Pattern;
            while (*Left != '\0' && *Right != '\0' && *Left == *Right) {
                ++Left;
                ++Right;
            }
            if (*Right == '\0') return true;
        }
        return false;
    };
    EXPECT_TRUE(Contains(Diagnostic, "allocator=System"));
    EXPECT_TRUE(Contains(Diagnostic, "destroyed_with_live_allocations=true"));
    EXPECT_TRUE(Contains(Diagnostic, "outstanding_allocations=1"));

    // 未解放破棄は子プロセスだけで起こしたため、親の履歴は汚染されない。
    const FSystemAllocatorProcessStatistics After = FSystemAllocator::CaptureProcessStatistics();
    EXPECT_EQ(After.live_allocator_count, Baseline.live_allocator_count);
    EXPECT_EQ(After.destroyed_with_live_allocations_count, Baseline.destroyed_with_live_allocations_count);
    EXPECT_EQ(After.destroyed_outstanding_bytes, Baseline.destroyed_outstanding_bytes);
    EXPECT_EQ(After.destroyed_outstanding_allocation_count, Baseline.destroyed_outstanding_allocation_count);
}

ACS_TEST(Memory, LinearAllocatorBumps)
{
    FLinearAllocator la(4096);
    EXPECT_EQ(la.AllocationCount(), 0ull);
    void* a = la.Alloc(64, 8, FSourceLoc::Current());
    void* b = la.Alloc(64, 8, FSourceLoc::Current());
    EXPECT_TRUE(a != nullptr);
    EXPECT_TRUE(b != nullptr);
    EXPECT_TRUE(b > a);
    EXPECT_EQ(la.AllocationCount(), 2ull);
    la.Free(a);
    EXPECT_EQ(la.AllocationCount(), 2ull); // 個別 Free は一括寿命を短縮しない。
    la.Reset();
    EXPECT_EQ(la.AllocationCount(), 0ull);
    void* c = la.Alloc(64, 8, FSourceLoc::Current());
    EXPECT_EQ(c, a);
    EXPECT_EQ(la.AllocationCount(), 1ull);
}

ACS_TEST(Memory, LinearAllocatorRejectsOverflowAndInvalidAlignment)
{
    FLinearAllocator Allocator(4096u);
    void* const First = Allocator.Alloc(1u, 1u, FSourceLoc::Current());
    EXPECT_TRUE(First != nullptr);
    EXPECT_TRUE(Allocator.Alloc(~usize(0), 1u, FSourceLoc::Current()) == nullptr);
    EXPECT_TRUE(Allocator.Alloc(16u, 0u, FSourceLoc::Current()) == nullptr);
    EXPECT_TRUE(Allocator.Alloc(16u, 24u, FSourceLoc::Current()) == nullptr);
    EXPECT_EQ(Allocator.AllocationCount(), 1ull);
    EXPECT_EQ(Allocator.BytesAllocated(), 1ull);
}

ACS_TEST(Memory, PoolAllocatorReusesSlots)
{
    FPoolAllocator p(64, 16);
    EXPECT_EQ(p.AllocationCount(), 0ull);
    void* a = p.Alloc(64, 8, FSourceLoc::Current());
    void* b = p.Alloc(64, 8, FSourceLoc::Current());
    EXPECT_TRUE(a != nullptr);
    EXPECT_TRUE(b != nullptr);
    EXPECT_EQ(p.AllocationCount(), 2ull);
    p.Free(a);
    EXPECT_EQ(p.AllocationCount(), 1ull);
    void* c = p.Alloc(64, 8, FSourceLoc::Current());
    EXPECT_EQ(c, a); // LIFO reuse
    EXPECT_EQ(p.AllocationCount(), 2ull);
    p.Free(b);
    p.Free(c);
    EXPECT_EQ(p.AllocationCount(), 0ull);
}

ACS_TEST(Memory, PoolAllocatorRejectsInvalidAndDuplicateFree)
{
    FPoolAllocator Pool(64u, 4u);
    void* const First = Pool.Alloc(64u, 8u, FSourceLoc::Current());
    EXPECT_TRUE(First != nullptr);
    EXPECT_EQ(Pool.AllocationCount(), 1ull);

    Pool.Free(static_cast<u8*>(First) + 1u);
    EXPECT_EQ(Pool.AllocationCount(), 1ull);

    u8 ForeignStorage[64] = {};
    Pool.Free(ForeignStorage);
    EXPECT_EQ(Pool.AllocationCount(), 1ull);

    Pool.Free(First);
    Pool.Free(First);
    EXPECT_EQ(Pool.AllocationCount(), 0ull);

    void* Allocations[4] = {};
    for (usize Index = 0u; Index < 4u; ++Index) {
        Allocations[Index] = Pool.Alloc(64u, 8u, FSourceLoc::Current());
        EXPECT_TRUE(Allocations[Index] != nullptr);
        for (usize PreviousIndex = 0u; PreviousIndex < Index; ++PreviousIndex) {
            EXPECT_TRUE(Allocations[Index] != Allocations[PreviousIndex]);
        }
    }
    EXPECT_TRUE(Pool.Alloc(64u, 8u, FSourceLoc::Current()) == nullptr);

    for (void* Allocation : Allocations) {
        Pool.Free(Allocation);
    }
    EXPECT_EQ(Pool.AllocationCount(), 0ull);
}

ACS_TEST(Memory, PoolAllocatorConcurrentAllocAndFreeRemainBalanced)
{
    constexpr usize kThreadCount = 8u;
    constexpr usize kOperationCount = 20000u;
    FPoolAllocator Pool(64u, 32u);
    TAtomic<u32> FailureCount{0u};

    struct FContext {
        FPoolAllocator* Pool = nullptr;
        TAtomic<u32>* FailureCount = nullptr;
    } Contexts[kThreadCount] = {};
    FThread Threads[kThreadCount];

    for (usize Index = 0u; Index < kThreadCount; ++Index) {
        Contexts[Index].Pool = &Pool;
        Contexts[Index].FailureCount = &FailureCount;
        auto ThreadResult = FThread::Spawn(
            [](void* UserData) {
                auto* ThreadContext = static_cast<FContext*>(UserData);
                for (usize Operation = 0u; Operation < kOperationCount; ++Operation) {
                    void* Allocation = nullptr;
                    while (!Allocation) {
                        Allocation = ThreadContext->Pool->Alloc(64u, 8u, FSourceLoc::Current());
                        if (!Allocation) Yield();
                    }
                    MemSet(Allocation, static_cast<int>(Operation & 0xffu), 64u);
                    ThreadContext->Pool->Free(Allocation);
                }
            },
            &Contexts[Index]);
        if (ThreadResult.IsErr()) {
            FailureCount.FetchAdd(1u);
            continue;
        }
        Threads[Index] = Move(ThreadResult.Value());
    }

    for (FThread& Thread : Threads) {
        if (Thread.Joinable()) Thread.Join();
    }

    EXPECT_EQ(FailureCount.Load(EMemoryOrder::Acquire), 0u);
    EXPECT_EQ(Pool.AllocationCount(), 0ull);
}

/** バッチ確保・返却のロック回数と無効入力拒否を検証する。 */
ACS_TEST(Memory, PoolAllocatorBatchReducesLockingAndRejectsBadEntries)
{
    // 64 byte alignment の 8 ブロックプール。
    FPoolAllocator Pool(48u, 8u, 64u);
    // 枯渇後の未取得分も含む出力領域。
    void* Allocations[10] = {};
    // バッチ確保前のロック回数。
    const u64 LockCountBefore = Pool.LockAcquisitionCount();
    EXPECT_EQ(Pool.AllocBatch(Allocations, 10u), static_cast<usize>(8));
    EXPECT_EQ(Pool.LockAcquisitionCount() - LockCountBefore, 1ull);
    EXPECT_EQ(Pool.AllocationCount(), 8ull);

    for (usize Index = 0u; Index < 8u; ++Index) {
        EXPECT_TRUE(Allocations[Index] != nullptr);
        EXPECT_TRUE((reinterpret_cast<uptr>(Allocations[Index]) & 63u) == 0u);
        for (usize Previous = 0u; Previous < Index; ++Previous) {
            EXPECT_NE(Allocations[Index], Allocations[Previous]);
        }
    }
    EXPECT_TRUE(Allocations[8] == nullptr);
    EXPECT_TRUE(Allocations[9] == nullptr);

    // プール外ポインタ検証に使う byte。
    u8 Foreign = 0u;
    // 重複・途中・外部・nullptr と全有効ブロックを混ぜた返却列。
    void* Returns[] = {Allocations[0], Allocations[0], static_cast<u8*>(Allocations[1]) + 1u, &Foreign, nullptr, Allocations[1], Allocations[2], Allocations[3], Allocations[4], Allocations[5], Allocations[6], Allocations[7]};
    // バッチ返却前のロック回数。
    const u64 LockCountBeforeFree = Pool.LockAcquisitionCount();
    EXPECT_EQ(Pool.FreeBatch(Returns, sizeof(Returns) / sizeof(Returns[0])), static_cast<usize>(8));
    EXPECT_EQ(Pool.LockAcquisitionCount() - LockCountBeforeFree, 1ull);
    EXPECT_EQ(Pool.AllocationCount(), 0ull);
}

/** 型付きプールのコンパイル時 layout と重複破棄拒否を検証する。 */
ACS_TEST(Memory, TypedPoolPolicyFixesLayoutAtCompileTime)
{
    /** 64 byte alignment と破棄計数を持つテスト値。 */
    struct alignas(64) FTypedValue {
        /** 値と破棄回数出力先を設定して構築する。 */
        explicit FTypedValue(u32 InValue, u32& InDestructionCount) noexcept : DestructionCount(&InDestructionCount), Value(InValue)
        {
        }

        /** 破棄回数を増やす。 */
        ~FTypedValue() noexcept
        {
            ++*DestructionCount;
        }

        /** 破棄回数の出力先。 */
        u32* DestructionCount = nullptr;

        /** テスト用の保持値。 */
        u32 Value = 0u;
    };

    // 2 要素固定の型付きプール。
    using FTypedPool = TTypedPoolAllocator<FTypedValue, 2u>;
    static_assert(FTypedPool::BlockCount() == 2u);
    static_assert(FTypedPool::BlockSize() >= sizeof(FTypedValue));
    static_assert((FTypedPool::BlockSize() & 63u) == 0u);

    // 構築値の破棄回数。
    u32 DestructionCount = 0u;
    // layout を型から決定するプール。
    FTypedPool Pool;
    // 1 個目の構築値。
    FTypedValue* const First = Pool.Create(17u, DestructionCount);
    // 2 個目の構築値。
    FTypedValue* const Second = Pool.Create(29u, DestructionCount);
    EXPECT_TRUE(First != nullptr);
    EXPECT_TRUE(Second != nullptr);
    EXPECT_TRUE(Pool.Create(41u, DestructionCount) == nullptr);
    EXPECT_TRUE((reinterpret_cast<uptr>(First) & 63u) == 0u);
    EXPECT_EQ(First->Value, 17u);
    EXPECT_EQ(Second->Value, 29u);

    EXPECT_TRUE(Pool.Destroy(First));
    EXPECT_FALSE(Pool.Destroy(First));
    EXPECT_TRUE(Pool.Destroy(Second));
    EXPECT_EQ(DestructionCount, 2u);
    EXPECT_EQ(Pool.AllocationCount(), 0ull);
}

/** 複数スレッドのバッチ確保・返却が所有数を均衡させることを検証する。 */
ACS_TEST(Memory, PoolAllocatorBatchConcurrencyRemainsBalanced)
{
    // 並列 worker 数。
    constexpr usize kThreadCount = 4u;
    // worker ごとの反復回数。
    constexpr usize kIterations = 4000u;
    // 1 回に取得するブロック数。
    constexpr usize kBatchSize = 4u;
    // 全 worker が共有するプール。
    FPoolAllocator Pool(64u, 64u);
    // spawn または返却失敗の件数。
    TAtomic<u32> FailureCount{0u};

    /** worker が参照する共有状態。 */
    struct FContext {
        /** 共有プール。 */
        FPoolAllocator* Pool = nullptr;

        /** 失敗件数の共有出力先。 */
        TAtomic<u32>* FailureCount = nullptr;
    };
    // worker ごとの文脈。
    FContext Contexts[kThreadCount] = {};
    // join 対象の worker スレッド。
    FThread Threads[kThreadCount];

    // バッチ確保・書き込み・返却を反復する worker。
    const auto Worker = [](void* UserData) {
        // 現在 worker の共有状態。
        auto* const Context = static_cast<FContext*>(UserData);
        for (usize Iteration = 0u; Iteration < kIterations; ++Iteration) {
            // 現在反復で取得するブロック群。
            void* Batch[kBatchSize] = {};
            // 現在取得できたブロック数。
            usize Acquired = 0u;
            while (Acquired != kBatchSize) {
                Acquired = Context->Pool->AllocBatch(Batch, kBatchSize);
                if (Acquired != kBatchSize) {
                    Context->Pool->FreeBatch(Batch, Acquired);
                    Yield();
                }
            }
            for (void* Pointer : Batch) {
                MemSet(Pointer, 0x5A, 64u);
            }
            if (Context->Pool->FreeBatch(Batch, kBatchSize) != kBatchSize) {
                Context->FailureCount->FetchAdd(1u);
            }
        }
    };

    for (usize Index = 0u; Index < kThreadCount; ++Index) {
        Contexts[Index] = {&Pool, &FailureCount};
        // 現在 worker の spawn 結果。
        auto ThreadResult = FThread::Spawn(Worker, &Contexts[Index]);
        if (ThreadResult.IsErr()) {
            FailureCount.FetchAdd(1u);
            continue;
        }
        Threads[Index] = Move(ThreadResult.Value());
    }

    for (FThread& Thread : Threads) {
        if (Thread.Joinable()) Thread.Join();
    }
    EXPECT_EQ(FailureCount.Load(EMemoryOrder::Acquire), 0u);
    EXPECT_EQ(Pool.AllocationCount(), 0ull);
}

ACS_TEST(Memory, ArenaGrows)
{
    FArenaAllocator ar(1024);
    EXPECT_EQ(ar.AllocationCount(), 0ull);
    void* a = ar.Alloc(800, 16, FSourceLoc::Current());
    void* b = ar.Alloc(800, 16, FSourceLoc::Current()); // forces new page
    EXPECT_TRUE(a != nullptr);
    EXPECT_TRUE(b != nullptr);
    EXPECT_EQ(ar.AllocationCount(), 2ull);
    ar.Free(a);
    EXPECT_EQ(ar.AllocationCount(), 2ull); // 個別 Free は一括寿命を短縮しない。
    ar.Reset(/*release_pages*/ false);
    EXPECT_EQ(ar.AllocationCount(), 0ull);
    void* c = ar.Alloc(64, 16, FSourceLoc::Current());
    EXPECT_TRUE(c != nullptr);
    EXPECT_EQ(ar.AllocationCount(), 1ull);
}

ACS_TEST(Memory, ArenaResetReusesEveryRetainedPage)
{
    FSystemAllocator Backing;
    FArenaAllocator Arena(256u, &Backing);

    for (usize Cycle = 0u; Cycle < 64u; ++Cycle) {
        EXPECT_TRUE(Arena.Alloc(200u, 16u, FSourceLoc::Current()) != nullptr);
        EXPECT_TRUE(Arena.Alloc(200u, 16u, FSourceLoc::Current()) != nullptr);
        EXPECT_TRUE(Arena.Alloc(200u, 16u, FSourceLoc::Current()) != nullptr);
        EXPECT_EQ(Backing.AllocationCount(), 3ull);
        Arena.Reset(false);
    }

    Arena.Reset(true);
    EXPECT_EQ(Backing.AllocationCount(), 0ull);
}

ACS_TEST(Memory, ArenaLargeAlignmentIsBounded)
{
    FArenaAllocator Arena(64);

    void* const AlignedAllocation = Arena.Alloc(16, 4096, FSourceLoc::Current());
    EXPECT_TRUE(AlignedAllocation != nullptr);
    EXPECT_TRUE((reinterpret_cast<uptr>(AlignedAllocation) & 4095u) == 0u);
    EXPECT_EQ(Arena.AllocationCount(), 1ull);

    EXPECT_TRUE(Arena.Alloc(16, 24, FSourceLoc::Current()) == nullptr);
    EXPECT_TRUE(Arena.Alloc(16, 128u * 1024u, FSourceLoc::Current()) == nullptr);
    EXPECT_TRUE(Arena.Alloc(~usize(0), 16, FSourceLoc::Current()) == nullptr);
    EXPECT_EQ(Arena.AllocationCount(), 1ull);
}

ACS_TEST(Memory, ArenaResetWaitsForActiveAllocationAndRejectsNewAllocation)
{
    constexpr usize kPageSize = 1u * 1024u * 1024u;
    FBlockingArenaBacking Backing;
    FArenaAllocator Arena(kPageSize, &Backing);
    EXPECT_TRUE(Arena.Alloc(64u, 16u, FSourceLoc::Current()) != nullptr);

    struct FContext {
        FArenaAllocator* allocator = nullptr;
        void* allocation = nullptr;
        TAtomic<u32> reset_started{0};
        TAtomic<u32> reset_finished{0};
    } TestContext;
    TestContext.allocator = &Arena;

    Backing.BlockNextAllocation();
    auto AllocationThreadResult = FThread::Spawn(
        [](void* UserData) {
            auto* ThreadContext = static_cast<FContext*>(UserData);
            ThreadContext->allocation = ThreadContext->allocator->Alloc(2u * kPageSize, 64u, FSourceLoc::Current());
        },
        &TestContext);
    EXPECT_TRUE(AllocationThreadResult.IsOk());
    if (AllocationThreadResult.IsErr()) {
        return;
    }

    while (!Backing.HasBlockedAllocationEntered()) {
        Yield();
    }

    auto ResetThreadResult = FThread::Spawn(
        [](void* UserData) {
            auto* ThreadContext = static_cast<FContext*>(UserData);
            ThreadContext->reset_started.Store(1u, EMemoryOrder::Release);
            ThreadContext->allocator->Reset(true);
            ThreadContext->reset_finished.Store(1u, EMemoryOrder::Release);
        },
        &TestContext);
    EXPECT_TRUE(ResetThreadResult.IsOk());
    if (ResetThreadResult.IsErr()) {
        Backing.ResumeBlockedAllocation();
        AllocationThreadResult.Value().Join();
        Arena.Reset(true);
        return;
    }

    while (TestContext.reset_started.Load(EMemoryOrder::Acquire) == 0u) {
        Yield();
    }

    bool bResetRejectionObserved = false;
    for (u32 Attempt = 0u; Attempt < 10000u; ++Attempt) {
        if (!Arena.Alloc(1u, 1u, FSourceLoc::Current())) {
            bResetRejectionObserved = true;
            break;
        }
        Yield();
    }
    EXPECT_TRUE(bResetRejectionObserved);
    EXPECT_EQ(TestContext.reset_finished.Load(EMemoryOrder::Acquire), 0u);

    Backing.ResumeBlockedAllocation();
    AllocationThreadResult.Value().Join();
    ResetThreadResult.Value().Join();

    EXPECT_TRUE(TestContext.allocation != nullptr);
    EXPECT_EQ(TestContext.reset_finished.Load(EMemoryOrder::Acquire), 1u);
    EXPECT_EQ(Arena.BytesAllocated(), 0ull);
    EXPECT_EQ(Arena.AllocationCount(), 0ull);
    EXPECT_EQ(Backing.AllocationCount(), 0ull);

    EXPECT_TRUE(Arena.Alloc(64u, 16u, FSourceLoc::Current()) != nullptr);
    Arena.Reset(true);
    EXPECT_EQ(Backing.AllocationCount(), 0ull);
}

struct FProbe {
    static int destroyed;
    int v;
    FProbe(int x = 0) noexcept : v(x)
    {
    }
    ~FProbe() noexcept
    {
        ++destroyed;
    }
};
int FProbe::destroyed = 0;

ACS_TEST(Memory, UniquePtrDestroys)
{
    FProbe::destroyed = 0;
    {
        auto p = MakeUnique<FProbe>(99);
        EXPECT_EQ(p->v, 99);
    }
    EXPECT_EQ(FProbe::destroyed, 1);
}

ACS_TEST(Memory, RcSharesAndReleases)
{
    FProbe::destroyed = 0;
    {
        TSharedPtr<FProbe> a = MakeShared<FProbe>(7);
        TSharedPtr<FProbe> b = a;
        EXPECT_EQ(a.UseCount(), 2u);
        EXPECT_EQ(a->v, 7);
    }
    EXPECT_EQ(FProbe::destroyed, 1);
}
