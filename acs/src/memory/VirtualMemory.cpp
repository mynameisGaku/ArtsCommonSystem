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
/** GetSystemInfo の結果を 1 回だけ取得してキャッシュする小ホルダ。 */
struct SystemInfoCache {
    /** OS から取得したシステム情報 (ページサイズ/予約粒度等)。 */
    SYSTEM_INFO si;

    /** 構築時に GetSystemInfo を 1 回呼ぶ。 */
    SystemInfoCache() noexcept { ::GetSystemInfo(&si); }
};

/**
 * キャッシュ済みのシステム情報を返す (初回呼び出しで取得)。
 *
 * @return プロセス唯一の SystemInfoCache への参照。
 */
const SystemInfoCache& GetSI() noexcept { static SystemInfoCache c; return c; }
} // namespace

usize VmPageSize() noexcept           { return GetSI().si.dwPageSize; }
usize VmAllocGranularity() noexcept   { return GetSI().si.dwAllocationGranularity; }

bool VmIsAligned(uptr addr, usize alignment) noexcept {
    return (addr & (alignment - 1)) == 0;
}

VmReservation::~VmReservation() noexcept {
    Release();
}

VmReservation::VmReservation(VmReservation&& o) noexcept
    : m_Base(o.m_Base), m_Capacity(o.m_Capacity), m_Committed(o.m_Committed),
      m_LruCount(o.m_LruCount), m_LruHits(o.m_LruHits), m_LruMisses(o.m_LruMisses) {
    for (u32 i = 0; i < o.m_LruCount; ++i) m_Lru[i] = o.m_Lru[i];
    o.m_Base = nullptr;
    o.m_Capacity = 0;
    o.m_Committed = 0;
    o.m_LruCount = 0;
}

VmReservation& VmReservation::operator=(VmReservation&& o) noexcept {
    if (this == &o) return *this;
    Release();
    m_Base = o.m_Base;
    m_Capacity = o.m_Capacity;
    m_Committed = o.m_Committed;
    m_LruCount = o.m_LruCount;
    m_LruHits = o.m_LruHits;
    m_LruMisses = o.m_LruMisses;
    for (u32 i = 0; i < o.m_LruCount; ++i) m_Lru[i] = o.m_Lru[i];
    o.m_Base = nullptr;
    o.m_Capacity = 0;
    o.m_Committed = 0;
    o.m_LruCount = 0;
    return *this;
}

// 仮想範囲を予約する（物理ページは未割当）
TResult<VmReservation> VmReservation::Reserve(usize capacity_bytes) noexcept {
    if (capacity_bytes == 0) return ACS_ERR(Memory, 10, "VmReservation::Reserve: capacity 0");
    const usize gran = VmAllocGranularity();
    capacity_bytes = (capacity_bytes + gran - 1) & ~(gran - 1);

    void* const base = ::VirtualAlloc(nullptr, capacity_bytes, MEM_RESERVE, PAGE_READWRITE);
    if (!base) {
        const DWORD err = ::GetLastError();
        return ACS_ERR_OS(Memory, 11, "VirtualAlloc MEM_RESERVE failed", err);
    }
    VmReservation r;
    r.m_Base = base;
    r.m_Capacity = capacity_bytes;
    r.m_Committed = 0;
    return TResult<VmReservation>(OkInit, Move(r));
}

void VmReservation::Release() noexcept {
    if (!m_Base) return;
    LruEvictAll();
    ::VirtualFree(m_Base, 0, MEM_RELEASE);
    m_Base = nullptr;
    m_Capacity = 0;
    m_Committed = 0;
    m_LruCount = 0;
}

// LRU 末尾エントリを実 VirtualFree して新エントリを先頭挿入
void VmReservation::LruInsert(u64 offset, u32 page_count) noexcept {
    if (m_LruCount == kLruEntries) {
        const mapped_t& victim = m_Lru[kLruEntries - 1];
        void* const addr = static_cast<u8*>(m_Base) + (victim.packed_virtual_addr << 16);
        const usize bytes = static_cast<usize>(victim.page_count) * VmPageSize();
        ::VirtualFree(addr, bytes, MEM_DECOMMIT);
        --m_LruCount;
    }
    for (u32 i = m_LruCount; i > 0; --i) m_Lru[i] = m_Lru[i - 1];
    mapped_t& head = m_Lru[0];
    head.packed_virtual_addr = (offset >> 16) & ((1ull << 44) - 1);
    head.page_count          = page_count;
    head.sparse              = 0;
    head.misc                = 0;
    ++m_LruCount;
}

// 一致エントリを取り除いて true を返す（再利用可能）
bool VmReservation::LruTake(u64 offset, u32 page_count) noexcept {
    const u64 packed = (offset >> 16) & ((1ull << 44) - 1);
    for (u32 i = 0; i < m_LruCount; ++i) {
        if (m_Lru[i].packed_virtual_addr == packed && m_Lru[i].page_count == page_count) {
            for (u32 j = i; j + 1 < m_LruCount; ++j) m_Lru[j] = m_Lru[j + 1];
            --m_LruCount;
            ++m_LruHits;
            return true;
        }
    }
    ++m_LruMisses;
    return false;
}

// LRU 内のすべてを実 VirtualFree
void VmReservation::LruEvictAll() noexcept {
    for (u32 i = 0; i < m_LruCount; ++i) {
        const mapped_t& v = m_Lru[i];
        void* const addr = static_cast<u8*>(m_Base) + (v.packed_virtual_addr << 16);
        const usize bytes = static_cast<usize>(v.page_count) * VmPageSize();
        ::VirtualFree(addr, bytes, MEM_DECOMMIT);
    }
    m_LruCount = 0;
}

// 物理ページ確保（LRU ヒットなら VirtualAlloc 省略）
TResult<void> VmReservation::Commit(usize offset, usize size) noexcept {
    if (!m_Base) return ACS_ERR(Memory, 12, "Commit: reservation released");
    if (offset + size > m_Capacity) return ACS_ERR(Memory, 13, "Commit: out of reservation");
    const usize page_size = VmPageSize();
    const u32 page_count = static_cast<u32>((size + page_size - 1) / page_size);

    if (LruTake(static_cast<u64>(offset), page_count)) {
        m_Committed += size;
        return Ok();
    }

    void* const addr = static_cast<u8*>(m_Base) + offset;
    void* const p = ::VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE);
    if (!p) {
        const DWORD err = ::GetLastError();
        return ACS_ERR_OS(Memory, 14, "VirtualAlloc MEM_COMMIT failed", err);
    }
    m_Committed += size;
    return Ok();
}

// 物理ページ返却（実 VirtualFree は LRU エビクト時）
TResult<void> VmReservation::Decommit(usize offset, usize size) noexcept {
    if (!m_Base) return ACS_ERR(Memory, 15, "Decommit: reservation released");
    if (offset + size > m_Capacity) return ACS_ERR(Memory, 16, "Decommit: out of reservation");
    const usize page_size = VmPageSize();
    const u32 page_count = static_cast<u32>((size + page_size - 1) / page_size);
    LruInsert(static_cast<u64>(offset), page_count);
    m_Committed = (m_Committed > size) ? m_Committed - size : 0;
    return Ok();
}

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
    const usize blocks = size / kBlock;
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
    const usize done = blocks * kBlock;
    if (done < size) ::memset(static_cast<u8*>(dst) + done, 0, size - done);
}

} // namespace acs
