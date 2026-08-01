// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — CSystemAllocator 実装
// -----------------------------------------------------------------------------
// HeapAlloc は最大で MEMORY_ALLOCATION_ALIGNMENT (16B on x64) しか保証しない。
// 任意アライメントを得るため、要求サイズ + アライメント余裕 + ヘッダ分を
// 確保し、ポインタを切り上げて返す。Free 時は元ポインタをヘッダから取得。
// =============================================================================
#include "memory/SystemAllocator.h"
#include "memory/Memory.h"
#include "foundation/Platform.h"

#include <new>

namespace acs {

namespace {

/** 全 CSystemAllocator の侵入レジストリを守る。POD なので静的初期化・破棄順序へ依存しない。 */
SRWLOCK g_system_allocator_registry_lock = SRWLOCK_INIT;

/** 生存中 CSystemAllocator の侵入リスト先頭。 */
CSystemAllocator* g_system_allocator_registry_head = nullptr;

/** 生存中 CSystemAllocator の数。レジストリロック下で更新する。 */
u64 g_live_system_allocator_count = 0;

/** 最後に割り当てた CSystemAllocator 構築世代。レジストリロック下で単調増加させる。 */
u64 g_last_system_allocator_generation = 0;

/** 未解放確保を残したまま破棄された CSystemAllocator の累積数。 */
u64 g_destroyed_with_live_allocations_count = 0;

/** 破棄時に残っていた実ヒープ確保量の累積。 */
u64 g_destroyed_outstanding_bytes = 0;

/** 破棄時に残っていた未解放確保件数の累積。 */
u64 g_destroyed_outstanding_allocation_count = 0;

/** u64 の累積を飽和加算し、診断カウンタのラップを防ぐ。 */
u64 SaturatingAdd(u64 Left, u64 Right) noexcept
{
    const u64 Maximum = ~u64(0);
    return Right > Maximum - Left ? Maximum : Left + Right;
}

/** 動的確保も CRT 文字列処理も使わない固定長診断バッファ。 */
class FSystemAllocatorDiagnosticBuffer final {
public:
    /** 文字列リテラル等を末尾へ追加する。容量超過時は安全に切り詰める。 */
    void AppendText(const char* Text) noexcept
    {
        if (!Text) return;
        while (*Text != '\0' && m_Length + 1u < sizeof(m_Data)) {
            m_Data[m_Length++] = *Text++;
        }
        m_Data[m_Length] = '\0';
    }

    /** 符号なし整数を 10 進表記で末尾へ追加する。 */
    void AppendUnsigned(u64 Value) noexcept
    {
        char Reversed[32];
        usize Count = 0;
        do {
            Reversed[Count++] = static_cast<char>('0' + (Value % 10u));
            Value /= 10u;
        } while (Value != 0 && Count < sizeof(Reversed));
        while (Count > 0) {
            char Digit[2] = {Reversed[--Count], '\0'};
            AppendText(Digit);
        }
    }

    /** ポインタ値を 0x 付き 16 進表記で末尾へ追加する。 */
    void AppendPointer(const void* Pointer) noexcept
    {
        static constexpr char kHexDigits[] = "0123456789abcdef";
        AppendText("0x");
        uptr Value = reinterpret_cast<uptr>(Pointer);
        char Reversed[sizeof(uptr) * 2u];
        usize Count = 0;
        do {
            Reversed[Count++] = kHexDigits[Value & 0xFu];
            Value >>= 4u;
        } while (Value != 0 && Count < sizeof(Reversed));
        while (Count > 0) {
            char Digit[2] = {Reversed[--Count], '\0'};
            AppendText(Digit);
        }
    }

    /** 現在の文字列先頭を返す。 */
    const char* Data() const noexcept
    {
        return m_Data;
    }

    /** 終端 null を除く現在のバイト数を返す。 */
    u32 Length() const noexcept
    {
        return static_cast<u32>(m_Length);
    }

private:
    /** 診断 1 行を保持する固定長領域。 */
    char m_Data[512] = {};

