// SPDX-License-Identifier: Apache-2.0
// TLSF アロケータ（Two-Level Segregated Fit、O(1) alloc/free）
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "memory/Allocator.h"
#include "memory/VirtualMemory.h"

namespace acs {

namespace tlsf {

// 構成定数
constexpr int FL_INDEX_MAX     = 32;                          // 最大単一ブロック 4 GiB
constexpr int SL_INDEX_LOG2    = 5;                           // 32 サブバケット
constexpr int SL_INDEX_COUNT   = 1 << SL_INDEX_LOG2;
constexpr int FL_INDEX_SHIFT   = SL_INDEX_LOG2 + 2;
constexpr int FL_INDEX_COUNT   = FL_INDEX_MAX - FL_INDEX_SHIFT + 1;
constexpr int SMALL_BLOCK_SIZE = 1 << FL_INDEX_SHIFT;
constexpr int ALIGN_SIZE       = 16;
constexpr int MIN_BLOCK_SIZE   = 32;

// ブロックヘッダ。フリー時はリストポインタが payload とオーバーレイする
struct FBlockHeader {
    FBlockHeader* prev_phys_block;   // 物理的に前のブロック
    usize        size_and_flags;    // 上位ビット=サイズ、ビット 0=this_free、ビット 1=prev_free
    FBlockHeader* next_free;         // フリー時のみ有効
    FBlockHeader* prev_free;         // フリー時のみ有効
};

} // namespace tlsf

class FTlsfAllocator final : public FAllocator {
public:
    FTlsfAllocator() noexcept = default;
    ~FTlsfAllocator() noexcept override;

    FTlsfAllocator(const FTlsfAllocator&) = delete;
    FTlsfAllocator& operator=(const FTlsfAllocator&) = delete;

    // 単一プールで初期化（pool_base は 16 バイト整列、pool_size >= 1KB 推奨）
    TResult<void> Init(void* pool_base, usize pool_size) noexcept;

    // VmReservation を保持しつつ commit_initial バイトをプールとして登録
    TResult<void> InitWithReservation(VmReservation&& reservation,
                                     usize commit_initial) noexcept;

    // 追加プール登録（既存プール枯渇時など）
    TResult<void> AddPool(void* pool_base, usize pool_size) noexcept;

    void* Alloc(usize size, usize alignment, FSourceLoc loc) noexcept override;
    void  Free (void* ptr)                                  noexcept override;
    void* Realloc(void* ptr, usize old_size, usize new_size,
                  usize alignment, FSourceLoc loc)           noexcept override;

    u64 BytesAllocated() const noexcept override { return _bytes_used; }
    u64 PeakBytes()      const noexcept override { return _bytes_peak; }
    const char* Name()   const noexcept override { return "TLSF"; }

    struct Stats {
        u64 bytes_used;
        u64 bytes_peak;
        u64 free_blocks;
        u64 used_blocks;
        u64 largest_free_block;
    };
    Stats GetStats() const noexcept;

private:
    u32              _fl_bitmap = 0;
    u32              _sl_bitmap[tlsf::FL_INDEX_COUNT] = {};
    tlsf::FBlockHeader* _blocks[tlsf::FL_INDEX_COUNT][tlsf::SL_INDEX_COUNT] = {};
    tlsf::FBlockHeader  _null_block {};

    VmReservation    _reservation;
    bool             _owns_reservation = false;

    u64              _bytes_used = 0;
    u64              _bytes_peak = 0;

    void InsertFreeBlock(tlsf::FBlockHeader* block) noexcept;
    void RemoveFreeBlock(tlsf::FBlockHeader* block) noexcept;
    tlsf::FBlockHeader* SearchSuitableBlock(int& fl, int& sl) noexcept;
    void TrimFreeBlock(tlsf::FBlockHeader* block, usize size) noexcept;
    tlsf::FBlockHeader* MergePrev(tlsf::FBlockHeader* block) noexcept;
    tlsf::FBlockHeader* MergeNext(tlsf::FBlockHeader* block) noexcept;
};

} // namespace acs
