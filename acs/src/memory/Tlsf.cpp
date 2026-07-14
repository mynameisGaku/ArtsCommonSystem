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
#include "foundation/Platform.h"
#include "foundation/Move.h"
#include "threading/Atomic.h"

#include <intrin.h>

// メモリデバッグ機能 (free 後 poison / 深い検証アサート)。既定で Assert 有効ビルド
// (= 通常 Debug) のみ ON にし、Release ではゼロコスト。これは常時有効の軽量ガード
// (範囲検証 / 二重 free 検知) とは別レイヤで、開発時に UAF・破損を即検出するためのもの。
#ifndef ACS_MEMORY_DEBUG
#    define ACS_MEMORY_DEBUG ACS_ASSERTS_ENABLED
#endif

namespace acs {

namespace tlsf {

/**
 * 最上位の立っているビット位置を返す (32bit、log2 相当)。
 *
 * @param v 入力値。
 * @return 最上位ビットの位置 (v==0 なら -1)。
 */
ACS_FORCEINLINE int Fls(u32 v) noexcept
{
    if (v == 0) return -1;
    unsigned long Index;
    _BitScanReverse(&Index, v);
    return static_cast<int>(Index);
}

/**
 * 最下位の立っているビット位置を返す (32bit)。
 *
 * @param v 入力値。
 * @return 最下位ビットの位置 (v==0 なら -1)。
 */
ACS_FORCEINLINE int Ffs(u32 v) noexcept
{
    if (v == 0) return -1;
    unsigned long Index;
    _BitScanForward(&Index, v);
    return static_cast<int>(Index);
}

/**
 * 最上位の立っているビット位置を返す (usize 幅、log2 相当)。
 *
 * @param v 入力値。
 * @return 最上位ビットの位置 (v==0 なら -1)。
 */
ACS_FORCEINLINE int FlsSize(usize v) noexcept
{
    if (v == 0) return -1;
    unsigned long Index;
#if ACS_ARCH_X64
    _BitScanReverse64(&Index, static_cast<unsigned __int64>(v));
#else
    _BitScanReverse(&Index, static_cast<unsigned long>(v));
#endif
    return static_cast<int>(Index);
}

/** size_and_flags の bit0: このブロックがフリーであることを示す。 */
constexpr usize kBlockFreeBit = 1ull << 0;

/** size_and_flags の bit1: 物理的に前のブロックがフリーであることを示す。 */
constexpr usize kPrevFreeBit = 1ull << 1;

/** size_and_flags の bit2: thread-local キャッシュが再利用待ちで保持していることを示す。 */
constexpr usize kThreadCacheBit = 1ull << 2;

/** size_and_flags からサイズ部だけ取り出すマスク (下位 3bit を除く)。 */
constexpr usize kBlockSizeMask = ~(kBlockFreeBit | kPrevFreeBit | kThreadCacheBit);

/** ブロックヘッダのうち、次ブロックと重複しない実オーバーヘッド (size_and_flags 分)。 */
constexpr usize kBlockHeaderOverhead = sizeof(usize);

/** ヘッダ先頭から payload 起点までのオフセット。 */
constexpr usize kBlockStartOffset = sizeof(FBlockHeader*) + sizeof(usize);

/**
 * ブロックのサイズ部を取り出す。
 *
 * @param b 対象ブロック。
 * @return フラグを除いたブロックサイズ。
 */
ACS_FORCEINLINE usize BlockSize(const FBlockHeader* b) noexcept
{
    const volatile usize* const State = reinterpret_cast<const volatile usize*>(&b->size_and_flags);
    return atomic_detail::LoadAcquire(State) & kBlockSizeMask;
}

/**
 * フラグを保ったままブロックのサイズ部だけ書き換える。
 *
 * @param b 対象ブロック。
 * @param Size 新しいブロックサイズ。
 */
ACS_FORCEINLINE void SetBlockSize(FBlockHeader* b, usize Size) noexcept
{
    const usize OldFlags = b->size_and_flags & ~kBlockSizeMask;
    b->size_and_flags = Size | OldFlags;
}

/**
 * ブロックがフリーかを返す。
 *
 * @param b 対象ブロック。
 * @return フリーなら true。
 */
ACS_FORCEINLINE bool IsFree(const FBlockHeader* b) noexcept
{
    const volatile usize* const State = reinterpret_cast<const volatile usize*>(&b->size_and_flags);
    return (atomic_detail::LoadAcquire(State) & kBlockFreeBit) != 0;
}

/**
 * 物理的に前のブロックがフリーかを返す。
 *
 * @param b 対象ブロック。
 * @return 前ブロックがフリーなら true。
 */
ACS_FORCEINLINE bool IsPrevFree(const FBlockHeader* b) noexcept
{
    const volatile usize* const State = reinterpret_cast<const volatile usize*>(&b->size_and_flags);
    return (atomic_detail::LoadAcquire(State) & kPrevFreeBit) != 0;
}

/**
 * ブロックが thread-local キャッシュに保管中かを返す。
 *
 * @param b 対象ブロック。
 * @return キャッシュ保管中なら true。
 */
ACS_FORCEINLINE bool IsThreadCached(const FBlockHeader* b) noexcept
{
    const volatile usize* const State = reinterpret_cast<const volatile usize*>(&b->size_and_flags);
    return (atomic_detail::LoadAcquire(State) & kThreadCacheBit) != 0;
}

/**
 * ブロックにフリーフラグを立てる。
 *
 * @param b 対象ブロック。
 */
ACS_FORCEINLINE void MarkFree(FBlockHeader* b) noexcept
{
    b->size_and_flags |= kBlockFreeBit;
}

/**
 * ブロックのフリーフラグを下ろす (使用中にする)。
 *
 * @param b 対象ブロック。
 */
ACS_FORCEINLINE void MarkUsed(FBlockHeader* b) noexcept
{
    b->size_and_flags &= ~kBlockFreeBit;
}

/**
 * 「前ブロックはフリー」フラグを立てる。
 *
 * @param b 対象ブロック。
 */
ACS_FORCEINLINE void MarkPrevFree(FBlockHeader* b) noexcept
{
    volatile usize* const State = reinterpret_cast<volatile usize*>(&b->size_and_flags);
    (void)atomic_detail::FetchOr(State, kPrevFreeBit);
}

/**
 * 「前ブロックはフリー」フラグを下ろす。
 *
 * @param b 対象ブロック。
 */
ACS_FORCEINLINE void MarkPrevUsed(FBlockHeader* b) noexcept
{
    volatile usize* const State = reinterpret_cast<volatile usize*>(&b->size_and_flags);
    (void)atomic_detail::FetchAnd(State, ~kPrevFreeBit);
}

/**
 * ブロックヘッダから payload ポインタへ変換する。
 *
 * @param b 対象ブロック。
 * @return payload 先頭ポインタ。
 */
ACS_FORCEINLINE void* BlockToPtr(const FBlockHeader* b) noexcept
{
    return reinterpret_cast<u8*>(const_cast<FBlockHeader*>(b)) + kBlockStartOffset;
}

/**
 * payload ポインタからブロックヘッダへ戻る。
 *
 * @param p payload ポインタ。
 * @return 対応するブロックヘッダ。
 */
ACS_FORCEINLINE FBlockHeader* PtrToBlock(const void* p) noexcept
{
    return reinterpret_cast<FBlockHeader*>(reinterpret_cast<u8*>(const_cast<void*>(p)) - kBlockStartOffset);
}

/**
 * 物理的に次のブロックを得る (隣接統合に使う)。
 *
 * @param b 対象ブロック。
 * @return 物理的に直後のブロック。
 */
ACS_FORCEINLINE FBlockHeader* NextBlock(const FBlockHeader* b) noexcept
{
    return reinterpret_cast<FBlockHeader*>(reinterpret_cast<u8*>(BlockToPtr(b)) + BlockSize(b) - kBlockHeaderOverhead);
}

/**
 * サイズから挿入先の (FL, SL) インデックスを計算する。
 *
 * @details 小サイズは線形領域に均等割り、それ以外は log2 + 上位ビット切り出しで対数的に分配する。
 * @param Size 対象サイズ。
 * @param fl 第 1 レベルインデックスを返す。
 * @param sl 第 2 レベルインデックスを返す。
 */
ACS_FORCEINLINE void MappingInsert(usize Size, int& fl, int& sl) noexcept
{
    if (Size < SMALL_BLOCK_SIZE) {
        // 小サイズは線形領域に均等割り
        fl = 0;
        sl = static_cast<int>(Size) / (SMALL_BLOCK_SIZE / SL_INDEX_COUNT);
    } else {
        // 通常領域は log2 + 上位ビット切り出しで対数的に分配
        fl = FlsSize(Size);
        sl = static_cast<int>((Size >> (fl - SL_INDEX_LOG2)) ^ (1u << SL_INDEX_LOG2));
        fl -= (FL_INDEX_SHIFT - 1);
        // 破損メタデータでも配列外へ進まない最終防壁。通常経路は AddPool と Alloc が
        // 4GiB の表現上限を事前検証するため、この分岐へ到達しない。
        ACS_ASSERT(fl < FL_INDEX_COUNT);
        if (fl >= FL_INDEX_COUNT) {
            fl = FL_INDEX_COUNT - 1;
            sl = SL_INDEX_COUNT - 1;
        }
    }
}

/**
 * 確保探索用に、要求サイズを次のサブバケット境界まで切り上げて (FL, SL) を求める。
 *
 * @details 切り上げにより、選んだバケット内のどのブロックでも要求を満たせることを保証する (O(1) の代償)。
 * @param Size 要求サイズ。
 * @param fl 探索開始の第 1 レベルインデックスを返す。
 * @param sl 探索開始の第 2 レベルインデックスを返す。
 */
ACS_FORCEINLINE void MappingSearch(usize Size, int& fl, int& sl) noexcept
{
    if (Size >= SMALL_BLOCK_SIZE) {
        const usize Round = (1ull << (FlsSize(Size) - SL_INDEX_LOG2)) - 1;
        Size += Round;
    }
    MappingInsert(Size, fl, sl);
}

/**
 * 要求サイズを 16 整列を保つ block_size へ変換する。
 *
 * @details
 * 重要な不変条件: 連続するブロックヘッダの間隔は block_size + kBlockHeaderOverhead。
 * 先頭ブロックは 16 整列、payload は header + kBlockStartOffset(=16) なので、すべての
 * payload を 16 整列に保つには全ブロックヘッダが 16 整列である必要があり、それには
 * (block_size + kBlockHeaderOverhead) ≡ 0 (mod 16)、すなわち block_size ≡ 8 (mod 16)
 * でなければならない。AlignUp(size,16) は ≡0 で連鎖の途中で payload が 8 整列に落ちるため、
 * +kBlockHeaderOverhead して ≡8 に補正する。free 時に next_free/prev_free を収容できる
 * 最小ブロック (≡8) へクランプもする。
 * @param Size 要求サイズ (0 なら 0 を返す)。
 * @return 16 整列を保つブロックサイズ (使用可能領域 >= size を必ず満たす)。
 */
ACS_FORCEINLINE usize AdjustRequestSize(usize Size, usize /*Alignment*/) noexcept
{
    if (Size == 0) return 0;
    // AlignUp(size,16) (>= size, ≡0 mod16) に overhead を足して ≡8 mod16 にする。
    // 結果として使用可能領域 block_size - overhead >= size を必ず満たす。
    usize AdjustedSize = AlignUp(Size, ALIGN_SIZE) + kBlockHeaderOverhead;
    // ≡8 を保った最小ブロック (free 時に next_free/prev_free を収容できる) へクランプ。
    constexpr usize kMinBlock8 = MIN_BLOCK_SIZE - kBlockHeaderOverhead; // 32-8 = 24, ≡8 mod16
    if (AdjustedSize < kMinBlock8) AdjustedSize = kMinBlock8;
    return AdjustedSize;
}

} // namespace tlsf

FTlsfAllocator::~FTlsfAllocator() noexcept
{
    // ここで失敗しても m_Reservation のデストラクタが同じ所有状態から再試行する。
    if (Reset().IsErr()) {
        // オブジェクト破棄では再試行 API を呼べないため、外部境界表だけは確実に回収する。
        // VM は直後の VmReservation デストラクタが同じ所有状態から再度 Release する。
        for (int Index = 0; Index < m_PoolSpanCount; ++Index) {
            if (m_PoolSpans[Index].allocation_start_bitmap) {
                (void)::HeapFree(::GetProcessHeap(), 0, m_PoolSpans[Index].allocation_start_bitmap);
            }
            m_PoolSpans[Index] = {};
        }
        m_PoolSpanCount = 0;
    }
}

// 単一プール初期化
TResult<void> FTlsfAllocator::Init(void* PoolBase, usize PoolSize) noexcept
{
    if (m_Initialized) return ACS_ERR(Memory, 25, "TLSF::Init already initialized");
    if (!PoolBase || PoolSize < 1024) return ACS_ERR(Memory, 20, "TLSF::Init invalid pool");
    if ((reinterpret_cast<uptr>(PoolBase) & (tlsf::ALIGN_SIZE - 1)) != 0)
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

    m_BytesUsed = 0;
    m_BytesPeak = 0;
    m_AllocationCount = 0;
    for (int Index = 0; Index < m_PoolSpanCount; ++Index) {
        if (m_PoolSpans[Index].allocation_start_bitmap) {
            (void)::HeapFree(::GetProcessHeap(), 0, m_PoolSpans[Index].allocation_start_bitmap);
        }
        m_PoolSpans[Index] = {};
    }
    m_PoolSpanCount = 0;

    m_Initialized = true;
    TResult<void> AddPoolResult = AddPool(PoolBase, PoolSize);
    if (AddPoolResult.IsErr()) {
        m_Initialized = false;
        return AddPoolResult;
    }
    return Ok();
}

// VmReservation も保持する初期化
TResult<void> FTlsfAllocator::InitWithReservation(VmReservation&& Reservation, usize InitialCommitBytes) noexcept
{
    if (m_Initialized) return ACS_ERR(Memory, 25, "TLSF::InitWithReservation already initialized");
    auto CommitResult = Reservation.Commit(0, InitialCommitBytes);
    if (CommitResult.IsErr()) return CommitResult;
    void* const Base = Reservation.Base();
    auto InitResult = Init(Base, InitialCommitBytes);
    if (InitResult.IsErr()) return InitResult;
    m_Reservation = Move(Reservation);
    m_bOwnsReservation = true;
    m_CommittedBytes = InitialCommitBytes; // 次の grow はこの offset から commit する
    return Ok();
}

// OOM 時に予約からコミットを段階的に伸ばし、needed_bytes のブロックを収容できる
// 新プールを追加する。幾何級数 (現コミットの 50%) で伸ばしてプール数を対数に抑える。
bool FTlsfAllocator::GrowToFit(usize NeededBytes) noexcept
{
    using namespace tlsf;
    if (!m_bOwnsReservation) return false; // 予約を所有しないプールは grow 不可
    if (NeededBytes > MAXIMUM_SEARCHABLE_BLOCK_SIZE) return false;
    const usize Capacity = m_Reservation.Capacity();
    if (m_CommittedBytes >= Capacity) return false; // 予約を使い切った = 真の OOM
    const usize RemainingBytes = Capacity - m_CommittedBytes;

    // 新プールは needed_bytes のブロック + プールヘッダ/番兵を収容する必要がある。
    // さらに MappingSearch は要求を次のサブバケット境界まで切り上げてから探索するため、
    // 新プールの空きブロックは「切り上げ後サイズ」以上でないと見つからない (= O(1) 保証の代償)。
    // 切り上げ量は最大で needed_bytes / SL_INDEX_COUNT(=32) 程度。安全に needed_bytes/16 + 1page を
    // 上乗せして、必ず探索バケット以上のサイズになるようにする。
    const usize PoolOverhead = kBlockStartOffset + kBlockHeaderOverhead * 2u + MIN_BLOCK_SIZE;
    const usize SearchRound = (NeededBytes >> 4) + VmPageSize();
    const usize MaximumValue = ~usize(0);
    if (SearchRound > MaximumValue - NeededBytes || PoolOverhead > MaximumValue - NeededBytes - SearchRound) {
        return false;
    }
    const usize RequiredBytes = NeededBytes + SearchRound + PoolOverhead;

    usize ChunkBytes = RequiredBytes;
    const usize GeometricGrowth = m_CommittedBytes / 2u; // 幾何級数: 現コミットの 50%
    if (GeometricGrowth > ChunkBytes) ChunkBytes = GeometricGrowth;
    constexpr usize kMinGrowBytes = 1u * 1024u * 1024u; // 1 MiB 下限 (小プール乱立を防ぐ)
    if (ChunkBytes < kMinGrowBytes) ChunkBytes = kMinGrowBytes;

    // 1プールのブロックがFL表現上限を越えないよう、大きな予約は複数プールへ分割する。
    const usize PoolMetadataBytes = kBlockStartOffset + kBlockHeaderOverhead;
    const usize MaximumPoolBytes = MAXIMUM_BLOCK_SIZE > MaximumValue - PoolMetadataBytes
                                       ? MaximumValue
                                       : MAXIMUM_BLOCK_SIZE + PoolMetadataBytes;
    const usize PageSize = VmPageSize();
    const usize MaximumPoolChunk = MaximumPoolBytes & ~(PageSize - 1u);
    if (ChunkBytes > MaximumPoolChunk) ChunkBytes = MaximumPoolChunk;
    ChunkBytes = AlignUp(ChunkBytes, PageSize);                   // Commit はページ単位
    if (ChunkBytes > RemainingBytes) ChunkBytes = RemainingBytes; // 残予約でキャップ (最終 grow は残り全部)
    if (ChunkBytes < RequiredBytes) return false;                 // 残予約では要求を満たせない = OOM

    const auto CommitResult = m_Reservation.Commit(m_CommittedBytes, ChunkBytes);
    if (CommitResult.IsErr()) return false;
    u8* const Base = static_cast<u8*>(m_Reservation.Base()) + m_CommittedBytes;
    const auto AddPoolResult = AddPool(Base, ChunkBytes);
    if (AddPoolResult.IsErr()) return false; // (commit は次回 grow で再利用される)
    m_CommittedBytes += ChunkBytes;
    return true;
}

// プールを TLSF に登録（先頭にフリーブロック 1 個 + 末尾に終端番兵）
TResult<void> FTlsfAllocator::AddPool(void* PoolBase, usize PoolSize) noexcept
{
    using namespace tlsf;

    if (!m_Initialized) {
        return ACS_ERR(Memory, 28, "AddPool requires an initialized TLSF allocator");
    }
    if (!PoolBase || (reinterpret_cast<uptr>(PoolBase) & (ALIGN_SIZE - 1u)) != 0u) {
        return ACS_ERR(Memory, 23, "AddPool base must be 16-byte aligned");
    }

    // pool_size を 16B 境界に切り下げ
    PoolSize &= ~(usize(ALIGN_SIZE - 1));
    if (PoolSize < MIN_BLOCK_SIZE + kBlockHeaderOverhead * 2) return ACS_ERR(Memory, 22, "AddPool too small");

    const usize InitialBlockSize = PoolSize - kBlockStartOffset - kBlockHeaderOverhead;
    const uptr PoolAddress = reinterpret_cast<uptr>(PoolBase);
    if (InitialBlockSize > MAXIMUM_BLOCK_SIZE || PoolSize > (~uptr(0)) - PoolAddress) {
        return ACS_ERR(Memory, 24, "AddPool exceeds TLSF block representation");
    }
    const uptr PoolEnd = PoolAddress + PoolSize;
    if (m_PoolSpanCount >= kMaxTrackedPools) {
        return ACS_ERR(Memory, 26, "AddPool tracking capacity exceeded");
    }
    for (int Index = 0; Index < m_PoolSpanCount; ++Index) {
        const PoolSpan& ExistingSpan = m_PoolSpans[Index];
        if (PoolAddress < ExistingSpan.hi && ExistingSpan.lo < PoolEnd) {
            return ACS_ERR(Memory, 27, "AddPool overlaps an existing pool");
        }
    }

    // payload 先頭候補は16バイト量子なので、1量子1bitの外部表で正規境界を証明する。
    // ユーザー領域外へ置くことで、payload 内に偽ヘッダを作られても所有判定を偽装できない。
    const usize AllocationSlotCount = PoolSize / ALIGN_SIZE;
    const usize AllocationBitmapBytes = (AllocationSlotCount + 7u) / 8u;
    auto* const AllocationBitmap = static_cast<u8*>(
        ::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, AllocationBitmapBytes));
    if (!AllocationBitmap) {
        return ACS_ERR(Memory, 29, "AddPool allocation-start bitmap allocation failed");
    }