    /** 終端 null を除く現在長。 */
    usize m_Length = 0;
};

/** 未解放を伴うアロケータ破棄を Logger 非依存で機械可読出力する。 */
void EmitDestroyedWithLiveAllocations(const CSystemAllocator* Allocator, u64 AllocatorGeneration, u64 OutstandingBytes,
                                      u64 OutstandingAllocationCount, u64 DestroyedWithLiveAllocationsCount,
                                      u64 DestroyedOutstandingBytes, u64 DestroyedOutstandingAllocationCount) noexcept
{
    FSystemAllocatorDiagnosticBuffer Message;
    Message.AppendText("[acs][memory] allocator=System allocator_address=");
    Message.AppendPointer(Allocator);
    Message.AppendText(" allocator_generation=");
    Message.AppendUnsigned(AllocatorGeneration);
    Message.AppendText(" destroyed_with_live_allocations=true outstanding_allocations=");
    Message.AppendUnsigned(OutstandingAllocationCount);
    Message.AppendText(" outstanding_bytes=");
    Message.AppendUnsigned(OutstandingBytes);
    Message.AppendText(" destroyed_with_live_allocations_total=");
    Message.AppendUnsigned(DestroyedWithLiveAllocationsCount);
    Message.AppendText(" destroyed_outstanding_allocations_total=");
    Message.AppendUnsigned(DestroyedOutstandingAllocationCount);
    Message.AppendText(" destroyed_outstanding_bytes_total=");
    Message.AppendUnsigned(DestroyedOutstandingBytes);
    Message.AppendText("\r\n");

    ::OutputDebugStringA(Message.Data());
    const HANDLE StandardError = ::GetStdHandle(STD_ERROR_HANDLE);
    if (StandardError && StandardError != INVALID_HANDLE_VALUE) {
        DWORD Written = 0;
        (void)::WriteFile(StandardError, Message.Data(), Message.Length(), &Written, nullptr);
    }
}

/** CSystemAllocator が払い出したブロックを識別する固定値。 */
constexpr u64 kAllocationHeaderMagic = 0x414353535953544Dull;

/** ユーザポインタ直前に置く所有権・回収情報。 */
struct FAllocationHeader {
    u64 magic = 0;
    CSystemAllocator* owner = nullptr;
    u64 owner_generation = 0;
    void* allocation_base = nullptr;
    usize allocation_size = 0;
    usize requested_size = 0;
};

/** 解放処理の内部結果。 */
enum class EAlignedFreeResult : u8 {
    NullPointer,
    Success,
    InvalidMetadata,
    OwnerMismatch,
    OwnerGenerationMismatch,
    StatisticsUnderflow,
    HeapFreeFailed,
};

/** 解放試行の診断値。失敗時もヘッダを再読せず機械可読ログへ渡す。 */
struct FAlignedFreeDetails {
    usize freed_size = 0;
    DWORD operating_system_error = ERROR_SUCCESS;
    const CSystemAllocator* actual_owner = nullptr;
    u64 actual_owner_generation = 0;
};

/** 解放拒否や HeapFree 失敗を Logger 非依存で機械可読出力する。 */
void EmitFreeFailure(const CSystemAllocator* Allocator, u64 AllocatorGeneration, const void* Pointer,
                     const FAlignedFreeDetails& Details, EAlignedFreeResult Result) noexcept
{
    const char* Reason = "unknown";
    if (Result == EAlignedFreeResult::InvalidMetadata) {
        Reason = "invalid_metadata";
    } else if (Result == EAlignedFreeResult::OwnerMismatch) {
        Reason = "owner_mismatch";
    } else if (Result == EAlignedFreeResult::OwnerGenerationMismatch) {
        Reason = "owner_generation_mismatch";
    } else if (Result == EAlignedFreeResult::StatisticsUnderflow) {
        Reason = "statistics_underflow";
    } else if (Result == EAlignedFreeResult::HeapFreeFailed) {
        Reason = "heap_free_failed";
    }

    FSystemAllocatorDiagnosticBuffer Message;
    Message.AppendText("[acs][memory] allocator=System free_failed=true reason=");
    Message.AppendText(Reason);
    Message.AppendText(" allocator_address=");
    Message.AppendPointer(Allocator);
    Message.AppendText(" allocator_generation=");
    Message.AppendUnsigned(AllocatorGeneration);
    Message.AppendText(" actual_owner_address=");
    Message.AppendPointer(Details.actual_owner);
    Message.AppendText(" actual_owner_generation=");
    Message.AppendUnsigned(Details.actual_owner_generation);
    Message.AppendText(" pointer=");
    Message.AppendPointer(Pointer);
    Message.AppendText(" operating_system_error=");
    Message.AppendUnsigned(Details.operating_system_error);
    Message.AppendText("\r\n");

    ::OutputDebugStringA(Message.Data());
    const HANDLE StandardError = ::GetStdHandle(STD_ERROR_HANDLE);
    if (StandardError && StandardError != INVALID_HANDLE_VALUE) {
        DWORD Written = 0;
        (void)::WriteFile(StandardError, Message.Data(), Message.Length(), &Written, nullptr);
    }
}

/** 再確保の所有権拒否や旧領域解放失敗を Logger 非依存で機械可読出力する。 */
void EmitReallocationFailure(const CSystemAllocator* Allocator, u64 AllocatorGeneration, const void* Pointer,
                             const FAlignedFreeDetails& Details, EAlignedFreeResult Result) noexcept
{
    const char* Reason = "invalid_metadata";
    if (Result == EAlignedFreeResult::OwnerMismatch) {
        Reason = "owner_mismatch";
    } else if (Result == EAlignedFreeResult::OwnerGenerationMismatch) {
        Reason = "owner_generation_mismatch";
    } else if (Result == EAlignedFreeResult::StatisticsUnderflow) {
        Reason = "statistics_underflow";
    } else if (Result == EAlignedFreeResult::HeapFreeFailed) {
        Reason = "heap_free_failed";
    }

    FSystemAllocatorDiagnosticBuffer Message;
    Message.AppendText("[acs][memory] allocator=System realloc_failed=true reason=");
    Message.AppendText(Reason);
    Message.AppendText(" allocator_address=");
    Message.AppendPointer(Allocator);
    Message.AppendText(" allocator_generation=");
    Message.AppendUnsigned(AllocatorGeneration);
    Message.AppendText(" actual_owner_address=");
    Message.AppendPointer(Details.actual_owner);
    Message.AppendText(" actual_owner_generation=");
    Message.AppendUnsigned(Details.actual_owner_generation);
    Message.AppendText(" pointer=");
    Message.AppendPointer(Pointer);
    Message.AppendText(" operating_system_error=");
    Message.AppendUnsigned(Details.operating_system_error);
    Message.AppendText("\r\n");

    ::OutputDebugStringA(Message.Data());
    const HANDLE StandardError = ::GetStdHandle(STD_ERROR_HANDLE);
    if (StandardError && StandardError != INVALID_HANDLE_VALUE) {
        DWORD Written = 0;
        (void)::WriteFile(StandardError, Message.Data(), Message.Length(), &Written, nullptr);
    }
}

/**
 * 任意アライメントで確保する (ヘッダ退避方式)。
 *
 * @details
 * size + alignment + ヘッダ分をまとめて HeapAlloc し、ユーザポインタを境界へ切り上げる。
 * ユーザポインタの手前へ配置 new でヘッダを構築し、所有者・構築世代・元ポインタを保持する。
 * 加算オーバーフロー (過小確保→OOB) を検出したら nullptr。
 * @param Size 要求バイト数。
 * @param Alignment 要求アライメント。
 * @param Owner 所有するアロケータ。
 * @param OwnerGeneration 所有するアロケータの構築世代。
 * @param ActualSize 実際にヒープから取った総バイト数を返す (統計用、失敗時 0)。
 * @return アライン済みユーザポインタ (失敗時 nullptr)。
 */
void* AlignedAlloc(usize Size, usize Alignment, CSystemAllocator* Owner, u64 OwnerGeneration,
                   usize& ActualSize) noexcept
{
    if (Alignment == 0 || (Alignment & (Alignment - 1u)) != 0) {
        ActualSize = 0;
        return nullptr;
    }
    const usize MinimumAlignment = alignof(FAllocationHeader) > sizeof(void*) ? alignof(FAllocationHeader)
                                                                             : sizeof(void*);
    if (Alignment < MinimumAlignment) {
        Alignment = MinimumAlignment;
    }

    const usize MaximumSize = ~usize(0);
    const usize AlignmentSlack = Alignment - 1u;
    if (AlignmentSlack > MaximumSize - sizeof(FAllocationHeader) ||
        Size > MaximumSize - sizeof(FAllocationHeader) - AlignmentSlack) {
        ActualSize = 0;
        return nullptr;
    }
    const usize RawSize = Size + sizeof(FAllocationHeader) + AlignmentSlack;
    void* Raw = ::HeapAlloc(::GetProcessHeap(), 0, RawSize);
    if (!Raw) {
        ActualSize = 0;
        return nullptr;
    }

    const uptr RawAddress = reinterpret_cast<uptr>(Raw);
    if (RawAddress > MaximumSize - sizeof(FAllocationHeader)) {
        (void)::HeapFree(::GetProcessHeap(), 0, Raw);
        ActualSize = 0;
        return nullptr;
    }
    const uptr AlignmentBase = RawAddress + sizeof(FAllocationHeader);
    if (AlignmentBase > MaximumSize - AlignmentSlack) {
        (void)::HeapFree(::GetProcessHeap(), 0, Raw);
        ActualSize = 0;
        return nullptr;
    }

    const uptr AlignedAddress = (AlignmentBase + AlignmentSlack) & ~static_cast<uptr>(AlignmentSlack);
    const uptr HeaderAddress = AlignedAddress - sizeof(FAllocationHeader);
    auto* Header = ::new (reinterpret_cast<void*>(HeaderAddress)) FAllocationHeader{};
    Header->magic = kAllocationHeaderMagic;
    Header->owner = Owner;
    Header->owner_generation = OwnerGeneration;
    Header->allocation_base = Raw;
    Header->allocation_size = RawSize;
    Header->requested_size = Size;
    ActualSize = RawSize;
    return reinterpret_cast<void*>(AlignedAddress);
}

/** 正規ポインタ直前のヘッダと、確保範囲内に収まる要求サイズを検証する。 */
const FAllocationHeader* ValidateAllocationHeader(const void* Pointer) noexcept
{
    if (!Pointer) {
        return nullptr;
    }

    const uptr PointerAddress = reinterpret_cast<uptr>(Pointer);
    if (PointerAddress < sizeof(FAllocationHeader)) {
        return nullptr;
    }
    const uptr HeaderAddress = PointerAddress - sizeof(FAllocationHeader);
    if ((HeaderAddress & (alignof(FAllocationHeader) - 1u)) != 0) {
        return nullptr;
    }

    const auto* Header = reinterpret_cast<const FAllocationHeader*>(HeaderAddress);
    if (Header->magic != kAllocationHeaderMagic || !Header->owner || !Header->allocation_base ||
        Header->owner_generation == 0 || Header->allocation_size < sizeof(FAllocationHeader) ||
        Header->requested_size == 0) {
        return nullptr;
    }

    const uptr AllocationAddress = reinterpret_cast<uptr>(Header->allocation_base);
    if (HeaderAddress < AllocationAddress) {
        return nullptr;
    }
    const usize HeaderOffset = static_cast<usize>(HeaderAddress - AllocationAddress);
    if (HeaderOffset > Header->allocation_size || sizeof(FAllocationHeader) > Header->allocation_size - HeaderOffset) {
        return nullptr;
    }
    const usize UserOffset = HeaderOffset + sizeof(FAllocationHeader);
    if (Header->requested_size > Header->allocation_size - UserOffset) {
        return nullptr;
    }
    return Header;
}

/**
 * AlignedAlloc で確保した領域を所有権と統計不変条件の検証後に解放する。
 *
 * @param Allocator 解放を要求したアロケータ。
 * @param AllocatorGeneration 解放を要求したアロケータの構築世代。
 * @param Pointer AlignedAlloc が返した正確な生存ポインタ (nullptr 可)。
 * @param Details 解放サイズと失敗診断値の出力先。
 * @return 解放結果。
 */
EAlignedFreeResult AlignedFree(CSystemAllocator* Allocator, u64 AllocatorGeneration, void* Pointer,
                               FAlignedFreeDetails& Details) noexcept
{
    Details = FAlignedFreeDetails{};
    if (!Pointer) {
        return EAlignedFreeResult::NullPointer;
    }

    const FAllocationHeader* const Header = ValidateAllocationHeader(Pointer);
    if (!Header) {
        return EAlignedFreeResult::InvalidMetadata;
    }
    Details.actual_owner = Header->owner;
    Details.actual_owner_generation = Header->owner_generation;
    if (Header->owner != Allocator) {
        return EAlignedFreeResult::OwnerMismatch;
    }
    if (Header->owner_generation != AllocatorGeneration) {
        return EAlignedFreeResult::OwnerGenerationMismatch;
    }

    void* const AllocationBase = Header->allocation_base;
    const usize AllocationSize = Header->allocation_size;
    if (Allocator->BytesAllocated() < AllocationSize || Allocator->AllocationCount() == 0) {
        return EAlignedFreeResult::StatisticsUnderflow;
    }
    if (!::HeapFree(::GetProcessHeap(), 0, AllocationBase)) {
        Details.operating_system_error = ::GetLastError();
        return EAlignedFreeResult::HeapFreeFailed;
    }
    Details.freed_size = AllocationSize;
    return EAlignedFreeResult::Success;
}

} // namespace

CSystemAllocator::CSystemAllocator() noexcept
{
    ::AcquireSRWLockExclusive(&g_system_allocator_registry_lock);
    g_last_system_allocator_generation = SaturatingAdd(g_last_system_allocator_generation, 1u);
    m_Generation = g_last_system_allocator_generation;
    m_RegistryNext = g_system_allocator_registry_head;
    g_system_allocator_registry_head = this;
    m_RegistryRegistered = true;
    g_live_system_allocator_count = SaturatingAdd(g_live_system_allocator_count, 1u);
    ::ReleaseSRWLockExclusive(&g_system_allocator_registry_lock);
}

CSystemAllocator::~CSystemAllocator() noexcept
{
    const u64 OutstandingBytes = m_Bytes.Load(EMemoryOrder::Acquire);
    const u64 OutstandingAllocationCount = m_AllocationCount.Load(EMemoryOrder::Acquire);
    const bool bHasLiveAllocations = OutstandingBytes != 0 || OutstandingAllocationCount != 0;

    u64 DestroyedWithLiveAllocationsCount = 0;
    u64 DestroyedOutstandingBytes = 0;
    u64 DestroyedOutstandingAllocationCount = 0;

    ::AcquireSRWLockExclusive(&g_system_allocator_registry_lock);
    if (m_RegistryRegistered) {
        CSystemAllocator** Link = &g_system_allocator_registry_head;
        while (*Link && *Link != this)
            Link = &((*Link)->m_RegistryNext);
        if (*Link == this) {
            *Link = m_RegistryNext;
            if (g_live_system_allocator_count > 0) --g_live_system_allocator_count;
        }
        m_RegistryNext = nullptr;
        m_RegistryRegistered = false;
    }
    if (bHasLiveAllocations) {
        g_destroyed_with_live_allocations_count = SaturatingAdd(g_destroyed_with_live_allocations_count, 1u);
        g_destroyed_outstanding_bytes = SaturatingAdd(g_destroyed_outstanding_bytes, OutstandingBytes);
        g_destroyed_outstanding_allocation_count = SaturatingAdd(g_destroyed_outstanding_allocation_count,
                                                                 OutstandingAllocationCount);
    }
    DestroyedWithLiveAllocationsCount = g_destroyed_with_live_allocations_count;
    DestroyedOutstandingBytes = g_destroyed_outstanding_bytes;
    DestroyedOutstandingAllocationCount = g_destroyed_outstanding_allocation_count;
    ::ReleaseSRWLockExclusive(&g_system_allocator_registry_lock);

    // レジストリロック外で OS へ直接出力し、再入や Logger の静的破棄順序へ依存しない。
    if (bHasLiveAllocations) {
        EmitDestroyedWithLiveAllocations(this, m_Generation, OutstandingBytes, OutstandingAllocationCount,
                                         DestroyedWithLiveAllocationsCount, DestroyedOutstandingBytes,
                                         DestroyedOutstandingAllocationCount);
    }
}

FSystemAllocatorProcessStatistics CSystemAllocator::CaptureProcessStatistics() noexcept
{
    FSystemAllocatorProcessStatistics Statistics;
    ::AcquireSRWLockShared(&g_system_allocator_registry_lock);
    Statistics.live_allocator_count = g_live_system_allocator_count;
    Statistics.destroyed_with_live_allocations_count = g_destroyed_with_live_allocations_count;
    Statistics.destroyed_outstanding_bytes = g_destroyed_outstanding_bytes;
    Statistics.destroyed_outstanding_allocation_count = g_destroyed_outstanding_allocation_count;

    for (const CSystemAllocator* Allocator = g_system_allocator_registry_head; Allocator;
         Allocator = Allocator->m_RegistryNext) {
        Statistics.outstanding_bytes = SaturatingAdd(Statistics.outstanding_bytes, Allocator->BytesAllocated());
        Statistics.outstanding_allocation_count = SaturatingAdd(Statistics.outstanding_allocation_count,
                                                                Allocator->AllocationCount());
    }
    ::ReleaseSRWLockShared(&g_system_allocator_registry_lock);
    return Statistics;
}

void* CSystemAllocator::Alloc(usize Size, usize Alignment, FSourceLoc /*Location*/) noexcept
{
    if (Size == 0) return nullptr;
    usize ActualSize = 0;
    void* const Allocation = AlignedAlloc(Size, Alignment, this, m_Generation, ActualSize);
    if (!Allocation) return nullptr;
    // 統計更新（atomic）
    const u64 CurrentAllocationBytes = m_Bytes.FetchAdd(ActualSize) + ActualSize;
    m_AllocationCount.FetchAdd(1u);
    // ピーク更新は CAS で（cur > peak の間ループ）
    u64 ObservedPeakBytes = m_Peak.Load(EMemoryOrder::Relaxed);
    while (CurrentAllocationBytes > ObservedPeakBytes &&
           !m_Peak.CompareExchange(ObservedPeakBytes, CurrentAllocationBytes)) {}
    return Allocation;
}

void CSystemAllocator::Free(void* Pointer) noexcept
{
    if (!Pointer) return;
    FAlignedFreeDetails Details;
    const EAlignedFreeResult Result = AlignedFree(this, m_Generation, Pointer, Details);
    if (Result != EAlignedFreeResult::Success) {
        EmitFreeFailure(this, m_Generation, Pointer, Details, Result);
        return;
    }
    m_Bytes.FetchSub(Details.freed_size);
    m_AllocationCount.FetchSub(1u);
}

void* CSystemAllocator::Realloc(void* Pointer, usize OldSize, usize NewSize, usize Alignment,
                                FSourceLoc Location) noexcept
{
    (void)OldSize;
    if (!Pointer) {
        return Alloc(NewSize, Alignment, Location);
    }

    const FAllocationHeader* const Header = ValidateAllocationHeader(Pointer);
    if (!Header || Header->owner != this || Header->owner_generation != m_Generation) {
        FAlignedFreeDetails Details;
        EAlignedFreeResult FailureResult = EAlignedFreeResult::InvalidMetadata;
        if (Header) {
            Details.actual_owner = Header->owner;
            Details.actual_owner_generation = Header->owner_generation;
            FailureResult = Header->owner != this ? EAlignedFreeResult::OwnerMismatch
                                                  : EAlignedFreeResult::OwnerGenerationMismatch;
        }
        EmitReallocationFailure(this, m_Generation, Pointer, Details, FailureResult);
        return NewSize == 0 ? Pointer : nullptr;
    }

    if (NewSize == 0) {
        FAlignedFreeDetails Details;
        const EAlignedFreeResult FreeResult = AlignedFree(this, m_Generation, Pointer, Details);
        if (FreeResult != EAlignedFreeResult::Success) {
            EmitReallocationFailure(this, m_Generation, Pointer, Details, FreeResult);
            return Pointer;
        }
        m_Bytes.FetchSub(Details.freed_size);
        m_AllocationCount.FetchSub(1u);
        return nullptr;
    }

    const usize PreviousRequestedSize = Header->requested_size;
    void* const Replacement = Alloc(NewSize, Alignment, Location);
    if (!Replacement) {
        return nullptr;
    }
    MemCopy(Replacement, Pointer, PreviousRequestedSize < NewSize ? PreviousRequestedSize : NewSize);

    FAlignedFreeDetails Details;
    const EAlignedFreeResult FreeResult = AlignedFree(this, m_Generation, Pointer, Details);
    if (FreeResult != EAlignedFreeResult::Success) {
        Free(Replacement);
        EmitReallocationFailure(this, m_Generation, Pointer, Details, FreeResult);
        return nullptr;
    }

    m_Bytes.FetchSub(Details.freed_size);
    m_AllocationCount.FetchSub(1u);
    return Replacement;
}

} // namespace acs
