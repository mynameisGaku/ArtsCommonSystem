// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — FShardedTlsfAllocator 実装
// =============================================================================
#include "memory/ShardedTlsf.h"
#include "memory/VirtualMemory.h"
#include "memory/Memory.h"
#include "threading/ScopedLock.h"
#include "foundation/Move.h"
#include "foundation/Assert.h"
#include "foundation/Compiler.h"
#include "foundation/Platform.h"   // GetSystemInfo

namespace acs {

namespace {

// 論理コア数 (シャード数の自動決定 / clamp 用)。
u32 DetectLogicalCores() noexcept {
    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    const u32 n = static_cast<u32>(si.dwNumberOfProcessors);
    return n > 0u ? n : 1u;
}

// スレッドに割り当てるシャード index。全シャード化アロケータで共有する分散ヒント。
// (各アロケータは自分の m_ShardCount で剰余を取るので共有で問題ない)
ACS_THREAD_LOCAL int t_assigned_shard = -1;

} // namespace

FShardedTlsfAllocator::~FShardedTlsfAllocator() noexcept { Shutdown(); }

TResult<void> FShardedTlsfAllocator::Init(usize total_reserve_bytes, usize commit_initial_bytes,
                                          u32 shard_count) noexcept {
    if (m_Inited) return ACS_ERR(Memory, 50, "FShardedTlsfAllocator: already initialized");

    u32 n = (shard_count != 0u) ? shard_count : DetectLogicalCores();
    if (n > kMaxShards) n = kMaxShards;
    if (n == 0u) n = 1u;

    const usize gran = VmAllocGranularity();
    const usize page = VmPageSize();

    // 各シャードの予約 / 初期コミットを算出 (粒度整列)。
    usize per_reserve = total_reserve_bytes / n;
    per_reserve = (per_reserve + gran - 1u) & ~(gran - 1u);            // 64KiB 整列
    if (per_reserve < 1u * 1024u * 1024u) per_reserve = 1u * 1024u * 1024u;  // 最低 1MiB/shard

    usize per_commit = commit_initial_bytes / n;
    per_commit = (per_commit + page - 1u) & ~(page - 1u);             // ページ整列
    const usize kMinCommit = 64u * 1024u;
    if (per_commit < kMinCommit)  per_commit = kMinCommit;
    if (per_commit > per_reserve) per_commit = per_reserve;

    for (u32 i = 0; i < n; ++i) {
        auto rr = VmReservation::Reserve(per_reserve);
        if (rr.IsErr()) { Shutdown(); return Err<void>(rr.Error()); }
        auto ir = m_Shards[i].alloc.InitWithReservation(Move(rr.Value()), per_commit);
        if (ir.IsErr()) { Shutdown(); return ir; }
    }
    m_ShardCount = n;
    m_Inited = true;
    return Ok();
}

void FShardedTlsfAllocator::Shutdown() noexcept {
    for (u32 i = 0; i < m_ShardCount; ++i) {
        FScopedLock lk(m_Shards[i].lock);
        m_Shards[i].alloc.Reset();   // 予約解放 + 状態リセット (再 Init 可能に)
    }
    m_ShardCount = 0;
    m_Inited = false;
    m_NextShard.Store(0, EMemoryOrder::Relaxed);
}

int FShardedTlsfAllocator::ShardIndexForThread() noexcept {
    if (t_assigned_shard < 0) {
        t_assigned_shard = static_cast<int>(m_NextShard.FetchAdd(1));
    }
    return t_assigned_shard % static_cast<int>(m_ShardCount);
}

int FShardedTlsfAllocator::ShardIndexForPtr(const void* p) const noexcept {
    // ContainsPtr は予約レンジの O(1) 判定 (ロック不要 — 予約 Base/Capacity は Init 後不変)。
    for (u32 i = 0; i < m_ShardCount; ++i) {
        if (m_Shards[i].alloc.ContainsPtr(p)) return static_cast<int>(i);
    }
    return -1;
}

void* FShardedTlsfAllocator::Alloc(usize size, usize alignment, FSourceLoc loc) noexcept {
    if (!m_Inited || size == 0) return nullptr;
    const int start = ShardIndexForThread();
    // 自分のシャードから試し、満杯なら隣へフォールバック (偏り/枯渇でも全体予約まで使える)。
    for (u32 i = 0; i < m_ShardCount; ++i) {
        const u32 idx = static_cast<u32>(start + static_cast<int>(i)) % m_ShardCount;
        Shard& sh = m_Shards[idx];
        FScopedLock lk(sh.lock);
        void* p = sh.alloc.Alloc(size, alignment, loc);
        if (p) return p;
    }
    return nullptr;   // 全シャード満杯 = 真の OOM
}

void FShardedTlsfAllocator::Free(void* ptr) noexcept {
    if (!ptr) return;
    const int s = ShardIndexForPtr(ptr);
    if (s < 0) {
        ACS_ASSERT(false && "FShardedTlsfAllocator::Free: 所有シャード不明 (野良/別アロケータ由来)");
        return;
    }
    FScopedLock lk(m_Shards[s].lock);
    m_Shards[s].alloc.Free(ptr);
}

void* FShardedTlsfAllocator::Realloc(void* ptr, usize old_size, usize new_size,
                                     usize alignment, FSourceLoc loc) noexcept {
    if (!m_Inited) return nullptr;
    if (ptr == nullptr) return Alloc(new_size, alignment, loc);
    if (new_size == 0)  { Free(ptr); return nullptr; }

    const int s = ShardIndexForPtr(ptr);
    if (s < 0) {
        ACS_ASSERT(false && "FShardedTlsfAllocator::Realloc: 所有シャード不明");
        return Alloc(new_size, alignment, loc);
    }
    {
        // 同一シャード内で in-place / 同シャード移動を試みる。
        FScopedLock lk(m_Shards[s].lock);
        void* p = m_Shards[s].alloc.Realloc(ptr, old_size, new_size, alignment, loc);
        if (p) return p;
    }
    // 同シャードで不可 → 別シャードへ移動。ロックを重ねない (デッドロック回避):
    // 先に新規確保 (任意シャード) → コピー → 旧を解放。失敗時は旧を保持 (realloc 契約)。
    void* np = Alloc(new_size, alignment, loc);
    if (!np) return nullptr;
    MemCopy(np, ptr, old_size < new_size ? old_size : new_size);
    Free(ptr);
    return np;
}

u64 FShardedTlsfAllocator::BytesAllocated() const noexcept {
    u64 total = 0;
    for (u32 i = 0; i < m_ShardCount; ++i) total += m_Shards[i].alloc.BytesAllocated();
    return total;   // 統計の読み取りはロックしない (近似で可)
}

u64 FShardedTlsfAllocator::PeakBytes() const noexcept {
    // 各シャードのピーク合算は厳密な「同時ピーク」ではないが、上限の目安として有用。
    u64 total = 0;
    for (u32 i = 0; i < m_ShardCount; ++i) total += m_Shards[i].alloc.PeakBytes();
    return total;
}

bool FShardedTlsfAllocator::ValidateHeap() noexcept {
    for (u32 i = 0; i < m_ShardCount; ++i) {
        FScopedLock lk(m_Shards[i].lock);
        if (!m_Shards[i].alloc.ValidateHeap()) return false;
    }
    return true;
}

} // namespace acs
