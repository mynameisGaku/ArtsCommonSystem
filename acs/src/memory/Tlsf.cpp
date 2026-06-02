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

// メモリデバッグ機能 (free 後 poison / 深い検証アサート)。既定で Assert 有効ビルド
// (= 通常 Debug) のみ ON にし、Release ではゼロコスト。これは常時有効の軽量ガード
// (範囲検証 / 二重 free 検知) とは別レイヤで、開発時に UAF・破損を即検出するためのもの。
#ifndef ACS_MEMORY_DEBUG
#  define ACS_MEMORY_DEBUG ACS_ASSERTS_ENABLED
#endif

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
    m_CommittedBytes = commit_initial;   // 次の grow はこの offset から commit する
    return Ok();
}

// OOM 時に予約からコミットを段階的に伸ばし、needed_bytes のブロックを収容できる
// 新プールを追加する。幾何級数 (現コミットの 50%) で伸ばしてプール数を対数に抑える。
bool FTlsfAllocator::GrowToFit(usize needed_bytes) noexcept {
    using namespace tlsf;
    if (!m_bOwnsReservation) return false;            // 予約を所有しないプールは grow 不可
    const usize cap = m_Reservation.Capacity();
    if (m_CommittedBytes >= cap) return false;        // 予約を使い切った = 真の OOM
    const usize remaining = cap - m_CommittedBytes;

    // 新プールは needed_bytes のブロック + プールヘッダ/番兵を収容する必要がある。
    // さらに MappingSearch は要求を次のサブバケット境界まで切り上げてから探索するため、
    // 新プールの空きブロックは「切り上げ後サイズ」以上でないと見つからない (= O(1) 保証の代償)。
    // 切り上げ量は最大で needed_bytes / SL_INDEX_COUNT(=32) 程度。安全に needed_bytes/16 + 1page を
    // 上乗せして、必ず探索バケット以上のサイズになるようにする。
    const usize pool_overhead = kBlockStartOffset + kBlockHeaderOverhead * 2u + MIN_BLOCK_SIZE;
    const usize search_round  = (needed_bytes >> 4) + VmPageSize();
    const usize need = needed_bytes + search_round + pool_overhead;

    usize chunk = need;
    const usize geo = m_CommittedBytes / 2u;          // 幾何級数: 現コミットの 50%
    if (geo > chunk) chunk = geo;
    constexpr usize kMinGrowBytes = 1u * 1024u * 1024u;  // 1 MiB 下限 (小プール乱立を防ぐ)
    if (chunk < kMinGrowBytes) chunk = kMinGrowBytes;
    chunk = AlignUp(chunk, VmPageSize());             // Commit はページ単位
    if (chunk > remaining) chunk = remaining;          // 残予約でキャップ (最終 grow は残り全部)
    if (chunk < need) return false;                    // 残予約では要求を満たせない = OOM

    auto cr = m_Reservation.Commit(m_CommittedBytes, chunk);
    if (cr.IsErr()) return false;
    u8* base = static_cast<u8*>(m_Reservation.Base()) + m_CommittedBytes;
    auto ar = AddPool(base, chunk);
    if (ar.IsErr()) return false;                      // (commit は次回 grow で再利用される)
    m_CommittedBytes += chunk;
    return true;
}