    // Free 時の所属検証用にプール範囲 [base, base+size) と境界表を記録する。
    m_PoolSpans[m_PoolSpanCount].lo = PoolAddress;
    m_PoolSpans[m_PoolSpanCount].hi = PoolEnd;
    m_PoolSpans[m_PoolSpanCount].allocation_start_bitmap = AllocationBitmap;
    m_PoolSpans[m_PoolSpanCount].allocation_start_bitmap_bytes = AllocationBitmapBytes;
    ++m_PoolSpanCount;

    // プール全体を覆う巨大フリーブロックを生成
    FBlockHeader* Block = reinterpret_cast<FBlockHeader*>(static_cast<u8*>(PoolBase) - kBlockStartOffset +
                                                          kBlockStartOffset);
    Block->prev_phys_block = nullptr;
    Block->size_and_flags = InitialBlockSize | kBlockFreeBit;
    Block->next_free = nullptr;
    Block->prev_free = nullptr;
    InsertFreeBlock(Block);

    // 末尾に「サイズ 0、used」の終端番兵を置く（隣接統合のループ終端）
    FBlockHeader* Sentinel = NextBlock(Block);
    Sentinel->prev_phys_block = Block;
    Sentinel->size_and_flags = 0;
    MarkPrevFree(Sentinel);
    return Ok();
}

