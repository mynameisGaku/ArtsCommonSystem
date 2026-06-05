// SPDX-License-Identifier: Apache-2.0
// TLSF アロケータ（Two-Level Segregated Fit、O(1) alloc/free）
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "memory/Allocator.h"
#include "memory/VirtualMemory.h"

namespace acs {

namespace tlsf {

/** 第 1 レベル (FL) インデックスの最大ビット数 (= 最大単一ブロック 4GiB に対応)。 */
constexpr int FL_INDEX_MAX     = 32;

/** 第 2 レベル (SL) の細分割を表す log2 (= 各 FL を 32 サブバケットに分ける)。 */
constexpr int SL_INDEX_LOG2    = 5;

/** 第 2 レベルのサブバケット数 (2^SL_INDEX_LOG2)。 */
constexpr int SL_INDEX_COUNT   = 1 << SL_INDEX_LOG2;

/** 線形領域と対数領域の境界となる FL インデックスのシフト量。 */
constexpr int FL_INDEX_SHIFT   = SL_INDEX_LOG2 + 2;

/** 第 1 レベルのバケット数 (m_Blocks / m_SlBitmap の第 1 次元サイズ)。 */
constexpr int FL_INDEX_COUNT   = FL_INDEX_MAX - FL_INDEX_SHIFT + 1;

/** 小ブロック領域 (線形割り当て) と対数割り当て領域の境界サイズ。 */
constexpr int SMALL_BLOCK_SIZE = 1 << FL_INDEX_SHIFT;

/** payload の保証アライメント (バイト)。 */
constexpr int ALIGN_SIZE       = 16;

/** フリーポインタ 2 本を収容できる最小ブロックサイズ (バイト)。 */
constexpr int MIN_BLOCK_SIZE   = 32;

/** TLSF のブロックヘッダ (フリー時のみ next_free/prev_free が payload 領域とオーバーレイする)。 */
struct FBlockHeader {
    /** 物理的に直前のブロック (隣接統合に使う)。 */
    FBlockHeader* prev_phys_block;

    /** 上位ビット=ブロックサイズ、bit0=this_free、bit1=prev_free のパック値。 */
    usize        size_and_flags;

    /** 同一バケットの次のフリーブロック (フリー時のみ有効)。 */
    FBlockHeader* next_free;

    /** 同一バケットの前のフリーブロック (フリー時のみ有効)。 */
    FBlockHeader* prev_free;
};

} // namespace tlsf

/**
 * Two-Level Segregated Fit による O(1) alloc/free の汎用アロケータ。
 *
 * @details
 * (FL, SL) の 2 段ビットマップでフリーリストを索引付けし、要求サイズ以上の最小ブロックを
 * O(1) で見つける。隣接フリーブロックは O(1) で統合して外部断片化を抑える。VM 予約から
 * 構築した場合は OOM 時に予約から段階的に commit して自動拡張する (auto-grow)。常時有効の
 * 安全ガード (非整列/範囲外/二重 free 検知) を持ち、in-place realloc に対応する。本クラス自体は
 * 同期しない — マルチスレッドでは呼び出し側でロックすること (FShardedTlsfAllocator が部品として使う)。
 */
class FTlsfAllocator final : public FAllocator {
public:
    /** 未初期化状態で構築する (使用前に Init/InitWithReservation を呼ぶこと)。 */
    FTlsfAllocator() noexcept = default;

    /** 破棄する (予約を所有する場合の解放は Reset で行う)。 */
    ~FTlsfAllocator() noexcept override;

    /** コピー禁止 (ヒープ状態を単独所有するため)。 */
    FTlsfAllocator(const FTlsfAllocator&) = delete;

    /** コピー代入も禁止。 */
    FTlsfAllocator& operator=(const FTlsfAllocator&) = delete;

    /**
     * 既存メモリ領域を単一プールとして初期化する。
     *
     * @details pool_base は 16B 整列必須、pool_size は 1KiB 以上推奨。プール所有権は移らない (呼び出し側が解放)。
     * @param pool_base プールに使う領域の先頭 (16B 整列)。
     * @param pool_size プールのバイト数。
     * @return 成功なら空の TResult、引数不正ならエラー。
     */
    TResult<void> Init(void* pool_base, usize pool_size) noexcept;

