// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory - VirtualMemory 実装
// =============================================================================
#include "memory/VirtualMemory.h"
#include "foundation/Platform.h"
#include "foundation/Move.h"
#include "foundation/Compiler.h" // ACS_TARGET_AVX

#if ACS_ARCH_X64
#    include <immintrin.h>
#    include <intrin.h>
#endif
#include <cstdio>
#include <cstring>

namespace acs {

namespace {

/** GetSystemInfo の結果を 1 回だけ取得してキャッシュする小ホルダ。 */
struct FSystemInfoCache {
    /** OS から取得したシステム情報 (ページサイズ/予約粒度等)。 */
    SYSTEM_INFO system_info;

    /** 構築時に GetSystemInfo を 1 回呼ぶ。 */
    FSystemInfoCache() noexcept
    {
        ::GetSystemInfo(&system_info);
    }
};

/** キャッシュ済みのシステム情報を返す。 */
const FSystemInfoCache& GetSystemInfoCache() noexcept
{
    static FSystemInfoCache Cache;
    return Cache;
}

#if ACS_ARCH_X64
/** CPU と OS の双方が AVX の XMM/YMM 状態を有効化しているかを検出する。 */
bool DetectAvxAvailability() noexcept
{
    int Registers[4]{};
    __cpuid(Registers, 0);
    if (Registers[0] < 1) {
        return false;
    }

    __cpuidex(Registers, 1, 0);
    constexpr int OperatingSystemXsaveBit = 1 << 27;
    constexpr int AvxBit = 1 << 28;
    if ((Registers[2] & OperatingSystemXsaveBit) == 0 || (Registers[2] & AvxBit) == 0) {
        return false;
    }

    constexpr unsigned long long XmmAndYmmStateMask = 0x6ull;
    return (_xgetbv(0) & XmmAndYmmStateMask) == XmmAndYmmStateMask;
}

/** 検出結果を一度だけ保持し、ホットパスで CPUID を繰り返さない。 */
bool CanUseAvxStores() noexcept
{
    static const bool bAvailable = DetectAvxAvailability();
    return bAvailable;
}

/** AVX 命令をベースライン経路へ混入させないため、実装を非インライン関数へ隔離する。 */
ACS_NEVERINLINE ACS_TARGET_AVX void ZeroWithAvxNonTemporalStores(void* Destination, usize Size) noexcept
{
    constexpr usize BlockSize = 256; // 8 x 32B
    __m256i* Output = static_cast<__m256i*>(Destination);
    const __m256i Zero = _mm256_setzero_si256();
    const usize BlockCount = Size / BlockSize;
    for (usize Index = 0; Index < BlockCount; ++Index) {
        _mm256_stream_si256(Output + 0, Zero);
        _mm256_stream_si256(Output + 1, Zero);
        _mm256_stream_si256(Output + 2, Zero);
        _mm256_stream_si256(Output + 3, Zero);
        _mm256_stream_si256(Output + 4, Zero);
        _mm256_stream_si256(Output + 5, Zero);
        _mm256_stream_si256(Output + 6, Zero);
        _mm256_stream_si256(Output + 7, Zero);
        Output += 8;
    }
    _mm_sfence();

    const usize CompletedSize = BlockCount * BlockSize;
    if (CompletedSize < Size) {
        ::memset(static_cast<u8*>(Destination) + CompletedSize, 0, Size - CompletedSize);
    }
}
#endif

/** offset と size が OS ページ境界へ整列しているかを返す。 */
bool IsPageAlignedRange(usize Offset, usize Size) noexcept
{
    const usize PageSize = VmPageSize();
    return Size != 0 && PageSize != 0 && (Offset % PageSize) == 0 && (Size % PageSize) == 0;
}

/** 指定範囲全体が同じ OS 状態かを VirtualQuery で確認する。 */
bool RangeHasState(void* ReservationBase, usize Offset, usize Size, DWORD ExpectedState) noexcept
{
    if (!ReservationBase || Size == 0) {
        return false;
    }

    constexpr uptr MaximumAddress = ~static_cast<uptr>(0);
    const uptr BaseAddress = reinterpret_cast<uptr>(ReservationBase);
    if (Offset > MaximumAddress - BaseAddress) {
        return false;
    }

    const uptr RangeBegin = BaseAddress + Offset;
    if (Size > MaximumAddress - RangeBegin) {
        return false;
    }
    const uptr RangeEnd = RangeBegin + Size;

    uptr Cursor = RangeBegin;
    while (Cursor < RangeEnd) {
        MEMORY_BASIC_INFORMATION Information{};
        if (::VirtualQuery(reinterpret_cast<const void*>(Cursor), &Information, sizeof(Information)) == 0) {
            return false;
        }
        if (Information.AllocationBase != ReservationBase || Information.State != ExpectedState) {
            return false;
        }

        const uptr RegionBegin = reinterpret_cast<uptr>(Information.BaseAddress);
        if (Information.RegionSize > MaximumAddress - RegionBegin) {
            return false;
        }
        const uptr RegionEnd = RegionBegin + Information.RegionSize;
        if (RegionEnd <= Cursor) {
            return false;
        }
        Cursor = RegionEnd < RangeEnd ? RegionEnd : RangeEnd;
    }
    return true;
}

/** MEM_RELEASE 失敗を Logger の寿命に依存せず機械可読出力する。 */
void EmitReleaseFailure(const void* ReservationBase, usize Capacity, DWORD OperatingSystemError) noexcept
{
    char Message[320]{};
    const int Length = ::snprintf(Message, sizeof(Message),
                                  "[acs][memory] allocator=VirtualMemory release_failed=true base=%p "
                                  "capacity_bytes=%llu operating_system_error=%lu\r\n",
                                  ReservationBase, static_cast<unsigned long long>(Capacity),
                                  static_cast<unsigned long>(OperatingSystemError));
    if (Length <= 0) {
        return;
    }

    Message[sizeof(Message) - 1u] = '\0';
    ::OutputDebugStringA(Message);
    const HANDLE StandardError = ::GetStdHandle(STD_ERROR_HANDLE);
    if (StandardError && StandardError != INVALID_HANDLE_VALUE) {
        DWORD Written = 0;
        const DWORD ByteCount = static_cast<DWORD>(Length < static_cast<int>(sizeof(Message)) ? Length
                                                                                              : sizeof(Message) - 1u);
        (void)::WriteFile(StandardError, Message, ByteCount, &Written, nullptr);
    }
}

} // namespace

usize VmPageSize() noexcept
{
    return GetSystemInfoCache().system_info.dwPageSize;
}

usize VmAllocGranularity() noexcept
{
    return GetSystemInfoCache().system_info.dwAllocationGranularity;
}

bool VmIsAligned(uptr Address, usize Alignment) noexcept
{
    return Alignment != 0 && (Alignment & (Alignment - 1)) == 0 && (Address & (Alignment - 1)) == 0;
}

FVmReservation::~FVmReservation() noexcept
{
    (void)Release();
}

FVmReservation::FVmReservation(FVmReservation&& Other) noexcept
    : m_Base(Other.m_Base),
      m_Capacity(Other.m_Capacity),
      m_ActiveCommittedBytes(Other.m_ActiveCommittedBytes),
      m_CachedCommittedBytes(Other.m_CachedCommittedBytes),
      m_MaximumCachedDecommitBytes(Other.m_MaximumCachedDecommitBytes),
      m_DecommitCacheCount(Other.m_DecommitCacheCount),
      m_DecommitCacheHits(Other.m_DecommitCacheHits),
      m_DecommitCacheMisses(Other.m_DecommitCacheMisses)
{
    for (u32 Index = 0; Index < Other.m_DecommitCacheCount; ++Index) {
        m_DecommitCache[Index] = Other.m_DecommitCache[Index];
    }
    Other.ResetState();
}

FVmReservation& FVmReservation::operator=(FVmReservation&& Other) noexcept
{
    if (this == &Other) {
        return *this;
    }

    if (Release().IsErr()) {
        return *this;
    }
    m_Base = Other.m_Base;
    m_Capacity = Other.m_Capacity;
    m_ActiveCommittedBytes = Other.m_ActiveCommittedBytes;
    m_CachedCommittedBytes = Other.m_CachedCommittedBytes;
    m_MaximumCachedDecommitBytes = Other.m_MaximumCachedDecommitBytes;
    m_DecommitCacheCount = Other.m_DecommitCacheCount;
    m_DecommitCacheHits = Other.m_DecommitCacheHits;
    m_DecommitCacheMisses = Other.m_DecommitCacheMisses;
    for (u32 Index = 0; Index < Other.m_DecommitCacheCount; ++Index) {
        m_DecommitCache[Index] = Other.m_DecommitCache[Index];
    }
    Other.ResetState();
    return *this;
}

// 仮想範囲を予約する。物理ページはまだ割り当てない。
TResult<FVmReservation> FVmReservation::Reserve(usize CapacityBytes) noexcept
{
    if (CapacityBytes == 0) {
        return ACS_ERR(Memory, 10, "VmReservation::Reserve: capacity 0");
    }

    const usize Granularity = VmAllocGranularity();
    const usize MaximumSize = ~static_cast<usize>(0);
    if (Granularity == 0 || CapacityBytes > MaximumSize - (Granularity - 1)) {
        return ACS_ERR(Memory, 17, "VmReservation::Reserve: capacity overflow");
    }
    CapacityBytes = ((CapacityBytes + Granularity - 1) / Granularity) * Granularity;

    void* const Base = ::VirtualAlloc(nullptr, CapacityBytes, MEM_RESERVE, PAGE_READWRITE);
    if (!Base) {
        const DWORD Error = ::GetLastError();
        return ACS_ERR_OS(Memory, 11, "VirtualAlloc MEM_RESERVE failed", Error);
    }

    FVmReservation Reservation;
    Reservation.m_Base = Base;
    Reservation.m_Capacity = CapacityBytes;
    return TResult<FVmReservation>(OkInit, Move(Reservation));
}

TResult<void> FVmReservation::Release() noexcept
{
    if (!m_Base) {
        return Ok();
    }

    // Flush が失敗しても MEM_RELEASE は予約内の全物理ページをまとめて返せる。
    (void)FlushDecommitCache();
    if (!::VirtualFree(m_Base, 0, MEM_RELEASE)) {
        const DWORD Error = ::GetLastError();
        EmitReleaseFailure(m_Base, m_Capacity, Error);
        return ACS_ERR_OS(Memory, 31, "VirtualFree MEM_RELEASE failed", Error);
    }
    ResetState();
    return Ok();
}

TResult<void> FVmReservation::InsertDecommitCache(usize Offset, usize Size) noexcept
{
    if (Size > m_MaximumCachedDecommitBytes) {
        return ACS_ERR(Memory, 32, "InsertDecommitCache: range exceeds byte limit");
    }

    while (m_DecommitCacheCount > 0 && (m_DecommitCacheCount == kDecommitCacheCapacity ||
                                        m_CachedCommittedBytes > m_MaximumCachedDecommitBytes - Size)) {
        auto Eviction = EvictDecommitCacheEntry(m_DecommitCacheCount - 1);
        if (Eviction.IsErr()) {
            return Eviction;
        }
    }

    for (u32 Index = m_DecommitCacheCount; Index > 0; --Index) {
        m_DecommitCache[Index] = m_DecommitCache[Index - 1];
    }
    m_DecommitCache[0].offset = Offset;
    m_DecommitCache[0].size = Size;
    ++m_DecommitCacheCount;
    return Ok();
}

bool FVmReservation::TakeDecommitCache(usize Offset, usize Size) noexcept
{
    for (u32 Index = 0; Index < m_DecommitCacheCount; ++Index) {
        const FDecommitCacheEntry& Entry = m_DecommitCache[Index];
        if (Entry.offset == Offset && Entry.size == Size) {
            m_CachedCommittedBytes -= Entry.size;
            RemoveDecommitCacheEntry(Index);
            if (m_DecommitCacheHits != ~static_cast<u64>(0)) {
                ++m_DecommitCacheHits;
            }
            return true;
        }
    }
    return false;
}

bool FVmReservation::OverlapsDecommitCache(usize Offset, usize Size) const noexcept
{
    const usize RangeEnd = Offset + Size;
    for (u32 Index = 0; Index < m_DecommitCacheCount; ++Index) {
        const FDecommitCacheEntry& Entry = m_DecommitCache[Index];
        const usize EntryEnd = Entry.offset + Entry.size;
        if (Offset < EntryEnd && Entry.offset < RangeEnd) {
            return true;
        }
    }
    return false;
}

TResult<void> FVmReservation::EvictDecommitCacheEntry(u32 Index) noexcept
{
    if (!m_Base || Index >= m_DecommitCacheCount) {
        return ACS_ERR(Memory, 24, "EvictDecommitCacheEntry: invalid state");
    }

    const FDecommitCacheEntry Entry = m_DecommitCache[Index];
    void* const Address = static_cast<u8*>(m_Base) + Entry.offset;
    if (!::VirtualFree(Address, Entry.size, MEM_DECOMMIT)) {
        const DWORD Error = ::GetLastError();
        return ACS_ERR_OS(Memory, 25, "VirtualFree MEM_DECOMMIT failed", Error);
    }

    m_CachedCommittedBytes -= Entry.size;
    RemoveDecommitCacheEntry(Index);
    return Ok();
}

void FVmReservation::RemoveDecommitCacheEntry(u32 Index) noexcept
{
    for (u32 MoveIndex = Index; MoveIndex + 1 < m_DecommitCacheCount; ++MoveIndex) {
        m_DecommitCache[MoveIndex] = m_DecommitCache[MoveIndex + 1];
    }
    if (m_DecommitCacheCount > 0) {
        --m_DecommitCacheCount;
        m_DecommitCache[m_DecommitCacheCount] = {};
    }
}

TResult<void> FVmReservation::Commit(usize Offset, usize Size) noexcept
{
    if (!m_Base) {
        return ACS_ERR(Memory, 12, "Commit: reservation released");
    }
    if (Size == 0) {
        return ACS_ERR(Memory, 18, "Commit: size 0");
    }
    if (Offset > m_Capacity || Size > m_Capacity - Offset) {
        return ACS_ERR(Memory, 13, "Commit: out of reservation");
    }
    if (!IsPageAlignedRange(Offset, Size)) {
        return ACS_ERR(Memory, 19, "Commit: offset and size must be page aligned");
    }

    if (TakeDecommitCache(Offset, Size)) {
        m_ActiveCommittedBytes += Size;
        return Ok();
    }
    if (OverlapsDecommitCache(Offset, Size)) {
        return ACS_ERR(Memory, 20, "Commit: partial overlap with decommit cache");
    }
    // MEM_COMMIT は本来冪等なので、既に利用中の範囲は統計を変えず成功させる。
    // TLSF の pool 登録が失敗して同じ範囲を再試行する経路とも互換になる。
    if (RangeHasState(m_Base, Offset, Size, MEM_COMMIT)) {
        return Ok();
    }
    if (!RangeHasState(m_Base, Offset, Size, MEM_RESERVE)) {
        return ACS_ERR(Memory, 21, "Commit: range has mixed or invalid state");
    }

    void* const Address = static_cast<u8*>(m_Base) + Offset;
    void* const CommittedAddress = ::VirtualAlloc(Address, Size, MEM_COMMIT, PAGE_READWRITE);
    if (!CommittedAddress) {
        const DWORD Error = ::GetLastError();
        return ACS_ERR_OS(Memory, 14, "VirtualAlloc MEM_COMMIT failed", Error);
    }
    if (CommittedAddress != Address) {
        (void)::VirtualFree(CommittedAddress, Size, MEM_DECOMMIT);
        return ACS_ERR(Memory, 30, "VirtualAlloc MEM_COMMIT returned unexpected address");
    }
    m_ActiveCommittedBytes += Size;
    if (m_DecommitCacheMisses != ~static_cast<u64>(0)) {
        ++m_DecommitCacheMisses;
    }
    return Ok();
}

TResult<void> FVmReservation::Decommit(usize Offset, usize Size) noexcept
{
    if (!m_Base) {
        return ACS_ERR(Memory, 15, "Decommit: reservation released");
    }
    if (Size == 0) {
        return ACS_ERR(Memory, 26, "Decommit: size 0");
    }
    if (Offset > m_Capacity || Size > m_Capacity - Offset) {
        return ACS_ERR(Memory, 16, "Decommit: out of reservation");
    }
    if (!IsPageAlignedRange(Offset, Size)) {
        return ACS_ERR(Memory, 27, "Decommit: offset and size must be page aligned");
    }
    if (OverlapsDecommitCache(Offset, Size)) {
        return ACS_ERR(Memory, 28, "Decommit: duplicate or overlapping cached range");
    }
    if (m_ActiveCommittedBytes < Size || !RangeHasState(m_Base, Offset, Size, MEM_COMMIT)) {
        return ACS_ERR(Memory, 29, "Decommit: range is not actively committed");
    }

    if (Size > m_MaximumCachedDecommitBytes) {
        void* const Address = static_cast<u8*>(m_Base) + Offset;
        if (!::VirtualFree(Address, Size, MEM_DECOMMIT)) {
            const DWORD Error = ::GetLastError();
            return ACS_ERR_OS(Memory, 33, "VirtualFree immediate MEM_DECOMMIT failed", Error);
        }
        m_ActiveCommittedBytes -= Size;
        return Ok();
    }

    auto Insertion = InsertDecommitCache(Offset, Size);
    if (Insertion.IsErr()) {
        return Insertion;
    }
    m_ActiveCommittedBytes -= Size;
    m_CachedCommittedBytes += Size;
    return Ok();
}

TResult<void> FVmReservation::SetMaximumCachedDecommitBytes(usize MaximumCachedDecommitBytes) noexcept
{
    auto TrimResult = TrimDecommitCache(MaximumCachedDecommitBytes);
    if (TrimResult.IsErr()) {
        return TrimResult;
    }
    m_MaximumCachedDecommitBytes = MaximumCachedDecommitBytes;
    return Ok();
}

TResult<void> FVmReservation::TrimDecommitCache(usize TargetCachedBytes) noexcept
{
    while (m_DecommitCacheCount > 0 && m_CachedCommittedBytes > TargetCachedBytes) {
        auto Eviction = EvictDecommitCacheEntry(m_DecommitCacheCount - 1);
        if (Eviction.IsErr()) {
            return Eviction;
        }
    }
    return Ok();
}

TResult<void> FVmReservation::FlushDecommitCache() noexcept
{
    return TrimDecommitCache(0);
}

void FVmReservation::ResetState() noexcept
{
    m_Base = nullptr;
    m_Capacity = 0;
    m_ActiveCommittedBytes = 0;
    m_CachedCommittedBytes = 0;
    m_MaximumCachedDecommitBytes = kDefaultMaximumCachedDecommitBytes;
    for (u32 Index = 0; Index < kDecommitCacheCapacity; ++Index) {
        m_DecommitCache[Index] = {};
    }
    m_DecommitCacheCount = 0;
    m_DecommitCacheHits = 0;
    m_DecommitCacheMisses = 0;
}

void VmZeroFastNT(void* Destination, usize Size) noexcept
{
    if (!Destination || Size == 0) {
        return;
    }

    constexpr usize BlockSize = 256; // 8 x 32B
    if (Size < BlockSize || (reinterpret_cast<uptr>(Destination) & 31) != 0) {
        ::memset(Destination, 0, Size);
        return;
    }

#if ACS_ARCH_X64
    if (CanUseAvxStores()) {
        ZeroWithAvxNonTemporalStores(Destination, Size);
        return;
    }
#endif

    ::memset(Destination, 0, Size);
}

} // namespace acs