// 指定ブロックを (FL, SL) バケットの先頭に挿入
void FTlsfAllocator::InsertFreeBlock(tlsf::FBlockHeader* Block) noexcept
{
    using namespace tlsf;
    int fl, sl;
    MappingInsert(BlockSize(Block), fl, sl);

    // 双方向リンクで先頭挿入
    FBlockHeader* const CurrentHead = m_Blocks[fl][sl];
    Block->next_free = CurrentHead;
    Block->prev_free = &m_NullBlock;
    CurrentHead->prev_free = Block;
    m_Blocks[fl][sl] = Block;

    // ビットマップに「このバケット非空」を立てる
    m_FlBitmap |= (1u << fl);
    m_SlBitmap[fl] |= (1u << sl);
}

// フリーリストから取り除く（バケットが空なら対応ビットも下ろす）
void FTlsfAllocator::RemoveFreeBlock(tlsf::FBlockHeader* Block) noexcept
{
    using namespace tlsf;
    FBlockHeader* const PreviousBlock = Block->prev_free;
    FBlockHeader* const NextFreeBlock = Block->next_free;
    NextFreeBlock->prev_free = PreviousBlock;
    PreviousBlock->next_free = NextFreeBlock;

    int fl, sl;
    MappingInsert(BlockSize(Block), fl, sl);
    if (m_Blocks[fl][sl] == Block) {
        m_Blocks[fl][sl] = NextFreeBlock;
        // バケットが空になったらビットマップを下ろす
        if (NextFreeBlock == &m_NullBlock) {
            m_SlBitmap[fl] &= ~(1u << sl);
            if (m_SlBitmap[fl] == 0) {
                m_FlBitmap &= ~(1u << fl);
            }
        }
    }
}