    /**
     * VM 予約を所有しつつ、その先頭 commit_initial バイトをプールとして初期化する。
     *
     * @details
     * 予約の所有権を奪い、OOM 時の auto-grow を有効化する。以後の OOM では予約末尾まで
     * 段階的に commit + AddPool して拡張する。
     * @param reservation 所有権を移す VM 予約。
     * @param commit_initial 初回にコミットしてプール登録するバイト数。
     * @return 成功なら空の TResult、コミット/初期化失敗ならエラー。
     */
    TResult<void> InitWithReservation(VmReservation&& reservation,
                                     usize commit_initial) noexcept;

    /**
     * 既存ヒープに追加のプールを登録する。
     *
     * @details auto-grow や手動拡張で複数プールを束ねる。Free 時の所属検証用に範囲も記録する (最大 kMaxTrackedPools)。
     * @param pool_base 追加プールの先頭 (16B 整列)。
     * @param pool_size 追加プールのバイト数 (内部で 16B 切り下げ)。
     * @return 成功なら空の TResult、サイズ不足ならエラー。
     */
    TResult<void> AddPool(void* pool_base, usize pool_size) noexcept;

    /**
     * size バイトを alignment 整列で確保する。
     *
     * @details
     * 16B 整列は chaining 不変条件で保証され、それを超える alignment は先頭ギャップを
     * leading free ブロックとして切り出して満たす。空きが無ければ auto-grow を試み、それも
     * 不可なら nullptr。size==0 や非現実的な size/alignment も nullptr。
     * @param size 確保するバイト数。
     * @param alignment 要求アライメント (16B 未満は 16 に引き上げ、2 のべき乗必須)。
     * @param loc 診断用の呼び出し位置 (本実装では未使用)。
     * @return 確保した領域 (失敗時 nullptr)。
     */
    void* Alloc(usize size, usize alignment, FSourceLoc loc) noexcept override;

    /**
     * ptr を解放し、隣接フリーブロックがあれば統合する。
     *
     * @details
     * nullptr は no-op。非整列・所属プール範囲外・二重 free・サイズ 0 の破損ブロックは
     * 安全ガードで弾く (Debug は assert、Release は no-op)。
     * @param ptr このアロケータが払い出した領域 (nullptr 可)。
     */
    void  Free (void* ptr)                                  noexcept override;

    /**
     * ptr を new_size に再確保する (可能なら in-place)。
     *
     * @details
     * 縮小・物理次が十分大きいフリーブロックなら ptr を保ったまま in-place で済ませる。
     * over-alignment で境界を満たせない場合や in-place 不能なら移動 (新規確保+コピー+解放)。
     * 不正/解放済みポインタは旧領域に触れず新規確保のみで安全側に倒す。
     * @param ptr 既存の確保 (nullptr なら新規確保)。
     * @param old_size 旧サイズ (移動時のコピー量決定に使う)。
     * @param new_size 新サイズ (0 なら解放)。
     * @param alignment 要求アライメント (16B 未満は 16 に引き上げ)。
     * @param loc 診断用の呼び出し位置。
     * @return 再確保した領域 (失敗時や new_size==0 のとき nullptr)。
     */
    void* Realloc(void* ptr, usize old_size, usize new_size,
                  usize alignment, FSourceLoc loc)           noexcept override;

    /**
     * 現在の使用バイト数を返す。
     *
     * @return 確保中ブロックのサイズ合計 (ヘッダ込みのブロックサイズ基準)。
     */
    u64 BytesAllocated() const noexcept override { return m_BytesUsed; }

    /**
     * 過去ピークの使用バイト数を返す。
     *
     * @return これまでの最大使用バイト数。
     */
    u64 PeakBytes()      const noexcept override { return m_BytesPeak; }

    /**
     * 識別名を返す。
     *
     * @return 文字列 "TLSF"。
     */
    const char* Name()   const noexcept override { return "TLSF"; }

