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
constexpr usize kBlockStartOffset = sizeof(BlockHeader*) + sizeof(usize);

// サイズ部分だけ取り出す
ACS_FORCEINLINE usize BlockSize(const BlockHeader* b) noexcept {
    return b->size_and_flags & kBlockSizeMask;
}
// フラグを保ちつつサイズだけ書き換える
ACS_FORCEINLINE void  SetBlockSize(BlockHeader* b, usize size) noexcept {
    usize old_flags = b->size_and_flags & ~kBlockSizeMask;
    b->size_and_flags = size | old_flags;
}
ACS_FORCEINLINE bool  IsFree(const BlockHeader* b) noexcept {
    return (b->size_and_flags & kBlockFreeBit) != 0;
}
ACS_FORCEINLINE bool  IsPrevFree(const BlockHeader* b) noexcept {
    return (b->size_and_flags & kPrevFreeBit) != 0;
}
ACS_FORCEINLINE void  MarkFree(BlockHeader* b) noexcept    { b->size_and_flags |= kBlockFreeBit; }
ACS_FORCEINLINE void  MarkUsed(BlockHeader* b) noexcept    { b->size_and_flags &= ~kBlockFreeBit; }
ACS_FORCEINLINE void  MarkPrevFree(BlockHeader* b) noexcept{ b->size_and_flags |= kPrevFreeBit; }
ACS_FORCEINLINE void  MarkPrevUsed(BlockHeader* b) noexcept{ b->size_and_flags &= ~kPrevFreeBit; }

// ヘッダから payload ポインタへ
ACS_FORCEINLINE void* BlockToPtr(const BlockHeader* b) noexcept {
    return reinterpret_cast<u8*>(const_cast<BlockHeader*>(b)) + kBlockStartOffset;
}
// payload からヘッダへ戻る
ACS_FORCEINLINE BlockHeader* PtrToBlock(const void* p) noexcept {
    return reinterpret_cast<BlockHeader*>(reinterpret_cast<u8*>(const_cast<void*>(p)) - kBlockStartOffset);
}