// 要求サイズ以上のフリーブロックが入ったバケットを O(1) で見つける
tlsf::FBlockHeader* FTlsfAllocator::SearchSuitableBlock(int& fl, int& sl) noexcept
{
    using namespace tlsf;
    // 同じ FL 内で sl 以上のビットを探す
    u32 SlMap = m_SlBitmap[fl] & (~0u << sl);
    if (SlMap == 0) {
        // 見つからなければ FL を 1 つ上にずらす
        const u32 FlMap = m_FlBitmap & (~0u << (fl + 1));
        if (FlMap == 0) return nullptr; // OOM
        fl = Ffs(FlMap);
        SlMap = m_SlBitmap[fl];
    }
    sl = Ffs(SlMap);
    return m_Blocks[fl][sl];
}

// 余剰サイズを切り出して新しいフリーブロックとして再登録
void FTlsfAllocator::TrimFreeBlock(tlsf::FBlockHeader* Block, usize Size) noexcept
{
    using namespace tlsf;
    // **引き算の前に比較形でガードする** (canonical TLSF の block_can_split)。
    // exact-fit (BlockSize==size) や僅差では `BlockSize - size - overhead` が
    // unsigned で wrap (~2^64) し、後段の `remaining < MIN_BLOCK_SIZE` 判定を
    // すり抜けて巨大な偽フリーブロックを stamp → MappingInsert が m_Blocks 配列外へ
    // wild write する (auditor HIGH 指摘、小サイズ領域の exact-fit alloc で到達)。
    const usize OriginalBlockSize = BlockSize(Block);
    if (OriginalBlockSize < Size || (OriginalBlockSize - Size) < kBlockHeaderOverhead + MIN_BLOCK_SIZE) {
        return; // 切り出す余裕が無い (exact-fit / 僅差を含む) → 分割しない
    }
    const usize RemainingSize = OriginalBlockSize - Size - kBlockHeaderOverhead;
    if (RemainingSize < MIN_BLOCK_SIZE) return; // 念のため (上のガードで保証済み)

    SetBlockSize(Block, Size);
    // 残りを新ブロックとして構築 → フリーリストへ
    FBlockHeader* Remainder = NextBlock(Block);
    Remainder->prev_phys_block = Block;
    Remainder->size_and_flags = RemainingSize | kBlockFreeBit;
    Remainder->next_free = nullptr;
    Remainder->prev_free = nullptr;
    InsertFreeBlock(Remainder);

    // 残ブロックの次に「prev はフリー」を伝達
    FBlockHeader* FollowingBlock = NextBlock(Remainder);
    FollowingBlock->prev_phys_block = Remainder;
    MarkPrevFree(FollowingBlock);
}

// 物理的に前のフリーブロックと統合
tlsf::FBlockHeader* FTlsfAllocator::MergePrev(tlsf::FBlockHeader* Block) noexcept
{
    using namespace tlsf;
    FBlockHeader* PreviousBlock = Block->prev_phys_block;
    RemoveFreeBlock(PreviousBlock); // 一旦リストから外す
    SetBlockSize(PreviousBlock, BlockSize(PreviousBlock) + BlockSize(Block) + kBlockHeaderOverhead);
    FBlockHeader* FollowingBlock = NextBlock(PreviousBlock);
    FollowingBlock->prev_phys_block = PreviousBlock;
    return PreviousBlock;
}

// 物理的に次のフリーブロックと統合
tlsf::FBlockHeader* FTlsfAllocator::MergeNext(tlsf::FBlockHeader* Block) noexcept
{
    using namespace tlsf;
    FBlockHeader* NextFreeBlock = NextBlock(Block);
    RemoveFreeBlock(NextFreeBlock);
    SetBlockSize(Block, BlockSize(Block) + BlockSize(NextFreeBlock) + kBlockHeaderOverhead);
    FBlockHeader* FollowingBlock = NextBlock(Block);
    FollowingBlock->prev_phys_block = Block;
    return Block;
}

// used ブロックを size に縮め、余りを free ブロックとして解放する (in-place realloc 用)。
// TrimFreeBlock との違い: block は used のまま保持し、切り出した余り(tail)を「解放」する。
// tail の物理次が free なら統合する (used ブロックは free 隣接を持ち得るため必須)。
void FTlsfAllocator::TrimUsedBlock(tlsf::FBlockHeader* Block, usize Size) noexcept
{
    using namespace tlsf;
    const usize OriginalBlockSize = BlockSize(Block);
    if (OriginalBlockSize < Size || (OriginalBlockSize - Size) < kBlockHeaderOverhead + MIN_BLOCK_SIZE) {
        return; // 切り出す余裕が無い (僅差) → そのまま (over-allocation を許容)
    }
    const usize RemainingSize = OriginalBlockSize - Size - kBlockHeaderOverhead;
    SetBlockSize(Block, Size); // block は used のまま (フラグ保持)
    FBlockHeader* Remainder = NextBlock(Block);
    Remainder->prev_phys_block = Block;
    // rem は free。prev(=block) は used なので prev_free は立てない。
    Remainder->size_and_flags = RemainingSize | kBlockFreeBit;
    Remainder->next_free = nullptr;
    Remainder->prev_free = nullptr;
    // rem の物理次。free なら rem に統合して二重隣接 free を防ぐ。
    FBlockHeader* FollowingBlock = NextBlock(Remainder);
    FollowingBlock->prev_phys_block = Remainder;
    if (IsFree(FollowingBlock)) {
        Remainder = MergeNext(Remainder); // FollowingBlock を Remainder に吸収
    }
    InsertFreeBlock(Remainder);
    FBlockHeader* SecondFollowingBlock = NextBlock(Remainder);
    SecondFollowingBlock->prev_phys_block = Remainder;
    MarkPrevFree(SecondFollowingBlock);
}