    /** GetStats が返す統計値。 */
    struct Stats {
        /** 現在の使用バイト数。 */
        u64 bytes_used;

        /** 過去ピークの使用バイト数。 */
        u64 bytes_peak;

        /** フリーブロック数 (現状未集計、0)。 */
        u64 free_blocks;

        /** 使用中ブロック数 (現状未集計、0)。 */
        u64 used_blocks;

        /** 最大フリーブロックのサイズ (現状未集計、0)。 */
        u64 largest_free_block;
    };

    /**
     * 統計のスナップショットを返す。
     *
     * @details bytes_used / bytes_peak のみ実値。ブロック数系は現状未集計で 0。
     * @return 統計値の Stats。
     */
    Stats GetStats() const noexcept;

    /**
     * ヒープの整合性を検証する (デバッグ/診断用)。
     *
     * @details
     * 物理ブロックチェインと prev_phys/prev_free フラグの一貫性を全プールで走査する
     * (O(ブロック数))。スレッドセーフではない — Alloc/Free と同じロック下で呼ぶこと。
     * @return 整合していれば true、破損を検出したら false。
     */
    bool ValidateHeap() const noexcept;

    /**
     * ptr がこのアロケータの管理範囲に属するかを返す。
     *
     * @details
     * シャード化アロケータが Free 時に所有シャードを特定するのに使う。予約所有時は予約レンジで
     * O(1) 判定、それ以外 (HeapAlloc プール等) は登録プールスパンで判定する。
     * @param p 判定対象のポインタ。
     * @return 管理範囲内なら true。
     */
    bool ContainsPtr(const void* p) const noexcept;

    /**
     * アロケータを未初期化状態へ戻す。
     *
     * @details
     * 予約を所有していれば解放する。再 Init を可能にする
     * (FShardedTlsfAllocator の Shutdown→再 Init や FMemorySystem の再初期化で使う)。
     */
    void Reset() noexcept;

    /**
     * payload ポインタからブロックサイズをロック無しで読む静的ヘルパ。
     *
     * @details
     * size_and_flags は payload の手前 sizeof(usize) に在り確保中は不変なので、ロック無しで
     * 安全に読める。thread-cache のサイズクラス判定に使う。このアロケータが払い出した
     * ポインタにのみ有効。
     * @param ptr このアロケータが払い出した payload ポインタ。
     * @return ブロックサイズ (下位 2bit のフラグを除いた値)。
     */
    static usize PayloadBlockSize(const void* ptr) noexcept {
        const usize saf = *reinterpret_cast<const usize*>(
            static_cast<const u8*>(ptr) - sizeof(usize));
        return saf & ~usize(3);   // 下位 2bit (free/prev_free フラグ) を落とす
    }

private:
    /** 第 1 レベルビットマップ (どの FL バケットが非空か)。 */
    u32              m_FlBitmap = 0;

    /** 第 2 レベルビットマップ (各 FL について、どの SL サブバケットが非空か)。 */
    u32              m_SlBitmap[tlsf::FL_INDEX_COUNT] = {};

    /** (FL, SL) ごとのフリーリスト先頭。 */
    tlsf::FBlockHeader* m_Blocks[tlsf::FL_INDEX_COUNT][tlsf::SL_INDEX_COUNT] = {};

    /** リスト末端を表す番兵ブロック (自己循環)。 */
    tlsf::FBlockHeader  m_NullBlock {};

    /** 所有する VM 予約 (InitWithReservation 時のみ)。 */
    VmReservation    m_Reservation;

    /** m_Reservation の所有権を持つか (true なら Reset で解放する)。 */
    bool             m_bOwnsReservation = false;

    /** 予約のうちコミット済みでプール登録された総バイト数 (= 次に commit する offset、auto-grow 用)。 */
    usize            m_CommittedBytes = 0;

    /** 現在の使用バイト数。 */
    u64              m_BytesUsed = 0;

    /** 過去ピークの使用バイト数。 */
    u64              m_BytesPeak = 0;

    /** 追跡できる登録プールの上限数 (超えると範囲検証を諦める)。 */
    static constexpr int kMaxTrackedPools = 64;

