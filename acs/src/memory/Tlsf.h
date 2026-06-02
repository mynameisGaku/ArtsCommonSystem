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

    u64 BytesAllocated() const noexcept override { return m_BytesUsed; }
    u64 PeakBytes()      const noexcept override { return m_BytesPeak; }
    const char* Name()   const noexcept override { return "TLSF"; }

    struct Stats {
        u64 bytes_used;
        u64 bytes_peak;
        u64 free_blocks;
        u64 used_blocks;
        u64 largest_free_block;
    };
    Stats GetStats() const noexcept;

    // ヒープ整合検証 (物理ブロックチェイン + prev_phys/prev_free フラグの一貫性)。
    // 破損していたら false。デバッグ/診断用 (O(ブロック数))。
    // スレッドセーフではない — Alloc/Free と同じロック下で呼ぶこと。
    bool ValidateHeap() const noexcept;

private:
    u32              m_FlBitmap = 0;
    u32              m_SlBitmap[tlsf::FL_INDEX_COUNT] = {};
    tlsf::FBlockHeader* m_Blocks[tlsf::FL_INDEX_COUNT][tlsf::SL_INDEX_COUNT] = {};
    tlsf::FBlockHeader  m_NullBlock {};

    VmReservation    m_Reservation;
    bool             m_bOwnsReservation = false;
    // 予約のうち既にコミット済みでプール登録された総バイト数 (= 次に commit する offset)。
    // auto-grow: OOM 時にここから予約末尾までを段階的に commit + AddPool して伸ばす。
    usize            m_CommittedBytes = 0;

    u64              m_BytesUsed = 0;
    u64              m_BytesPeak = 0;

    // 安全性: 払い出したポインタの所属プール範囲を記録し、Free 時に範囲外(野良/別
    // アロケータ由来)ポインタや二重 free を検出してヒープ破壊を未然に防ぐ。
    // プール数が上限を超えたら追跡を諦め (overflow)、範囲検証はスキップする(誤検出回避)。
    // auto-grow で複数プールが登録され得るため広めに確保 (幾何級数 grow なら対数個で収まる)。
    static constexpr int kMaxTrackedPools = 64;
    struct PoolSpan { uptr lo; uptr hi; };  // [lo, hi) = pool_base .. pool_base+pool_size
    PoolSpan         m_PoolSpans[kMaxTrackedPools] = {};
    int              m_PoolSpanCount   = 0;
    bool             m_PoolTrackOverflow = false;

    // ptr がいずれかの登録プール範囲内か (overflow 時は検証不能なので true 扱い)
    bool OwnsPointer(const void* p) const noexcept;

    // OOM 時に予約から needed_bytes 以上を収容できるよう commit + AddPool して伸ばす。
    // 予約を所有しない (HeapAlloc プール等) / 予約を使い切った場合は false。
    bool GrowToFit(usize needed_bytes) noexcept;

    void InsertFreeBlock(tlsf::FBlockHeader* block) noexcept;
    void RemoveFreeBlock(tlsf::FBlockHeader* block) noexcept;
    tlsf::FBlockHeader* SearchSuitableBlock(int& fl, int& sl) noexcept;
    void TrimFreeBlock(tlsf::FBlockHeader* block, usize size) noexcept;
    // used ブロックを size に縮め、余りを free ブロックとして解放する (in-place realloc 用)。
    // 末尾の free 隣接ブロックがあれば統合する。切り出す余裕が無ければ何もしない。
    void TrimUsedBlock(tlsf::FBlockHeader* block, usize size) noexcept;
    tlsf::FBlockHeader* MergePrev(tlsf::FBlockHeader* block) noexcept;
    tlsf::FBlockHeader* MergeNext(tlsf::FBlockHeader* block) noexcept;
};

} // namespace acs