/** アライメントの安全上限 (64KiB = VirtualAlloc 粒度)。これ以上は非現実的で search_size の wrap を招く。 */
static constexpr usize kMaxAlignment = 64u * 1024u;

// 確保
void* FTlsfAllocator::Alloc(usize Size, usize Alignment, FSourceLoc /*Location*/) noexcept
{
    using namespace tlsf;
    if (Size == 0) return nullptr;
    if (Size > MAXIMUM_SEARCHABLE_BLOCK_SIZE) return nullptr;
    if (Alignment < ALIGN_SIZE) Alignment = ALIGN_SIZE;
    if (Alignment > kMaxAlignment) return nullptr;            // 非現実的 alignment (search_size wrap 防止)
    if ((Alignment & (Alignment - 1u)) != 0u) return nullptr; // 2 のべき乗でない alignment は不正

    // サイズをアライン → バケット検索
    const usize AdjustedSize = AdjustRequestSize(Size, Alignment);
    if (AdjustedSize > MAXIMUM_SEARCHABLE_BLOCK_SIZE) return nullptr;

    // alignment > ALIGN_SIZE(16) の場合、chaining が保証する 16 整列では足りない。
    // 先頭に余白 (leading free ブロック) を切り出して payload を要求境界へ前進させる
    // memalign 経路を使う。そのぶん余白 + 最小 leading ブロックを上乗せして探索する。
    usize SearchSize = AdjustedSize;
    if (Alignment > ALIGN_SIZE) {
        const usize AlignmentOverhead = Alignment + kBlockHeaderOverhead + MIN_BLOCK_SIZE;
        if (AdjustedSize > MAXIMUM_SEARCHABLE_BLOCK_SIZE - AlignmentOverhead) return nullptr;
        SearchSize = AdjustedSize + AlignmentOverhead;
    }

    int fl, sl;
    MappingSearch(SearchSize, fl, sl);
    FBlockHeader* Block = SearchSuitableBlock(fl, sl);
    if (Block && Block != &m_NullBlock && BlockSize(Block) < SearchSize) Block = nullptr;
    if (!Block || Block == &m_NullBlock) {
        // OOM: 予約からコミットを伸ばして新プールを足し、再探索する (auto-grow)。
        if (!GrowToFit(SearchSize)) return nullptr; // 予約も尽きた = 真の OOM
        MappingSearch(SearchSize, fl, sl);
        Block = SearchSuitableBlock(fl, sl);
        if (!Block || Block == &m_NullBlock || BlockSize(Block) < SearchSize) {
            return nullptr; // grow しても入らない、または壊れたバケット候補
        }
    }

    // 取り出す
    RemoveFreeBlock(Block);

    // ---- over-alignment: 先頭ギャップを leading free ブロックとして切り出す ----
    if (Alignment > ALIGN_SIZE) {
        const u8* Payload = static_cast<u8*>(BlockToPtr(Block));
        u8* AlignedPayload = reinterpret_cast<u8*>(
            AlignUp(reinterpret_cast<uptr>(Payload), static_cast<uptr>(Alignment)));
        usize Gap = static_cast<usize>(AlignedPayload - Payload);
        if (Gap != 0) {
            // leading ブロックは free pointer (next_free/prev_free) を収容できる全長が必要。
            // gap が小さすぎる場合は 1 アライメント分前進させて十分な leading 長を確保する。
            const usize MinimumLeadingSize = kBlockHeaderOverhead + MIN_BLOCK_SIZE;
            while (Gap < MinimumLeadingSize) {
                AlignedPayload += Alignment;
                Gap += Alignment;
            }
            const usize OriginalBlockSize = BlockSize(Block);
            // leading: [block, block+gap)、remainder: block+gap 起点 (block_to_ptr = aligned)
            FBlockHeader* LeadingBlock = Block;
            FBlockHeader* Remainder = reinterpret_cast<FBlockHeader*>(reinterpret_cast<u8*>(Block) + Gap);
            SetBlockSize(LeadingBlock, Gap - kBlockHeaderOverhead); // ≡8 mod16
            MarkFree(LeadingBlock);
            Remainder->prev_phys_block = LeadingBlock;
            Remainder->size_and_flags = (OriginalBlockSize - Gap) | kBlockFreeBit | kPrevFreeBit;
            Remainder->next_free = nullptr;
            Remainder->prev_free = nullptr;
            // remainder の物理次ブロック (= 元の NextBlock) の prev リンクを張り直す
            FBlockHeader* FollowingRemainder = NextBlock(Remainder);
            FollowingRemainder->prev_phys_block = Remainder;
            // leading を free リストへ登録
            LeadingBlock->next_free = nullptr;
            LeadingBlock->prev_free = nullptr;
            InsertFreeBlock(LeadingBlock);
            Block = Remainder; // 以降は Remainder を確保対象にする
        }
    }

    // 必要分に分割
    TrimFreeBlock(Block, AdjustedSize);
    MarkUsed(Block);
    FBlockHeader* FollowingBlock = NextBlock(Block);
    MarkPrevUsed(FollowingBlock); // 次ブロックに「前は使用中」を伝達

    // 統計更新
    m_BytesUsed += BlockSize(Block);
    ++m_AllocationCount;
    if (m_BytesUsed > m_BytesPeak) m_BytesPeak = m_BytesUsed;

    void* const Result = BlockToPtr(Block);
    const bool bAllocationStartTracked = TrackAllocationStart(Result);
    ACS_ASSERT(bAllocationStartTracked);
    if (!bAllocationStartTracked) {
        // 外部境界表とヒープ本体の不一致を検出しても、払い出し途中の used ブロックを残さない。
        // 同じ位置に古いビットが残っていた場合だけ除去し、通常の Free と同じ統合経路へ戻す。
        if (IsAllocationStartTracked(Result)) {
            (void)UntrackAllocationStart(Result);
        }
        m_BytesUsed -= BlockSize(Block);
        --m_AllocationCount;
        MarkFree(Block);
        if (IsPrevFree(Block)) Block = MergePrev(Block);
        if (IsFree(NextBlock(Block))) Block = MergeNext(Block);
        InsertFreeBlock(Block);
        FBlockHeader* const RollbackFollowingBlock = NextBlock(Block);
        RollbackFollowingBlock->prev_phys_block = Block;
        MarkPrevFree(RollbackFollowingBlock);
        return nullptr;
    }
    // payload は必ず要求 alignment を満たす (16 は chaining 不変条件、>16 は上の leading split)
    ACS_ASSERT((reinterpret_cast<uptr>(Result) & (static_cast<uptr>(Alignment) - 1)) == 0);
    return Result;
}