// 物理的に次のブロックを得る（解放時の隣接統合に使う）
ACS_FORCEINLINE BlockHeader* NextBlock(const BlockHeader* b) noexcept {
    return reinterpret_cast<BlockHeader*>(
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

// 要求サイズをアライン + 最小ブロック制約に揃える
ACS_FORCEINLINE usize AdjustRequestSize(usize size, usize align) noexcept {
    if (size == 0) return 0;
    usize adjust = AlignUp(size, align);
    if (adjust < MIN_BLOCK_SIZE) adjust = MIN_BLOCK_SIZE;
    return adjust;
}

} // namespace tlsf

// =============================================================================
// TlsfAllocator 実装
// =============================================================================

TlsfAllocator::~TlsfAllocator() noexcept {}

// 単一プール初期化
Result<void> TlsfAllocator::Init(void* pool_base, usize pool_size) noexcept {
    if (!pool_base || pool_size < 1024) return ACS_ERR(Memory, 20, "TLSF::Init invalid pool");
    if ((reinterpret_cast<uptr>(pool_base) & (tlsf::ALIGN_SIZE - 1)) != 0)
        return ACS_ERR(Memory, 21, "TLSF::Init pool not 16B aligned");

    // ビットマップとリストヘッドを 0 / 番兵で初期化
    _fl_bitmap = 0;
    for (int i = 0; i < tlsf::FL_INDEX_COUNT; ++i) {
        _sl_bitmap[i] = 0;
        for (int j = 0; j < tlsf::SL_INDEX_COUNT; ++j) {
            _blocks[i][j] = &_null_block;
        }
    }
    // 番兵ノードは自分を指す循環リスト（リスト末端の表現）
    _null_block.next_free = &_null_block;
    _null_block.prev_free = &_null_block;
    _null_block.size_and_flags = 0;
    _null_block.prev_phys_block = nullptr;

    return AddPool(pool_base, pool_size);
}

// VmReservation も保持する初期化
Result<void> TlsfAllocator::InitWithReservation(VmReservation&& reservation,
                                                usize commit_initial) noexcept {
    auto cr = reservation.Commit(0, commit_initial);
    if (cr.IsErr()) return cr;
    void* base = reservation.Base();
    auto r = Init(base, commit_initial);
    if (r.IsErr()) return r;
    _reservation = Move(reservation);
    _owns_reservation = true;
    return Ok();
}

// プールを TLSF に登録（先頭にフリーブロック 1 個 + 末尾に終端番兵）
Result<void> TlsfAllocator::AddPool(void* pool_base, usize pool_size) noexcept {
    using namespace tlsf;

    // pool_size を 16B 境界に切り下げ
    pool_size &= ~(usize(ALIGN_SIZE - 1));
    if (pool_size < MIN_BLOCK_SIZE + kBlockHeaderOverhead * 2)
        return ACS_ERR(Memory, 22, "AddPool too small");

    // プール全体を覆う巨大フリーブロックを生成
    BlockHeader* block = reinterpret_cast<BlockHeader*>(
        static_cast<u8*>(pool_base) - kBlockStartOffset + kBlockStartOffset);
    block->prev_phys_block = nullptr;
    usize block_size = pool_size - kBlockStartOffset - kBlockHeaderOverhead;
    block->size_and_flags = block_size | kBlockFreeBit;
    block->next_free = nullptr;
    block->prev_free = nullptr;
    InsertFreeBlock(block);

    // 末尾に「サイズ 0、used」の終端番兵を置く（隣接統合のループ終端）
    BlockHeader* sentinel = NextBlock(block);
    sentinel->prev_phys_block = block;
    sentinel->size_and_flags = 0;
    MarkPrevFree(sentinel);
    return Ok();
}

// 指定ブロックを (FL, SL) バケットの先頭に挿入
void TlsfAllocator::InsertFreeBlock(tlsf::BlockHeader* block) noexcept {
    using namespace tlsf;
    int fl, sl;
    MappingInsert(BlockSize(block), fl, sl);

    // 双方向リンクで先頭挿入
    BlockHeader* current = _blocks[fl][sl];
    block->next_free = current;
    block->prev_free = &_null_block;
    current->prev_free = block;
    _blocks[fl][sl] = block;

    // ビットマップに「このバケット非空」を立てる
    _fl_bitmap |= (1u << fl);
    _sl_bitmap[fl] |= (1u << sl);
}

// フリーリストから取り除く（バケットが空なら対応ビットも下ろす）
void TlsfAllocator::RemoveFreeBlock(tlsf::BlockHeader* block) noexcept {
    using namespace tlsf;
    BlockHeader* prev = block->prev_free;
    BlockHeader* next = block->next_free;
    next->prev_free = prev;
    prev->next_free = next;

    int fl, sl;
    MappingInsert(BlockSize(block), fl, sl);
    if (_blocks[fl][sl] == block) {
        _blocks[fl][sl] = next;
        // バケットが空になったらビットマップを下ろす
        if (next == &_null_block) {
            _sl_bitmap[fl] &= ~(1u << sl);
            if (_sl_bitmap[fl] == 0) {
                _fl_bitmap &= ~(1u << fl);
            }
        }
    }
}

// 要求サイズ以上のフリーブロックが入ったバケットを O(1) で見つける
tlsf::BlockHeader* TlsfAllocator::SearchSuitableBlock(int& fl, int& sl) noexcept {
    using namespace tlsf;
    // 同じ FL 内で sl 以上のビットを探す
    u32 sl_map = _sl_bitmap[fl] & (~0u << sl);
    if (sl_map == 0) {
        // 見つからなければ FL を 1 つ上にずらす
        u32 fl_map = _fl_bitmap & (~0u << (fl + 1));
        if (fl_map == 0) return nullptr;  // OOM
        fl = Ffs(fl_map);
        sl_map = _sl_bitmap[fl];
    }
    sl = Ffs(sl_map);
    return _blocks[fl][sl];
}

// 余剰サイズを切り出して新しいフリーブロックとして再登録
void TlsfAllocator::TrimFreeBlock(tlsf::BlockHeader* block, usize size) noexcept {
    using namespace tlsf;
    usize remaining = BlockSize(block) - size - kBlockHeaderOverhead;
    if (remaining < MIN_BLOCK_SIZE) return;  // 切り出す余裕がないなら何もしない

    SetBlockSize(block, size);
    // 残りを新ブロックとして構築 → フリーリストへ
    BlockHeader* rem = NextBlock(block);
    rem->prev_phys_block = block;
    rem->size_and_flags = remaining | kBlockFreeBit;
    rem->next_free = nullptr;
    rem->prev_free = nullptr;
    InsertFreeBlock(rem);

    // 残ブロックの次に「prev はフリー」を伝達
    BlockHeader* after = NextBlock(rem);
    after->prev_phys_block = rem;
    MarkPrevFree(after);
}

// 物理的に前のフリーブロックと統合
tlsf::BlockHeader* TlsfAllocator::MergePrev(tlsf::BlockHeader* block) noexcept {
    using namespace tlsf;
    BlockHeader* prev = block->prev_phys_block;
    RemoveFreeBlock(prev);  // 一旦リストから外す
    SetBlockSize(prev, BlockSize(prev) + BlockSize(block) + kBlockHeaderOverhead);
    BlockHeader* after = NextBlock(prev);
    after->prev_phys_block = prev;
    return prev;
}

// 物理的に次のフリーブロックと統合
tlsf::BlockHeader* TlsfAllocator::MergeNext(tlsf::BlockHeader* block) noexcept {
    using namespace tlsf;
    BlockHeader* next = NextBlock(block);
    RemoveFreeBlock(next);
    SetBlockSize(block, BlockSize(block) + BlockSize(next) + kBlockHeaderOverhead);
    BlockHeader* after = NextBlock(block);
    after->prev_phys_block = block;
    return block;
}

// 確保
void* TlsfAllocator::Alloc(usize size, usize alignment, SourceLoc /*loc*/) noexcept {
    using namespace tlsf;
    if (size == 0) return nullptr;
    if (alignment < ALIGN_SIZE) alignment = ALIGN_SIZE;

    // サイズをアライン → バケット検索
    usize adjust = AdjustRequestSize(size, alignment);
    int fl, sl;
    MappingSearch(adjust, fl, sl);
    BlockHeader* block = SearchSuitableBlock(fl, sl);
    if (!block || block == &_null_block) return nullptr;  // OOM

    // 取り出して必要分に分割
    RemoveFreeBlock(block);
    TrimFreeBlock(block, adjust);
    MarkUsed(block);
    BlockHeader* after = NextBlock(block);
    MarkPrevUsed(after);  // 次ブロックに「前は使用中」を伝達

    // 統計更新
    _bytes_used += BlockSize(block);
    if (_bytes_used > _bytes_peak) _bytes_peak = _bytes_used;
    return BlockToPtr(block);
}

// 解放
void TlsfAllocator::Free(void* ptr) noexcept {
    using namespace tlsf;
    if (!ptr) return;
    BlockHeader* block = PtrToBlock(ptr);
    _bytes_used -= BlockSize(block);

    MarkFree(block);
    // 隣接フリーブロックがあれば統合（外部断片化を防ぐ）
    if (IsPrevFree(block))               block = MergePrev(block);
    if (IsFree(NextBlock(block)))        block = MergeNext(block);
    InsertFreeBlock(block);

    // 次ブロックに「前はフリー」を伝達
    BlockHeader* after = NextBlock(block);
    MarkPrevFree(after);
}

void* TlsfAllocator::Realloc(void* ptr, usize old_size, usize new_size,
                             usize alignment, SourceLoc loc) noexcept {
    return Allocator::Realloc(ptr, old_size, new_size, alignment, loc);
}

TlsfAllocator::Stats TlsfAllocator::GetStats() const noexcept {
    Stats s {};
    s.bytes_used = _bytes_used;
    s.bytes_peak = _bytes_peak;
    s.free_blocks = 0;
    s.used_blocks = 0;
    s.largest_free_block = 0;
    return s;
}

} // namespace acs