// プールを TLSF に登録（先頭にフリーブロック 1 個 + 末尾に終端番兵）
TResult<void> FTlsfAllocator::AddPool(void* pool_base, usize pool_size) noexcept {
    using namespace tlsf;

    // pool_size を 16B 境界に切り下げ
    pool_size &= ~(usize(ALIGN_SIZE - 1));
    if (pool_size < MIN_BLOCK_SIZE + kBlockHeaderOverhead * 2)
        return ACS_ERR(Memory, 22, "AddPool too small");

    // Free 時の所属検証用にプール範囲 [base, base+size) を記録する。
    // 追跡上限を超えたら overflow フラグを立て、以後 範囲検証を無効化する(誤検出回避)。
    if (m_PoolSpanCount < kMaxTrackedPools) {
        m_PoolSpans[m_PoolSpanCount].lo = reinterpret_cast<uptr>(pool_base);
        m_PoolSpans[m_PoolSpanCount].hi = reinterpret_cast<uptr>(pool_base) + pool_size;
        ++m_PoolSpanCount;
    } else {
        m_PoolTrackOverflow = true;
    }

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

// used ブロックを size に縮め、余りを free ブロックとして解放する (in-place realloc 用)。
// TrimFreeBlock との違い: block は used のまま保持し、切り出した余り(tail)を「解放」する。
// tail の物理次が free なら統合する (used ブロックは free 隣接を持ち得るため必須)。
void FTlsfAllocator::TrimUsedBlock(tlsf::FBlockHeader* block, usize size) noexcept {
    using namespace tlsf;
    const usize bs = BlockSize(block);
    if (bs < size || (bs - size) < kBlockHeaderOverhead + MIN_BLOCK_SIZE) {
        return;  // 切り出す余裕が無い (僅差) → そのまま (over-allocation を許容)
    }
    const usize remaining = bs - size - kBlockHeaderOverhead;
    SetBlockSize(block, size);                 // block は used のまま (フラグ保持)
    FBlockHeader* rem = NextBlock(block);
    rem->prev_phys_block = block;
    // rem は free。prev(=block) は used なので prev_free は立てない。
    rem->size_and_flags = remaining | kBlockFreeBit;
    rem->next_free = nullptr;
    rem->prev_free = nullptr;
    // rem の物理次。free なら rem に統合して二重隣接 free を防ぐ。
    FBlockHeader* after = NextBlock(rem);
    after->prev_phys_block = rem;
    if (IsFree(after)) {
        rem = MergeNext(rem);                  // after を rem に吸収 (RemoveFreeBlock(after) 込み)
    }
    InsertFreeBlock(rem);
    FBlockHeader* a2 = NextBlock(rem);
    a2->prev_phys_block = rem;
    MarkPrevFree(a2);                          // a2 の prev(=rem) は free
}

// 安全上限。これを超える要求は内部のサイズ計算 (AlignUp / search_size) が
// オーバーフローして過小確保 → OOB を招くため、計算前に弾く。
static constexpr usize kMaxAllocSize = (~usize(0)) >> 1;   // アドレス空間の半分 (実用上無制限)
static constexpr usize kMaxAlignment = 64u * 1024u;        // 64KiB (VirtualAlloc 粒度)。これ以上は非現実的

// 確保
void* FTlsfAllocator::Alloc(usize size, usize alignment, FSourceLoc /*loc*/) noexcept {
    using namespace tlsf;
    if (size == 0) return nullptr;
    if (size > kMaxAllocSize) return nullptr;                 // オーバーフロー防止
    if (alignment < ALIGN_SIZE) alignment = ALIGN_SIZE;
    if (alignment > kMaxAlignment) return nullptr;            // 非現実的 alignment (search_size wrap 防止)
    if ((alignment & (alignment - 1u)) != 0u) return nullptr; // 2 のべき乗でない alignment は不正

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
    if (!block || block == &m_NullBlock) {
        // OOM: 予約からコミットを伸ばして新プールを足し、再探索する (auto-grow)。
        if (!GrowToFit(search_size)) return nullptr;  // 予約も尽きた = 真の OOM
        MappingSearch(search_size, fl, sl);
        block = SearchSuitableBlock(fl, sl);
        if (!block || block == &m_NullBlock) return nullptr;  // grow しても入らない (異常)
    }

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

// ptr がいずれかの登録プール範囲内か。overflow 時は全プールを追跡できていないので
// 検証不能 → true 扱いにして正当な Free を誤って弾かないようにする。
bool FTlsfAllocator::OwnsPointer(const void* p) const noexcept {
    if (m_PoolTrackOverflow) return true;
    const uptr a = reinterpret_cast<uptr>(p);
    for (int i = 0; i < m_PoolSpanCount; ++i) {
        if (a >= m_PoolSpans[i].lo && a < m_PoolSpans[i].hi) return true;
    }
    return false;
}

// 物理ブロックチェイン + prev_phys/prev_free フラグの一貫性を検証する。
bool FTlsfAllocator::ValidateHeap() const noexcept {
    using namespace tlsf;
    for (int i = 0; i < m_PoolSpanCount; ++i) {
        FBlockHeader* block = reinterpret_cast<FBlockHeader*>(m_PoolSpans[i].lo);
        const uptr hi = m_PoolSpans[i].hi;
        FBlockHeader* prev = nullptr;
        for (;;) {
            if (reinterpret_cast<uptr>(block) >= hi) return false;  // 番兵前に範囲外 = 破損
            const usize bs = BlockSize(block);
            if (bs == 0) break;                                     // 終端番兵 (size 0, used)
            if (prev != nullptr && block->prev_phys_block != prev) return false;  // 物理リンク不整合
            if (prev != nullptr && (IsPrevFree(block) != IsFree(prev))) return false; // フラグ不整合
            prev = block;
            block = NextBlock(block);
        }
    }
    return true;
}

// 解放
void FTlsfAllocator::Free(void* ptr) noexcept {
    using namespace tlsf;
    if (!ptr) return;

    // --- 常時有効の安全ガード (低コスト、Release でも動作) ---
    // (0) 全 payload は 16 整列なので、非整列ポインタは当アロケータ由来でない (内部/野良ポインタ)。
    //     PtrToBlock が壊れたヘッダを指してヒープを破壊する前に弾く。
    if ((reinterpret_cast<uptr>(ptr) & (ALIGN_SIZE - 1)) != 0) {
        ACS_ASSERT(false && "FTlsfAllocator::Free: 非整列ポインタ (当アロケータ由来でない)");
        return;
    }
    // (1) 所属プール範囲外 (野良 / 別アロケータ由来) のポインタを弾く。誤った Free が
    //     ヒープ構造を破壊するのを未然に防ぐ。Debug は assert で即検出、Release は安全に no-op。
    if (!OwnsPointer(ptr)) {
        ACS_ASSERT(false && "FTlsfAllocator::Free: ポインタが当アロケータの所有でない");
        return;
    }
    FBlockHeader* block = PtrToBlock(ptr);
    // (2) 二重 free 検出: 既に free 状態のブロックを再 free するとフリーリストが壊れる。
    if (IsFree(block)) {
        ACS_ASSERT(false && "FTlsfAllocator::Free: 二重 free を検出");
        return;
    }
    // (3) サイズ健全性: used ブロックのサイズは非 0 (0 は終端番兵/破損)。
    const usize bs = BlockSize(block);
    if (bs == 0) {
        ACS_ASSERT(false && "FTlsfAllocator::Free: 破損ブロック (size 0)");
        return;
    }

    m_BytesUsed -= bs;
    MarkFree(block);

#if ACS_MEMORY_DEBUG
    // UAF 検出: 解放した payload のうちフリーリストノード (先頭 16B = next_free/prev_free) を
    // 除く領域を 0xDD で塗る。解放後に読まれた場合に目立たせる (Debug のみ)。
    {
        u8* node_end = static_cast<u8*>(BlockToPtr(block)) + sizeof(FBlockHeader*) * 2;
        u8* end      = reinterpret_cast<u8*>(NextBlock(block));
        for (u8* q = node_end; q < end; ++q) *q = static_cast<u8>(0xDD);
    }
#endif

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
    using namespace tlsf;
    if (ptr == nullptr)   return Alloc(new_size, alignment, loc);
    if (new_size == 0)    { Free(ptr); return nullptr; }
    if (alignment < ALIGN_SIZE) alignment = ALIGN_SIZE;
    if (new_size > kMaxAllocSize || alignment > kMaxAlignment) return nullptr;

    // in-place 可否の前提: 当アロケータ所有 + 16 整列 + used ブロックであること。
    // 不正/解放済みポインタは旧領域を読まず/解放せず、新規確保のみで安全側に倒す。
    const bool ptr_ok = OwnsPointer(ptr) &&
                        ((reinterpret_cast<uptr>(ptr) & (ALIGN_SIZE - 1)) == 0);
    if (!ptr_ok) {
        ACS_ASSERT(false && "FTlsfAllocator::Realloc: 不正なポインタ");
        return Alloc(new_size, alignment, loc);
    }
    FBlockHeader* block = PtrToBlock(ptr);
    if (IsFree(block)) {
        ACS_ASSERT(false && "FTlsfAllocator::Realloc: 解放済みポインタ");
        return Alloc(new_size, alignment, loc);
    }

    // over-alignment 要求で現在の先頭が境界を満たさない場合は in-place できない → 移動。
    if (alignment > ALIGN_SIZE &&
        (reinterpret_cast<uptr>(ptr) & (static_cast<uptr>(alignment) - 1)) != 0) {
        return FAllocator::Realloc(ptr, old_size, new_size, alignment, loc);
    }

    const usize adjust = AdjustRequestSize(new_size, ALIGN_SIZE);
    const usize old_bs = BlockSize(block);

    if (adjust > old_bs) {
        // 拡大: 物理的に次のブロックが free で、合算サイズが要求を満たすなら in-place 統合。
        // そうでなければコピーを伴う移動 (新規確保 + memcpy + 解放) にフォールバック。
        FBlockHeader* next = NextBlock(block);
        if (!(IsFree(next) && (old_bs + BlockSize(next) + kBlockHeaderOverhead) >= adjust)) {
            return FAllocator::Realloc(ptr, old_size, new_size, alignment, loc);
        }
        block = MergeNext(block);            // block は used のまま next を吸収
        MarkPrevUsed(NextBlock(block));      // 統合後の次ブロックへ「prev(=block) は used」を伝達
    }

    // ここで BlockSize(block) >= adjust。余剰を切り出して解放する (コピー不要、ptr 不変)。
    TrimUsedBlock(block, adjust);

    // 統計更新 (used 総量の増減ぶん)。
    const usize new_bs = BlockSize(block);
    if (new_bs >= old_bs) {
        m_BytesUsed += (new_bs - old_bs);
        if (m_BytesUsed > m_BytesPeak) m_BytesPeak = m_BytesUsed;
    } else {
        m_BytesUsed -= (old_bs - new_bs);
    }
    return ptr;
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