// 登録プールの範囲だけでなく、物理ブロック境界と前後リンクも検証する。
bool FTlsfAllocator::OwnsPointer(const void* Pointer) const noexcept
{
    using namespace tlsf;
    if (!IsPointerWithinPoolPayloadRange(Pointer) || !IsAllocationStartTracked(Pointer)) return false;

    constexpr usize kMinimumRepresentableBlockSize = MIN_BLOCK_SIZE - kBlockHeaderOverhead;
    const uptr Address = reinterpret_cast<uptr>(Pointer);
    for (int i = 0; i < m_PoolSpanCount; ++i) {
        const PoolSpan& Span = m_PoolSpans[i];
        if (Address < Span.lo || Address >= Span.hi || Address - Span.lo < kBlockStartOffset) continue;

        const uptr BlockAddress = Address - kBlockStartOffset;
        if (BlockAddress < Span.lo || Span.hi - BlockAddress < kBlockStartOffset) return false;
        const FBlockHeader* const Block = reinterpret_cast<const FBlockHeader*>(BlockAddress);
        const volatile usize* const State = reinterpret_cast<const volatile usize*>(&Block->size_and_flags);
        const usize CurrentBlockSize = atomic_detail::LoadAcquire(State) & kBlockSizeMask;
        if (CurrentBlockSize < kMinimumRepresentableBlockSize || CurrentBlockSize > MAXIMUM_BLOCK_SIZE ||
            (CurrentBlockSize & (ALIGN_SIZE - 1u)) != kBlockHeaderOverhead) {
            return false;
        }

        const usize NextOffset = CurrentBlockSize - kBlockHeaderOverhead;
        if (NextOffset > Span.hi - Address) return false;
        const uptr NextAddress = Address + NextOffset;
        if (NextAddress > Span.hi - kBlockStartOffset) return false;
        const FBlockHeader* const NextHeader = reinterpret_cast<const FBlockHeader*>(NextAddress);
        if (NextHeader->prev_phys_block != Block) return false;

        const FBlockHeader* const PreviousHeader = Block->prev_phys_block;
        if (BlockAddress == Span.lo) return PreviousHeader == nullptr;
        const uptr PreviousAddress = reinterpret_cast<uptr>(PreviousHeader);
        if (!PreviousHeader || PreviousAddress < Span.lo || PreviousAddress >= BlockAddress ||
            (PreviousAddress & (ALIGN_SIZE - 1u)) != 0u) {
            return false;
        }
        const volatile usize* const PreviousState = reinterpret_cast<const volatile usize*>(
            &PreviousHeader->size_and_flags);
        const usize PreviousSize = atomic_detail::LoadAcquire(PreviousState) & kBlockSizeMask;
        if (PreviousSize < kMinimumRepresentableBlockSize || PreviousSize > MAXIMUM_BLOCK_SIZE ||
            (PreviousSize & (ALIGN_SIZE - 1u)) != kBlockHeaderOverhead) {
            return false;
        }
        if (PreviousSize - kBlockHeaderOverhead > BlockAddress - (PreviousAddress + kBlockStartOffset)) {
            return false;
        }
        return PreviousAddress + kBlockStartOffset + PreviousSize - kBlockHeaderOverhead == BlockAddress;
    }
    return false;
}

bool FTlsfAllocator::IsPointerWithinPoolPayloadRange(const void* Pointer) const noexcept
{
    using namespace tlsf;
    if (!Pointer || (reinterpret_cast<uptr>(Pointer) & (ALIGN_SIZE - 1u)) != 0u) return false;

    const uptr Address = reinterpret_cast<uptr>(Pointer);
    for (int Index = 0; Index < m_PoolSpanCount; ++Index) {
        const PoolSpan& Span = m_PoolSpans[Index];
        if (Address >= Span.lo && Address < Span.hi && Address - Span.lo >= kBlockStartOffset) {
            return true;
        }
    }
    return false;
}

bool FTlsfAllocator::IsAllocationStartTracked(const void* Pointer) const noexcept
{
    using namespace tlsf;
    if (!Pointer || (reinterpret_cast<uptr>(Pointer) & (ALIGN_SIZE - 1u)) != 0u) return false;

    const uptr Address = reinterpret_cast<uptr>(Pointer);
    for (int Index = 0; Index < m_PoolSpanCount; ++Index) {
        const PoolSpan& Span = m_PoolSpans[Index];
        if (Address < Span.lo || Address >= Span.hi || !Span.allocation_start_bitmap) continue;

        const usize SlotIndex = static_cast<usize>((Address - Span.lo) / ALIGN_SIZE);
        const usize ByteIndex = SlotIndex >> 3u;
        if (ByteIndex >= Span.allocation_start_bitmap_bytes) return false;
        const u8 Bit = static_cast<u8>(1u << (SlotIndex & 7u));
        return (Span.allocation_start_bitmap[ByteIndex] & Bit) != 0u;
    }
    return false;
}

bool FTlsfAllocator::TrackAllocationStart(const void* Pointer) noexcept
{
    using namespace tlsf;
    if (!Pointer || (reinterpret_cast<uptr>(Pointer) & (ALIGN_SIZE - 1u)) != 0u) return false;

    const uptr Address = reinterpret_cast<uptr>(Pointer);
    for (int Index = 0; Index < m_PoolSpanCount; ++Index) {
        PoolSpan& Span = m_PoolSpans[Index];
        if (Address < Span.lo || Address >= Span.hi || !Span.allocation_start_bitmap) continue;

        const usize SlotIndex = static_cast<usize>((Address - Span.lo) / ALIGN_SIZE);
        const usize ByteIndex = SlotIndex >> 3u;
        if (ByteIndex >= Span.allocation_start_bitmap_bytes) return false;
        const u8 Bit = static_cast<u8>(1u << (SlotIndex & 7u));
        if ((Span.allocation_start_bitmap[ByteIndex] & Bit) != 0u) return false;
        Span.allocation_start_bitmap[ByteIndex] |= Bit;
        return true;
    }
    return false;
}

bool FTlsfAllocator::UntrackAllocationStart(const void* Pointer) noexcept
{
    using namespace tlsf;
    if (!Pointer || (reinterpret_cast<uptr>(Pointer) & (ALIGN_SIZE - 1u)) != 0u) return false;

    const uptr Address = reinterpret_cast<uptr>(Pointer);
    for (int Index = 0; Index < m_PoolSpanCount; ++Index) {
        PoolSpan& Span = m_PoolSpans[Index];
        if (Address < Span.lo || Address >= Span.hi || !Span.allocation_start_bitmap) continue;

        const usize SlotIndex = static_cast<usize>((Address - Span.lo) / ALIGN_SIZE);
        const usize ByteIndex = SlotIndex >> 3u;
        if (ByteIndex >= Span.allocation_start_bitmap_bytes) return false;
        const u8 Bit = static_cast<u8>(1u << (SlotIndex & 7u));
        if ((Span.allocation_start_bitmap[ByteIndex] & Bit) == 0u) return false;
        Span.allocation_start_bitmap[ByteIndex] &= static_cast<u8>(~Bit);
        return true;
    }
    return false;
}

