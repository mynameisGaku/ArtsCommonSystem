// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — VirtualMemory 実装
// =============================================================================
#include "memory/VirtualMemory.h"
#include "foundation/Platform.h"
#include "foundation/Move.h"
#include "foundation/Compiler.h"   // ACS_TARGET_AVX

#include <immintrin.h>
#include <cstring>

namespace acs {

namespace {
struct SystemInfoCache {
    SYSTEM_INFO si;
    SystemInfoCache() noexcept { ::GetSystemInfo(&si); }
};
const SystemInfoCache& GetSI() noexcept { static SystemInfoCache c; return c; }
} // namespace

usize VmPageSize() noexcept           { return GetSI().si.dwPageSize; }
usize VmAllocGranularity() noexcept   { return GetSI().si.dwAllocationGranularity; }

bool VmIsAligned(uptr addr, usize alignment) noexcept {
    return (addr & (alignment - 1)) == 0;
}

// ---- VmReservation ------------------------------------------------------

VmReservation::~VmReservation() noexcept {
    Release();
}

VmReservation::VmReservation(VmReservation&& o) noexcept
    : _base(o._base), _capacity(o._capacity), _committed(o._committed),
      _lru_count(o._lru_count), _lru_hits(o._lru_hits), _lru_misses(o._lru_misses) {
    for (u32 i = 0; i < o._lru_count; ++i) _lru[i] = o._lru[i];
    o._base = nullptr;
    o._capacity = 0;
    o._committed = 0;
    o._lru_count = 0;
}

VmReservation& VmReservation::operator=(VmReservation&& o) noexcept {
    if (this == &o) return *this;
    Release();
    _base = o._base;
    _capacity = o._capacity;
    _committed = o._committed;
    _lru_count = o._lru_count;
    _lru_hits = o._lru_hits;
    _lru_misses = o._lru_misses;
    for (u32 i = 0; i < o._lru_count; ++i) _lru[i] = o._lru[i];
    o._base = nullptr;
    o._capacity = 0;
    o._committed = 0;
    o._lru_count = 0;
    return *this;
}

// 仮想範囲を予約する（物理ページは未割当）
Result<VmReservation> VmReservation::Reserve(usize capacity_bytes) noexcept {
    if (capacity_bytes == 0) return ACS_ERR(Memory, 10, "VmReservation::Reserve: capacity 0");
    usize gran = VmAllocGranularity();
    capacity_bytes = (capacity_bytes + gran - 1) & ~(gran - 1);

    void* base = ::VirtualAlloc(nullptr, capacity_bytes, MEM_RESERVE, PAGE_READWRITE);
    if (!base) {
        DWORD err = ::GetLastError();
        return ACS_ERR_OS(Memory, 11, "VirtualAlloc MEM_RESERVE failed", err);
    }
    VmReservation r;
    r._base = base;
    r._capacity = capacity_bytes;
    r._committed = 0;
    return Result<VmReservation>(OkInit, Move(r));
}

void VmReservation::Release() noexcept {
    if (!_base) return;
    LruEvictAll();
    ::VirtualFree(_base, 0, MEM_RELEASE);
    _base = nullptr;
    _capacity = 0;
    _committed = 0;
    _lru_count = 0;
}

// LRU 末尾エントリを実 VirtualFree して新エントリを先頭挿入
void VmReservation::LruInsert(u64 offset, u32 page_count) noexcept {
    if (_lru_count == kLruEntries) {
        const mapped_t& victim = _lru[kLruEntries - 1];
        void* addr = static_cast<u8*>(_base) + (victim.packed_virtual_addr << 16);
        usize bytes = static_cast<usize>(victim.page_count) * VmPageSize();
        ::VirtualFree(addr, bytes, MEM_DECOMMIT);
        --_lru_count;
    }
    for (u32 i = _lru_count; i > 0; --i) _lru[i] = _lru[i - 1];
    mapped_t& head = _lru[0];
    head.packed_virtual_addr = (offset >> 16) & ((1ull << 44) - 1);
    head.page_count          = page_count;
    head.sparse              = 0;
    head.misc                = 0;
    ++_lru_count;
}

// 一致エントリを取り除いて true を返す（再利用可能）
bool VmReservation::LruTake(u64 offset, u32 page_count) noexcept {
    u64 packed = (offset >> 16) & ((1ull << 44) - 1);
    for (u32 i = 0; i < _lru_count; ++i) {
        if (_lru[i].packed_virtual_addr == packed && _lru[i].page_count == page_count) {
            for (u32 j = i; j + 1 < _lru_count; ++j) _lru[j] = _lru[j + 1];
            --_lru_count;
            ++_lru_hits;
            return true;
        }
    }
    ++_lru_misses;
    return false;
}

// LRU 内のすべてを実 VirtualFree
void VmReservation::LruEvictAll() noexcept {
    for (u32 i = 0; i < _lru_count; ++i) {
        const mapped_t& v = _lru[i];
        void* addr = static_cast<u8*>(_base) + (v.packed_virtual_addr << 16);
        usize bytes = static_cast<usize>(v.page_count) * VmPageSize();
        ::VirtualFree(addr, bytes, MEM_DECOMMIT);
    }
    _lru_count = 0;
}

// 物理ページ確保（LRU ヒットなら VirtualAlloc 省略）
Result<void> VmReservation::Commit(usize offset, usize size) noexcept {
    if (!_base) return ACS_ERR(Memory, 12, "Commit: reservation released");
    if (offset + size > _capacity) return ACS_ERR(Memory, 13, "Commit: out of reservation");
    usize page_size = VmPageSize();
    u32 page_count = static_cast<u32>((size + page_size - 1) / page_size);

    if (LruTake(static_cast<u64>(offset), page_count)) {
        _committed += size;
        return Ok();
    }

    void* addr = static_cast<u8*>(_base) + offset;
    void* p = ::VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE);
    if (!p) {
        DWORD err = ::GetLastError();
        return ACS_ERR_OS(Memory, 14, "VirtualAlloc MEM_COMMIT failed", err);
    }
    _committed += size;
    return Ok();
}