    /** 登録プール 1 つの範囲 [lo, hi) (= pool_base .. pool_base+pool_size)。 */
    struct PoolSpan {
        /** 範囲の下端 (pool_base)。 */
        uptr lo;

        /** 範囲の上端 (pool_base + pool_size、排他)。 */
        uptr hi;
    };

    /** 登録済みプールの範囲配列 (Free 時の所属検証用)。 */
    PoolSpan         m_PoolSpans[kMaxTrackedPools] = {};

    /** 記録済みプール範囲の数。 */
    int              m_PoolSpanCount   = 0;

    /** プール数が上限を超え範囲追跡を諦めたか (true なら範囲検証を無効化)。 */
    bool             m_PoolTrackOverflow = false;

    /**
     * ptr がいずれかの登録プール範囲内かを返す。
     *
     * @details overflow 時は全プールを追跡できていないので検証不能 → true 扱い (正当な Free を誤って弾かない)。
     * @param p 判定対象のポインタ。
     * @return 範囲内 (または overflow 時) なら true。
     */
    bool OwnsPointer(const void* p) const noexcept;

    /**
     * OOM 時に予約から commit + AddPool して needed_bytes 以上を収容できるよう拡張する。
     *
     * @details 幾何級数 (現コミットの 50%) で伸ばしてプール数を対数に抑える。予約非所有/予約枯渇時は拡張しない。
     * @param needed_bytes 収容したいブロックサイズ (探索切り上げ後)。
     * @return 拡張できれば true、不可なら false (= 真の OOM)。
     */
    bool GrowToFit(usize needed_bytes) noexcept;

    /**
     * ブロックを (FL, SL) バケットの先頭に挿入し、対応ビットマップを立てる。
     *
     * @param block 挿入するフリーブロック。
     */
    void InsertFreeBlock(tlsf::FBlockHeader* block) noexcept;

    /**
     * ブロックをフリーリストから取り除き、バケットが空になればビットマップを下ろす。
     *
     * @param block 取り除くフリーブロック。
     */
    void RemoveFreeBlock(tlsf::FBlockHeader* block) noexcept;

    /**
     * (fl, sl) 以上で最初に見つかる非空バケットのフリーブロックを返す。
     *
     * @param fl 入出力: 探索開始 FL。見つかったブロックの FL に更新される。
     * @param sl 入出力: 探索開始 SL。見つかったブロックの SL に更新される。
     * @return 見つかったフリーブロック (無ければ nullptr)。
     */
    tlsf::FBlockHeader* SearchSuitableBlock(int& fl, int& sl) noexcept;

    /**
     * フリーブロックを size に縮め、余りを新しいフリーブロックとして再登録する。
     *
     * @details 切り出す余裕が無い (exact-fit/僅差) 場合は何もしない (over-allocation を許容)。
     * @param block 縮めるフリーブロック (リストからは外れている前提)。
     * @param size 残す要求サイズ (ブロックサイズ単位)。
     */
    void TrimFreeBlock(tlsf::FBlockHeader* block, usize size) noexcept;

    /**
     * 使用中ブロックを size に縮め、余りを free ブロックとして解放する (in-place realloc 用)。
     *
     * @details
     * block は used のまま保持し、切り出した余り (tail) を解放する。tail の物理次が free なら
     * 統合する。切り出す余裕が無ければ何もしない。
     * @param block 縮める使用中ブロック。
     * @param size 残す要求サイズ (ブロックサイズ単位)。
     */
    void TrimUsedBlock(tlsf::FBlockHeader* block, usize size) noexcept;

    /**
     * block を物理的に前のフリーブロックと統合する。
     *
     * @param block 統合される側のブロック。
     * @return 統合後の (前の) ブロック。
     */
    tlsf::FBlockHeader* MergePrev(tlsf::FBlockHeader* block) noexcept;

    /**
     * block を物理的に次のフリーブロックと統合する。
     *
     * @param block 統合元のブロック。
     * @return 統合後の (= block) ブロック。
     */
    tlsf::FBlockHeader* MergeNext(tlsf::FBlockHeader* block) noexcept;
};

} // namespace acs