TResult<void> FTlsfAllocator::Reset() noexcept
{
    using namespace tlsf;
    if (m_bOwnsReservation) {
        TResult<void> ReleaseResult = m_Reservation.Release();
        if (ReleaseResult.IsErr()) {
            // 所有状態を残し、次回 Reset またはデストラクタから再試行できるようにする。
            return ReleaseResult;
        }
    }
    for (int Index = 0; Index < m_PoolSpanCount; ++Index) {
        if (m_PoolSpans[Index].allocation_start_bitmap) {
            (void)::HeapFree(::GetProcessHeap(), 0, m_PoolSpans[Index].allocation_start_bitmap);
        }
        m_PoolSpans[Index] = {};
    }
    m_bOwnsReservation = false;
    m_CommittedBytes = 0;
    m_FlBitmap = 0;
    for (int i = 0; i < FL_INDEX_COUNT; ++i) {
        m_SlBitmap[i] = 0;
        for (int j = 0; j < SL_INDEX_COUNT; ++j)
            m_Blocks[i][j] = &m_NullBlock;
    }
    m_NullBlock.next_free = &m_NullBlock;
    m_NullBlock.prev_free = &m_NullBlock;
    m_NullBlock.size_and_flags = 0;
    m_NullBlock.prev_phys_block = nullptr;
    m_BytesUsed = 0;
    m_BytesPeak = 0;
    m_AllocationCount = 0;
    m_PoolSpanCount = 0;
    m_Initialized = false;
    return Ok();
}

bool FTlsfAllocator::ContainsPtr(const void* Pointer) const noexcept
{
    // 予約所有時はその予約レンジ全体で O(1) 判定する (シャード化の Free ルーティング用)。
    // 予約は各シャード固有の連続 VM 範囲なので、所有判定はレンジ内かどうかで一意に決まる。
    if (m_bOwnsReservation && m_Reservation.Base() != nullptr) {
        const uptr Address = reinterpret_cast<uptr>(Pointer);
        const uptr ReservationBase = reinterpret_cast<uptr>(m_Reservation.Base());
        return Address >= ReservationBase && static_cast<usize>(Address - ReservationBase) < m_Reservation.Capacity();
    }
    // HeapAlloc プール等は登録プールスパンで判定 (overflow 無視で厳密に)。
    const uptr Address = reinterpret_cast<uptr>(Pointer);
    for (int i = 0; i < m_PoolSpanCount; ++i) {
        if (Address >= m_PoolSpans[i].lo && Address < m_PoolSpans[i].hi) return true;
    }
    return false;
}

// 物理ブロックチェイン + prev_phys/prev_free フラグの一貫性を検証する。
bool FTlsfAllocator::ValidateHeap() const noexcept
{
    using namespace tlsf;
    for (int i = 0; i < m_PoolSpanCount; ++i) {
        FBlockHeader* Block = reinterpret_cast<FBlockHeader*>(m_PoolSpans[i].lo);
        const uptr PoolEnd = m_PoolSpans[i].hi;
        FBlockHeader* PreviousBlock = nullptr;
        for (;;) {
            if (reinterpret_cast<uptr>(Block) >= PoolEnd) return false; // 番兵前に範囲外 = 破損
            const usize CurrentBlockSize = BlockSize(Block);
            if (CurrentBlockSize == 0) break; // 終端番兵 (size 0, used)
            if (PreviousBlock != nullptr && Block->prev_phys_block != PreviousBlock) return false;
            if (PreviousBlock != nullptr && (IsPrevFree(Block) != IsFree(PreviousBlock))) return false;
            if (IsAllocationStartTracked(BlockToPtr(Block)) == IsFree(Block)) return false;
            PreviousBlock = Block;
            Block = NextBlock(Block);
        }
    }
    return true;
}

void FTlsfAllocator::Free(void* Pointer) noexcept
{
    (void)TryFree(Pointer);
}

bool FTlsfAllocator::TryMarkThreadCacheBlock(void* Pointer) noexcept
{
    using namespace tlsf;
    // ShardedTLSF は予約所有ヒープだけを使う。初期化後に不変な予約範囲で確認し、
    // Grow/AddPool が更新する pool span 表をロック外から読まない。
    if (!Pointer || (reinterpret_cast<uptr>(Pointer) & (ALIGN_SIZE - 1u)) != 0u || !ContainsPtr(Pointer)) {
        return false;
    }

    FBlockHeader* const Block = PtrToBlock(Pointer);
    volatile usize* const State = reinterpret_cast<volatile usize*>(&Block->size_and_flags);
    usize CurrentState = atomic_detail::LoadAcquire(State);
    for (;;) {
        constexpr usize kMinimumRepresentableBlockSize = MIN_BLOCK_SIZE - kBlockHeaderOverhead;
        const usize CurrentBlockSize = CurrentState & kBlockSizeMask;
        if ((CurrentState & (kBlockFreeBit | kThreadCacheBit)) != 0 ||
            CurrentBlockSize < kMinimumRepresentableBlockSize || CurrentBlockSize > MAXIMUM_BLOCK_SIZE ||
            (CurrentBlockSize & (ALIGN_SIZE - 1u)) != kBlockHeaderOverhead) {
            return false;
        }
        if (atomic_detail::CompareExchange(State, CurrentState, CurrentState | kThreadCacheBit)) {
            return true;
        }
    }
}

bool FTlsfAllocator::TryRestoreThreadCacheBlock(void* Pointer) noexcept
{
    using namespace tlsf;
    if (!Pointer || (reinterpret_cast<uptr>(Pointer) & (ALIGN_SIZE - 1u)) != 0u || !ContainsPtr(Pointer)) {
        return false;
    }

    FBlockHeader* const Block = PtrToBlock(Pointer);
    volatile usize* const State = reinterpret_cast<volatile usize*>(&Block->size_and_flags);
    usize CurrentState = atomic_detail::LoadAcquire(State);
    for (;;) {
        constexpr usize kMinimumRepresentableBlockSize = MIN_BLOCK_SIZE - kBlockHeaderOverhead;
        const usize CurrentBlockSize = CurrentState & kBlockSizeMask;
        if ((CurrentState & kBlockFreeBit) != 0 || (CurrentState & kThreadCacheBit) == 0 ||
            CurrentBlockSize < kMinimumRepresentableBlockSize || CurrentBlockSize > MAXIMUM_BLOCK_SIZE ||
            (CurrentBlockSize & (ALIGN_SIZE - 1u)) != kBlockHeaderOverhead) {
            return false;
        }
        if (atomic_detail::CompareExchange(State, CurrentState, CurrentState & ~kThreadCacheBit)) {
            return true;
        }
    }
}

bool FTlsfAllocator::IsClientOwnedBlock(const void* Pointer) const noexcept
{
    using namespace tlsf;
    if (!Pointer || (reinterpret_cast<uptr>(Pointer) & (ALIGN_SIZE - 1)) != 0 || !OwnsPointer(Pointer)) {
        return false;
    }

    const FBlockHeader* const Block = PtrToBlock(Pointer);
    const volatile usize* const State = reinterpret_cast<const volatile usize*>(&Block->size_and_flags);
    const usize CurrentState = atomic_detail::LoadAcquire(State);
    return (CurrentState & (kBlockFreeBit | kThreadCacheBit)) == 0 && (CurrentState & kBlockSizeMask) != 0;
}

