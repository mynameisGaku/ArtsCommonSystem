// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — 仮想メモリ層（VirtualAlloc Reserve + Commit + LRU キャッシュ）
// -----------------------------------------------------------------------------
// 巨大な仮想アドレス空間を予約し、必要なページだけを物理にコミットする
// 低レベル抽象。Decommit したページは内部 LRU キャッシュ（16 エントリ）に
// 保持され、再 Commit 要求がヒットすればシステムコールを省略する。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"

namespace acs {

// ---- ページサイズ定数（論理単位） ----------------------------------------
inline constexpr usize kVmSmallPageSize  = 16 * 1024;          // 16 KiB
inline constexpr usize kVmMediumPageSize = 128 * 1024;         // 128 KiB
inline constexpr usize kVmSegmentSize    = 8 * 1024 * 1024;    // 8 MiB

// ---- OS ページ情報 -------------------------------------------------------
usize VmPageSize() noexcept;             // OS のページサイズ
usize VmAllocGranularity() noexcept;     // VirtualAlloc 予約粒度
bool  VmIsAligned(uptr addr, usize alignment) noexcept;

// =============================================================================
// マップ済み領域・ページ群を 8 バイトに圧縮するパック型
// =============================================================================

// マップ済み領域の記述子（8 バイト）
struct mapped_t {
    u64 packed_virtual_addr : 44;   // 仮想アドレス（64KiB 整列で十分な範囲）
    u64 page_count          : 16;   // ページ数
    u64 sparse              :  1;   // スパース管理フラグ
    u64 misc                :  3;   // その他フラグ
};
static_assert(sizeof(mapped_t) == 8, "mapped_t must be 8 bytes");

// 連続ページ群の記述子（8 バイト）
struct page_t {
    u64 continuous_page_count : 12; // 連続するページ数
    u64 misc                  :  4;
    u64 packed_virtual_addr   : 48; // 仮想アドレス（ページ整列）
};
static_assert(sizeof(page_t) == 8, "page_t must be 8 bytes");

// =============================================================================
// VmReservation — 仮想アドレス範囲の RAII ハンドル
// -----------------------------------------------------------------------------
// Reserve(N) で N バイトの予約を取り、デストラクタで Release する。
// Commit(offset, size) で必要な部分だけ物理ページを割り当て、
// Decommit はまず LRU キャッシュに入れて、エビクト時に実 VirtualFree する。
// =============================================================================
class VmReservation {
public:
    VmReservation() noexcept = default;
    ~VmReservation() noexcept;

    VmReservation(const VmReservation&) = delete;
    VmReservation& operator=(const VmReservation&) = delete;
    VmReservation(VmReservation&& o) noexcept;
    VmReservation& operator=(VmReservation&& o) noexcept;

    // 仮想範囲予約（VirtualAlloc MEM_RESERVE）
    static Result<VmReservation> Reserve(usize capacity_bytes) noexcept;

    void Release() noexcept;

    // 物理ページ確保。LRU ヒット時はシステムコールを省略する。
    Result<void> Commit  (usize offset, usize size) noexcept;
    // 物理ページ返却（実際の VirtualFree は LRU エビクト時）
    Result<void> Decommit(usize offset, usize size) noexcept;

    void* Base()      const noexcept { return _base; }
    usize Capacity()  const noexcept { return _capacity; }
    usize Committed() const noexcept { return _committed; }

    // LRU キャッシュ統計（プロファイラ用）
    u32   LruHitCount()  const noexcept { return _lru_hits; }
    u32   LruMissCount() const noexcept { return _lru_misses; }

private:
    static constexpr u32 kLruEntries = 16;
    void  LruInsert(u64 offset, u32 page_count) noexcept;
    bool  LruTake(u64 offset, u32 page_count) noexcept;
    void  LruEvictAll() noexcept;

    void*       _base       = nullptr;
    usize       _capacity   = 0;
    usize       _committed  = 0;

    mapped_t    _lru[kLruEntries] {};
    u32         _lru_count = 0;
    u32         _lru_hits = 0;
    u32         _lru_misses = 0;
};

// =============================================================================
// VmZeroFastNT — Non-Temporal write による高速ゼロクリア
// -----------------------------------------------------------------------------
// L1/L2 を汚染せず DDR へ直書きする。32B 整列かつ 256B 倍数ならアンロール
// 版を使い、それ以外は memset にフォールバックする。
// =============================================================================
void VmZeroFastNT(void* dst, usize size) noexcept;

} // namespace acs
