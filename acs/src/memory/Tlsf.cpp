// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — TLSF 実装
// -----------------------------------------------------------------------------
// (FL, SL) の 2 段ビットマップでフリーリストを索引付けし、O(1) で
// alloc / free を行う。隣接フリーブロックは O(1) で統合される。
// =============================================================================
#include "memory/Tlsf.h"
#include "memory/Memory.h"
#include "foundation/Assert.h"
#include "foundation/Move.h"

#include <intrin.h>

namespace acs {

namespace tlsf {

// 上位ビットの位置を返す（log2 相当）
ACS_FORCEINLINE int Fls(u32 v) noexcept {
    if (v == 0) return -1;
    unsigned long idx;
    _BitScanReverse(&idx, v);
    return static_cast<int>(idx);
}

// 下位の最初に立っているビット位置を返す
ACS_FORCEINLINE int Ffs(u32 v) noexcept {
    if (v == 0) return -1;
    unsigned long idx;
    _BitScanForward(&idx, v);
    return static_cast<int>(idx);
}

// 64bit 用の Fls
ACS_FORCEINLINE int FlsSize(usize v) noexcept {
    if (v == 0) return -1;
    unsigned long idx;
#if ACS_ARCH_X64
    _BitScanReverse64(&idx, static_cast<unsigned __int64>(v));
#else
    _BitScanReverse(&idx, static_cast<unsigned long>(v));
#endif
    return static_cast<int>(idx);
}

// size_and_flags の下位 2 ビットをフラグに使う
constexpr usize kBlockFreeBit     = 1ull << 0;
constexpr usize kPrevFreeBit      = 1ull << 1;
constexpr usize kBlockSizeMask    = ~(kBlockFreeBit | kPrevFreeBit);

// payload 起点までのオフセット
constexpr usize kBlockHeaderOverhead = sizeof(usize);
constexpr usize kBlockStartOffset = sizeof(FBlockHeader*) + sizeof(usize);

// サイズ部分だけ取り出す
ACS_FORCEINLINE usize BlockSize(const FBlockHeader* b) noexcept {
    return b->size_and_flags & kBlockSizeMask;
}
// フラグを保ちつつサイズだけ書き換える
ACS_FORCEINLINE void  SetBlockSize(FBlockHeader* b, usize size) noexcept {
    usize old_flags = b->size_and_flags & ~kBlockSizeMask;
    b->size_and_flags = size | old_flags;
}
ACS_FORCEINLINE bool  IsFree(const FBlockHeader* b) noexcept {
    return (b->size_and_flags & kBlockFreeBit) != 0;
}
ACS_FORCEINLINE bool  IsPrevFree(const FBlockHeader* b) noexcept {
    return (b->size_and_flags & kPrevFreeBit) != 0;
}
ACS_FORCEINLINE void  MarkFree(FBlockHeader* b) noexcept    { b->size_and_flags |= kBlockFreeBit; }
ACS_FORCEINLINE void  MarkUsed(FBlockHeader* b) noexcept    { b->size_and_flags &= ~kBlockFreeBit; }
ACS_FORCEINLINE void  MarkPrevFree(FBlockHeader* b) noexcept{ b->size_and_flags |= kPrevFreeBit; }
ACS_FORCEINLINE void  MarkPrevUsed(FBlockHeader* b) noexcept{ b->size_and_flags &= ~kPrevFreeBit; }

// ヘッダから payload ポインタへ
ACS_FORCEINLINE void* BlockToPtr(const FBlockHeader* b) noexcept {
    return reinterpret_cast<u8*>(const_cast<FBlockHeader*>(b)) + kBlockStartOffset;
}
// payload からヘッダへ戻る
ACS_FORCEINLINE FBlockHeader* PtrToBlock(const void* p) noexcept {
    return reinterpret_cast<FBlockHeader*>(reinterpret_cast<u8*>(const_cast<void*>(p)) - kBlockStartOffset);
}

// 物理的に次のブロックを得る（解放時の隣接統合に使う）
ACS_FORCEINLINE FBlockHeader* NextBlock(const FBlockHeader* b) noexcept {
    return reinterpret_cast<FBlockHeader*>(
        reinterpret_cast<u8*>(BlockToPtr(b)) + BlockSize(b) - kBlockHeaderOverhead);
}

// サイズから (FL, SL) インデックスを計算
ACS_FORCEINLINE void MappingInsert(usize size, int& fl, int& sl) noexcept {
    if (size < SMALL_BLOCK_SIZE) {
        // 小サイズは線形領域に均等割り
        fl = 0;
        sl = static_cast<int>(size) / (SMALL_BLOCK_SIZE / SL_INDEX_COUNT);
    } else {
        // 通常領域は log2 + 上位ビット切り出しで対数的に分配
        fl = FlsSize(size);
        sl = static_cast<int>((size >> (fl - SL_INDEX_LOG2)) ^ (1u << SL_INDEX_LOG2));
        fl -= (FL_INDEX_SHIFT - 1);
        // size >= 2^32 では fl が FL_INDEX_COUNT(=26) に達し配列を 1 つ越える。
        // 範囲外参照を防ぐため最上位バケットへクランプ（>=4GiB 単一ブロックは top bucket 扱い）。
        ACS_ASSERT(fl < FL_INDEX_COUNT);
        if (fl >= FL_INDEX_COUNT) fl = FL_INDEX_COUNT - 1;
    }
}

// alloc 用: 要求サイズを「次のサブバケット境界」まで切り上げる
ACS_FORCEINLINE void MappingSearch(usize size, int& fl, int& sl) noexcept {
    if (size >= SMALL_BLOCK_SIZE) {
        usize round = (1ull << (FlsSize(size) - SL_INDEX_LOG2)) - 1;
        size += round;
    }
    MappingInsert(size, fl, sl);
}

// 要求サイズを block_size へ変換する。
//
// 重要な不変条件: 連続するブロックヘッダの間隔は block_size + kBlockHeaderOverhead。
// 先頭ブロックは 16 整列、payload は header + kBlockStartOffset(=16) なので、すべての
// payload を 16 整列に保つには「全ブロックヘッダが 16 整列」である必要があり、それには
//     (block_size + kBlockHeaderOverhead) ≡ 0  (mod ALIGN_SIZE)
//   ⇔ block_size ≡ ALIGN_SIZE - kBlockHeaderOverhead ≡ 8  (mod 16)
// でなければならない。AlignUp(size,16) は ≡0 になり連鎖の途中で payload が 8 整列に
// 落ちる (確保の約半数が 16 整列を満たさない) ため、+kBlockHeaderOverhead して ≡8 に補正する。
// 先頭プールブロック (pool_size-24) も ≡8、分割/統合もこの剰余を保存する。
ACS_FORCEINLINE usize AdjustRequestSize(usize size, usize /*align*/) noexcept {
    if (size == 0) return 0;
    // AlignUp(size,16) (>= size, ≡0 mod16) に overhead を足して ≡8 mod16 にする。
    // 結果として使用可能領域 block_size - overhead >= size を必ず満たす。
    usize adjust = AlignUp(size, ALIGN_SIZE) + kBlockHeaderOverhead;
    // ≡8 を保った最小ブロック (free 時に next_free/prev_free を収容できる) へクランプ。
    constexpr usize kMinBlock8 = MIN_BLOCK_SIZE - kBlockHeaderOverhead; // 32-8 = 24, ≡8 mod16
    if (adjust < kMinBlock8) adjust = kMinBlock8;
    return adjust;
}

} // namespace tlsf

// =============================================================================
// FTlsfAllocator 実装
// =============================================================================

FTlsfAllocator::~FTlsfAllocator() noexcept {}

// 単一プール初期化
TResult<void> FTlsfAllocator::Init(void* pool_base, usize pool_size) noexcept {
    if (!pool_base || pool_size < 1024) return ACS_ERR(Memory, 20, "TLSF::Init invalid pool");
    if ((reinterpret_cast<uptr>(pool_base) & (tlsf::ALIGN_SIZE - 1)) != 0)
        return ACS_ERR(Memory, 21, "TLSF::Init pool not 16B aligned");

    // ビットマップとリストヘッドを 0 / 番兵で初期化
    m_FlBitmap = 0;
    for (int i = 0; i < tlsf::FL_INDEX_COUNT; ++i) {
        m_SlBitmap[i] = 0;
        for (int j = 0; j < tlsf::SL_INDEX_COUNT; ++j) {
            m_Blocks[i][j] = &m_NullBlock;
        }
    }
    // 番兵ノードは自分を指す循環リスト（リスト末端の表現）
    m_NullBlock.next_free = &m_NullBlock;
    m_NullBlock.prev_free = &m_NullBlock;
    m_NullBlock.size_and_flags = 0;
    m_NullBlock.prev_phys_block = nullptr;

    return AddPool(pool_base, pool_size);
}

// VmReservation も保持する初期化
TResult<void> FTlsfAllocator::InitWithReservation(VmReservation&& reservation,
                                                usize commit_initial) noexcept {
    auto cr = reservation.Commit(0, commit_initial);
    if (cr.IsErr()) return cr;
    void* base = reservation.Base();
    auto r = Init(base, commit_initial);
    if (r.IsErr()) return r;
    m_Reservation = Move(reservation);
    m_bOwnsReservation = true;
    return Ok();
}

// プールを TLSF に登録（先頭にフリーブロック 1 個 + 末尾に終端番兵）
TResult<void> FTlsfAllocator::AddPool(void* pool_base, usize pool_size) noexcept {
    using namespace tlsf;

    // pool_size を 16B 境界に切り下げ
    pool_size &= ~(usize(ALIGN_SIZE - 1));
    if (pool_size < MIN_BLOCK_SIZE + kBlockHeaderOverhead * 2)
        return ACS_ERR(Memory, 22, "AddPool too small");

    // プール全体を覆う巨大フリーブロックを生成
    FBlockHeader* block = reinterpret_cast<FBlockHeader*>(
        static_cast<u8*>(pool_base) - kBlockStartOffset + kBlockStartOffset);
    block->prev_phys_block = nullptr;
    usize block_size = pool_size - kBlockStartOffset - kBlockHeaderOverhead;
    block->size_and_flags = block_size | kBlockFreeBit;
    block->next_free = nullptr;
    block->prev_free = nullptr;
    InsertFreeBlock(block);

    // 末尾に「サイズ 0、used」の終端番兵を置く（隣接統合のループ終端）
    FBlockHeader* sentinel = NextBlock(block);
    sentinel->prev_phys_block = block;
    sentinel->size_and_flags = 0;
    MarkPrevFree(sentinel);
    return Ok();
}

// 指定ブロックを (FL, SL) バケットの先頭に挿入
void FTlsfAllocator::InsertFreeBlock(tlsf::FBlockHeader* block) noexcept {
    using namespace tlsf;
    int fl, sl;
    MappingInsert(BlockSize(block), fl, sl);

    // 双方向リンクで先頭挿入
    FBlockHeader* current = m_Blocks[fl][sl];
    block->next_free = current;
    block->prev_free = &m_NullBlock;
    current->prev_free = block;
    m_Blocks[fl][sl] = block;

    // ビットマップに「このバケット非空」を立てる
    m_FlBitmap |= (1u << fl);
    m_SlBitmap[fl] |= (1u << sl);
}

// フリーリストから取り除く（バケットが空なら対応ビットも下ろす）
void FTlsfAllocator::RemoveFreeBlock(tlsf::FBlockHeader* block) noexcept {
    using namespace tlsf;
    FBlockHeader* prev = block->prev_free;
    FBlockHeader* next = block->next_free;
    next->prev_free = prev;
    prev->next_free = next;

    int fl, sl;
    MappingInsert(BlockSize(block), fl, sl);
    if (m_Blocks[fl][sl] == block) {
        m_Blocks[fl][sl] = next;
        // バケットが空になったらビットマップを下ろす
        if (next == &m_NullBlock) {
            m_SlBitmap[fl] &= ~(1u << sl);
            if (m_SlBitmap[fl] == 0) {
                m_FlBitmap &= ~(1u << fl);
            }
        }
    }
}

// 要求サイズ以上のフリーブロックが入ったバケットを O(1) で見つける
tlsf::FBlockHeader* FTlsfAllocator::SearchSuitableBlock(int& fl, int& sl) noexcept {
    using namespace tlsf;
    // 同じ FL 内で sl 以上のビットを探す
    u32 sl_map = m_SlBitmap[fl] & (~0u << sl);
    if (sl_map == 0) {
        // 見つからなければ FL を 1 つ上にずらす
        u32 fl_map = m_FlBitmap & (~0u << (fl + 1));
        if (fl_map == 0) return nullptr;  // OOM
        fl = Ffs(fl_map);
        sl_map = m_SlBitmap[fl];
    }
    sl = Ffs(sl_map);
    return m_Blocks[fl][sl];
}

// 余剰サイズを切り出して新しいフリーブロックとして再登録
void FTlsfAllocator::TrimFreeBlock(tlsf::FBlockHeader* block, usize size) noexcept {
    using namespace tlsf;
    // **引き算の前に比較形でガードする** (canonical TLSF の block_can_split)。
    // exact-fit (BlockSize==size) や僅差では `BlockSize - size - overhead` が
    // unsigned で wrap (~2^64) し、後段の `remaining < MIN_BLOCK_SIZE` 判定を
    // すり抜けて巨大な偽フリーブロックを stamp → MappingInsert が m_Blocks 配列外へ
    // wild write する (auditor HIGH 指摘、小サイズ領域の exact-fit alloc で到達)。
    const usize bs = BlockSize(block);
    if (bs < size || (bs - size) < kBlockHeaderOverhead + MIN_BLOCK_SIZE) {
        return;  // 切り出す余裕が無い (exact-fit / 僅差を含む) → 分割しない
    }
    usize remaining = bs - size - kBlockHeaderOverhead;
    if (remaining < MIN_BLOCK_SIZE) return;  // 念のため (上のガードで保証済み)

    SetBlockSize(block, size);
    // 残りを新ブロックとして構築 → フリーリストへ
    FBlockHeader* rem = NextBlock(block);
    rem->prev_phys_block = block;
    rem->size_and_flags = remaining | kBlockFreeBit;
    rem->next_free = nullptr;
    rem->prev_free = nullptr;
    InsertFreeBlock(rem);

    // 残ブロックの次に「prev はフリー」を伝達
    FBlockHeader* after = NextBlock(rem);
    after->prev_phys_block = rem;
    MarkPrevFree(after);
}

// 物理的に前のフリーブロックと統合
tlsf::FBlockHeader* FTlsfAllocator::MergePrev(tlsf::FBlockHeader* block) noexcept {
    using namespace tlsf;
    FBlockHeader* prev = block->prev_phys_block;
    RemoveFreeBlock(prev);  // 一旦リストから外す
    SetBlockSize(prev, BlockSize(prev) + BlockSize(block) + kBlockHeaderOverhead);
    FBlockHeader* after = NextBlock(prev);
    after->prev_phys_block = prev;
    return prev;
}

// 物理的に次のフリーブロックと統合
tlsf::FBlockHeader* FTlsfAllocator::MergeNext(tlsf::FBlockHeader* block) noexcept {
    using namespace tlsf;
    FBlockHeader* next = NextBlock(block);
    RemoveFreeBlock(next);
    SetBlockSize(block, BlockSize(block) + BlockSize(next) + kBlockHeaderOverhead);
    FBlockHeader* after = NextBlock(block);
    after->prev_phys_block = block;
    return block;
}

// 確保
void* FTlsfAllocator::Alloc(usize size, usize alignment, FSourceLoc /*loc*/) noexcept {
    using namespace tlsf;
    if (size == 0) return nullptr;
    if (alignment < ALIGN_SIZE) alignment = ALIGN_SIZE;

    // サイズをアライン → バケット検索
    usize adjust = AdjustRequestSize(size, alignment);

    // alignment > ALIGN_SIZE(16) の場合、chaining が保証する 16 整列では足りない。
    // 先頭に余白 (leading free ブロック) を切り出して payload を要求境界へ前進させる
    // memalign 経路を使う。そのぶん余白 + 最小 leading ブロックを上乗せして探索する。
    usize search_size = adjust;
    if (alignment > ALIGN_SIZE) {
        search_size = adjust + alignment + (kBlockHeaderOverhead + MIN_BLOCK_SIZE);
    }

    int fl, sl;
    MappingSearch(search_size, fl, sl);
    FBlockHeader* block = SearchSuitableBlock(fl, sl);
    if (!block || block == &m_NullBlock) return nullptr;  // OOM

    // 取り出す
    RemoveFreeBlock(block);

    // ---- over-alignment: 先頭ギャップを leading free ブロックとして切り出す ----
    if (alignment > ALIGN_SIZE) {
        u8* ptr     = static_cast<u8*>(BlockToPtr(block));
        u8* aligned = reinterpret_cast<u8*>(
            AlignUp(reinterpret_cast<uptr>(ptr), static_cast<uptr>(alignment)));
        usize gap = static_cast<usize>(aligned - ptr);  // ptr/aligned とも 16 整列 → gap は 16 の倍数
        if (gap != 0) {
            // leading ブロックは free pointer (next_free/prev_free) を収容できる全長が必要。
            // gap が小さすぎる場合は 1 アライメント分前進させて十分な leading 長を確保する。
            const usize min_lead = kBlockHeaderOverhead + MIN_BLOCK_SIZE;
            while (gap < min_lead) {
                aligned += alignment;
                gap     += alignment;
            }
            const usize bs = BlockSize(block);
            // leading: [block, block+gap)、remainder: block+gap 起点 (block_to_ptr = aligned)
            FBlockHeader* leading   = block;
            FBlockHeader* remainder = reinterpret_cast<FBlockHeader*>(
                reinterpret_cast<u8*>(block) + gap);
            SetBlockSize(leading, gap - kBlockHeaderOverhead);  // ≡8 mod16
            MarkFree(leading);
            remainder->prev_phys_block = leading;
            remainder->size_and_flags  = (bs - gap) | kBlockFreeBit | kPrevFreeBit;
            remainder->next_free = nullptr;
            remainder->prev_free = nullptr;
            // remainder の物理次ブロック (= 元の NextBlock) の prev リンクを張り直す
            FBlockHeader* after_rem = NextBlock(remainder);
            after_rem->prev_phys_block = remainder;
            // leading を free リストへ登録
            leading->next_free = nullptr;
            leading->prev_free = nullptr;
            InsertFreeBlock(leading);
            block = remainder;  // 以降は remainder を確保対象にする
        }
    }

    // 必要分に分割
    TrimFreeBlock(block, adjust);
    MarkUsed(block);
    FBlockHeader* after = NextBlock(block);
    MarkPrevUsed(after);  // 次ブロックに「前は使用中」を伝達

    // 統計更新
    m_BytesUsed += BlockSize(block);
    if (m_BytesUsed > m_BytesPeak) m_BytesPeak = m_BytesUsed;

    void* result = BlockToPtr(block);
    // payload は必ず要求 alignment を満たす (16 は chaining 不変条件、>16 は上の leading split)
    ACS_ASSERT((reinterpret_cast<uptr>(result) & (static_cast<uptr>(alignment) - 1)) == 0);
    return result;
}

// 解放
void FTlsfAllocator::Free(void* ptr) noexcept {
    using namespace tlsf;
    if (!ptr) return;
    FBlockHeader* block = PtrToBlock(ptr);
    m_BytesUsed -= BlockSize(block);

    MarkFree(block);
    // 隣接フリーブロックがあれば統合（外部断片化を防ぐ）
    if (IsPrevFree(block))               block = MergePrev(block);
    if (IsFree(NextBlock(block)))        block = MergeNext(block);
    InsertFreeBlock(block);

    // 次ブロックに「前はフリー」を伝達
    FBlockHeader* after = NextBlock(block);
    MarkPrevFree(after);
}

void* FTlsfAllocator::Realloc(void* ptr, usize old_size, usize new_size,
                             usize alignment, FSourceLoc loc) noexcept {
    return FAllocator::Realloc(ptr, old_size, new_size, alignment, loc);
}

FTlsfAllocator::Stats FTlsfAllocator::GetStats() const noexcept {
    Stats s {};
    s.bytes_used = m_BytesUsed;
    s.bytes_peak = m_BytesPeak;
    s.free_blocks = 0;
    s.used_blocks = 0;
    s.largest_free_block = 0;
    return s;
}

} // namespace acs