bool FTlsfAllocator::TryFree(void* Pointer) noexcept
{
    using namespace tlsf;
    if (!Pointer) return true;

    // --- 常時有効の安全ガード (低コスト、Release でも動作) ---
    // (0) 全 payload は 16 整列なので、非整列ポインタは当アロケータ由来でない (内部/野良ポインタ)。
    //     PtrToBlock が壊れたヘッダを指してヒープを破壊する前に弾く。
    if ((reinterpret_cast<uptr>(Pointer) & (ALIGN_SIZE - 1)) != 0) {
        ACS_ASSERT(false && "FTlsfAllocator::Free: 非整列ポインタ (当アロケータ由来でない)");
        return false;
    }
    // (1) 所属プール範囲外 (野良 / 別アロケータ由来) のポインタを弾く。誤った Free が
    //     ヒープ構造を破壊するのを未然に防ぐ。Debug は assert で即検出、Release は安全に no-op。
    if (!OwnsPointer(Pointer)) {
        ACS_ASSERT(false && "FTlsfAllocator::Free: ポインタが当アロケータの所有でない");
        return false;
    }
    FBlockHeader* Block = PtrToBlock(Pointer);
    // (2) 二重 free 検出: 既に free 状態のブロックを再 free するとフリーリストが壊れる。
    if (IsFree(Block)) {
        ACS_ASSERT(false && "FTlsfAllocator::Free: 二重 free を検出");
        return false;
    }
    if (IsThreadCached(Block)) {
        return false;
    }
    // (3) サイズ健全性: used ブロックのサイズは非 0 (0 は終端番兵/破損)。
    const usize CurrentBlockSize = BlockSize(Block);
    if (CurrentBlockSize == 0) {
        ACS_ASSERT(false && "FTlsfAllocator::Free: 破損ブロック (size 0)");
        return false;
    }
    if (!UntrackAllocationStart(Pointer)) {
        ACS_ASSERT(false && "FTlsfAllocator::Free: allocation-start tracking mismatch");
        return false;
    }

    m_BytesUsed -= CurrentBlockSize;
    --m_AllocationCount;
    MarkFree(Block);

#if ACS_MEMORY_DEBUG
    // UAF 検出: 解放した payload のうちフリーリストノード (先頭 16B = next_free/prev_free) を
    // 除く領域を 0xDD で塗る。解放後に読まれた場合に目立たせる (Debug のみ)。
    {
        u8* const NodeEnd = static_cast<u8*>(BlockToPtr(Block)) + sizeof(FBlockHeader*) * 2;
        u8* const End = reinterpret_cast<u8*>(NextBlock(Block));
        for (u8* q = NodeEnd; q < End; ++q)
            *q = static_cast<u8>(0xDD);
    }
#endif

    // 隣接フリーブロックがあれば統合（外部断片化を防ぐ）
    if (IsPrevFree(Block)) Block = MergePrev(Block);
    if (IsFree(NextBlock(Block))) Block = MergeNext(Block);
    InsertFreeBlock(Block);

    // 次ブロックに「前はフリー」を伝達
    FBlockHeader* FollowingBlock = NextBlock(Block);
    MarkPrevFree(FollowingBlock);
    return true;
}

void* FTlsfAllocator::Realloc(void* Pointer, usize OldSize, usize NewSize, usize Alignment,
                              FSourceLoc Location) noexcept
{
    using namespace tlsf;
    if (Pointer == nullptr) return Alloc(NewSize, Alignment, Location);
    if (NewSize == 0) {
        return TryFree(Pointer) ? nullptr : Pointer;
    }
    if (Alignment < ALIGN_SIZE) Alignment = ALIGN_SIZE;
    if (NewSize > MAXIMUM_SEARCHABLE_BLOCK_SIZE || Alignment > kMaxAlignment || (Alignment & (Alignment - 1u)) != 0u) {
        return nullptr;
    }

    // in-place 可否の前提: 当アロケータ所有 + 16 整列 + used ブロックであること。
    // 不正/解放済みポインタは旧領域を読まず/解放せず、失敗を返して所有権を維持する。
    const bool bPointerIsValid = OwnsPointer(Pointer) && ((reinterpret_cast<uptr>(Pointer) & (ALIGN_SIZE - 1)) == 0);
    if (!bPointerIsValid) {
        ACS_ASSERT(false && "FTlsfAllocator::Realloc: 不正なポインタ");
        return nullptr;
    }
    FBlockHeader* Block = PtrToBlock(Pointer);
    if (IsFree(Block)) {
        ACS_ASSERT(false && "FTlsfAllocator::Realloc: 解放済みポインタ");
        return nullptr;
    }
    if (IsThreadCached(Block)) return nullptr;

    // over-alignment 要求で現在の先頭が境界を満たさない場合は in-place できない → 移動。
    if (Alignment > ALIGN_SIZE && (reinterpret_cast<uptr>(Pointer) & (static_cast<uptr>(Alignment) - 1)) != 0) {
        return FAllocator::Realloc(Pointer, OldSize, NewSize, Alignment, Location);
    }

    const usize AdjustedSize = AdjustRequestSize(NewSize, ALIGN_SIZE);
    if (AdjustedSize > MAXIMUM_SEARCHABLE_BLOCK_SIZE) return nullptr;
    const usize OldBlockSize = BlockSize(Block);

    if (AdjustedSize > OldBlockSize) {
        // 拡大: 物理的に次のブロックが free で、合算サイズが要求を満たすなら in-place 統合。
        // そうでなければコピーを伴う移動 (新規確保 + memcpy + 解放) にフォールバック。
        FBlockHeader* const NextFreeBlock = NextBlock(Block);
        if (!(IsFree(NextFreeBlock) &&
              (OldBlockSize + BlockSize(NextFreeBlock) + kBlockHeaderOverhead) >= AdjustedSize)) {
            return FAllocator::Realloc(Pointer, OldSize, NewSize, Alignment, Location);
        }
        Block = MergeNext(Block);       // Block は used のまま NextFreeBlock を吸収
        MarkPrevUsed(NextBlock(Block)); // 統合後の次ブロックへ「prev(=Block) は used」を伝達
    }

    // ここで BlockSize(block) >= adjust。余剰を切り出して解放する (コピー不要、ptr 不変)。
    TrimUsedBlock(Block, AdjustedSize);

    // 統計更新 (used 総量の増減ぶん)。
    const usize NewBlockSize = BlockSize(Block);
    if (NewBlockSize >= OldBlockSize) {
        m_BytesUsed += (NewBlockSize - OldBlockSize);
        if (m_BytesUsed > m_BytesPeak) m_BytesPeak = m_BytesUsed;
    } else {
        m_BytesUsed -= (OldBlockSize - NewBlockSize);
    }
    return Pointer;
}

FTlsfAllocator::Stats FTlsfAllocator::GetStats() const noexcept
{
    Stats Statistics{};
    Statistics.bytes_used = m_BytesUsed;
    Statistics.bytes_peak = m_BytesPeak;
    Statistics.free_blocks = 0;
    Statistics.used_blocks = m_AllocationCount;
    Statistics.largest_free_block = 0;
    return Statistics;
}

} // namespace acs