// 物理ページ返却（実 VirtualFree は LRU エビクト時）
Result<void> VmReservation::Decommit(usize offset, usize size) noexcept {
    if (!_base) return ACS_ERR(Memory, 15, "Decommit: reservation released");
    if (offset + size > _capacity) return ACS_ERR(Memory, 16, "Decommit: out of reservation");
    usize page_size = VmPageSize();
    u32 page_count = static_cast<u32>((size + page_size - 1) / page_size);
    LruInsert(static_cast<u64>(offset), page_count);
    _committed = (_committed > size) ? _committed - size : 0;
    return Ok();
}

// =============================================================================
// VmZeroFastNT — Non-Temporal write 版高速ゼロクリア
// =============================================================================
// ACS_TARGET_AVX: この関数だけ AVX を許可する（Clang/clang-cl は AVX 組み込み
// 関数を使う関数に target 属性を要求する。MSVC では空に展開される）。
ACS_TARGET_AVX void VmZeroFastNT(void* dst, usize size) noexcept {
    constexpr usize kBlock = 256;  // 8 × 32B
    if (size < kBlock || (reinterpret_cast<uptr>(dst) & 31) != 0) {
        ::memset(dst, 0, size);
        return;
    }
    __m256i* p = static_cast<__m256i*>(dst);
    const __m256i z = _mm256_setzero_si256();
    usize blocks = size / kBlock;
    for (usize i = 0; i < blocks; ++i) {
        _mm256_stream_si256(p + 0, z);
        _mm256_stream_si256(p + 1, z);
        _mm256_stream_si256(p + 2, z);
        _mm256_stream_si256(p + 3, z);
        _mm256_stream_si256(p + 4, z);
        _mm256_stream_si256(p + 5, z);
        _mm256_stream_si256(p + 6, z);
        _mm256_stream_si256(p + 7, z);
        p += 8;
    }
    _mm_sfence();
    usize done = blocks * kBlock;
    if (done < size) ::memset(static_cast<u8*>(dst) + done, 0, size - done);
}

} // namespace acs
