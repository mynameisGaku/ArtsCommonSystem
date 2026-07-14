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

/** 論理的な小ページサイズ (16 KiB)。 */
inline constexpr usize kVmSmallPageSize = 16 * 1024;

/** 論理的な中ページサイズ (128 KiB)。 */
inline constexpr usize kVmMediumPageSize = 128 * 1024;

/** 論理的なセグメントサイズ (8 MiB)。 */
inline constexpr usize kVmSegmentSize = 8 * 1024 * 1024;

/**
 * OS のページサイズを返す。
 *
 * @return ページサイズ (バイト)。
 */
usize VmPageSize() noexcept;

/**
 * VirtualAlloc の予約粒度を返す。
 *
 * @return 予約粒度 (バイト、典型的には 64KiB)。
 */
usize VmAllocGranularity() noexcept;

/**
 * アドレスが指定アライメントに整列しているかを返す。
 *
 * @param Address 判定するアドレス。
 * @param Alignment アライメント (0 以外の 2 のべき乗)。
 * @return 整列していれば true。0 または 2 のべき乗でなければ false。
 */
bool VmIsAligned(uptr Address, usize Alignment) noexcept;

/**
 * マップ済み領域を 8 バイトに圧縮した互換記述子。
 *
 * @warning 表現できるアドレスとページ数に上限があるため、所有権や LRU の内部管理には使用しない。
 */
struct mapped_t {
    /** 仮想アドレス (64KiB 整列前提で上位ビットをパック)。 */
    u64 packed_virtual_addr : 44;

    /** ページ数。 */
    u64 page_count : 16;

    /** スパース管理フラグ。 */
    u64 sparse : 1;

    /** その他フラグ (予約)。 */
    u64 misc : 3;
};
static_assert(sizeof(mapped_t) == 8, "mapped_t must be 8 bytes");

/** 連続ページ群を 8 バイトに圧縮した記述子。 */
struct page_t {
    /** 連続するページ数。 */
    u64 continuous_page_count : 12;

    /** その他フラグ (予約)。 */
    u64 misc : 4;

    /** 仮想アドレス (ページ整列前提でパック)。 */
    u64 packed_virtual_addr : 48;
};
static_assert(sizeof(page_t) == 8, "page_t must be 8 bytes");

/**
 * 仮想アドレス範囲の予約を表す RAII ハンドル。
 *
 * @details
 * Reserve(N) で N バイトの仮想範囲を予約し (物理ページ未割当)、デストラクタ/Release で返す。
 * Commit(Offset, Size) で必要な部分だけ物理ページを割り当てる。Offset と Size は OS ページ
 * サイズの倍数でなければならない。Decommit は実 VirtualFree を
 * すぐ行わず、まず内部 LRU キャッシュ (16 エントリ、既定の総保持上限 64 MiB) に入れ、
 * 再 Commit がヒットすればシステムコールを省略する。エビクト、明示 Flush、リリース時に
 * 実際に decommit する。総保持上限を超える単一範囲はキャッシュせず直ちに OS へ返す。
 * 既に利用中の範囲への Commit は統計を変えない冪等操作とする。二重/重複 Decommit と
 * キャッシュ範囲への部分重複操作はエラーにする。
 * Base() に対する VirtualAlloc/VirtualFree の直接呼び出しは統計を破壊するため禁止する。
 * move のみ可能 (copy 禁止)。同一インスタンスの並行操作には対応しない。
 */
class VmReservation {
public:
    /** 遅延デコミットキャッシュが既定で保持できる最大バイト数 (64 MiB)。 */
    static constexpr usize kDefaultMaximumCachedDecommitBytes = 64 * 1024 * 1024;

    /** 空の予約として構築する (Base()==nullptr)。 */
    VmReservation() noexcept = default;

    /** 予約を解放して破棄する (LRU を全て実 decommit してから MEM_RELEASE)。 */
    ~VmReservation() noexcept;

    /** コピー禁止 (予約を単独所有するため)。 */
    VmReservation(const VmReservation&) = delete;

    /** コピー代入も禁止。 */
    VmReservation& operator=(const VmReservation&) = delete;

    /**
     * ムーブ構築する (予約と LRU を奪い、相手を空にする)。
     *
     * @param Other 所有権を奪う元 (奪取後は空)。
     */
    VmReservation(VmReservation&& Other) noexcept;

    /**
     * ムーブ代入する (自身の予約を解放してから奪う)。
     *
     * @param Other 所有権を奪う元 (奪取後は空)。
     * @return *this。
     */
    VmReservation& operator=(VmReservation&& Other) noexcept;

    /**
     * 仮想範囲を予約する (VirtualAlloc MEM_RESERVE、物理ページ未割当)。
     *
     * @details 返される Base() と Capacity() は VirtualAlloc の予約粒度
     * (Windows では通常 64 KiB) に整列する。切り上げが usize を超える要求は拒否する。
     * @param CapacityBytes 予約サイズ (1 以上、内部で予約粒度に安全に切り上げ)。
     * @return 予約ハンドルを持つ TResult (失敗時は OS エラー)。
     */
    static TResult<VmReservation> Reserve(usize CapacityBytes) noexcept;

    /**
     * 予約を解放する (LRU を全て実 decommit してから MEM_RELEASE、多重呼び出し安全)。
     *
     * @details MEM_RELEASE が失敗した場合は所有状態を保持するため、呼び出し側は再試行できる。
     * @return 成功なら空の TResult、MEM_RELEASE 失敗なら所有状態を保持したエラー。
     */
    TResult<void> Release() noexcept;

    /**
     * 指定範囲に物理ページを割り当てる (MEM_COMMIT)。
     *
     * @details LRU ヒット時は VirtualAlloc を省略して再利用する。
     * @param Offset 予約先頭からのオフセット (OS ページサイズの倍数)。
     * @param Size コミットするバイト数 (1 ページ以上、OS ページサイズの倍数)。
     * @return 成功なら空の TResult、不正範囲、キャッシュとの部分重複、OS 失敗ならエラー。
     */
    TResult<void> Commit(usize Offset, usize Size) noexcept;

    /**
     * 指定範囲の物理ページを返却する。
     *
     * @details 実 VirtualFree は行わず LRU に入れる (エビクト時に実 decommit)。
     * @param Offset 予約先頭からのオフセット (OS ページサイズの倍数)。
     * @param Size デコミットするバイト数 (1 ページ以上、OS ページサイズの倍数)。
     * @return 成功なら空の TResult、不正範囲、重複、OS 失敗ならエラー。
     */
    TResult<void> Decommit(usize Offset, usize Size) noexcept;

    /**
     * 遅延デコミットキャッシュが保持できる最大バイト数を変更する。
     *
     * @details 現在の保持量が新しい上限を超える場合は、LRU 末尾から直ちに実デコミットする。
     * 0 を指定すると以降の Decommit はキャッシュせず、物理ページを直ちに OS へ返す。
     * @param MaximumCachedDecommitBytes キャッシュ全体の最大保持バイト数。
     * @return 成功なら空の TResult、既存範囲の VirtualFree に失敗した場合はエラー。
     */
    TResult<void> SetMaximumCachedDecommitBytes(usize MaximumCachedDecommitBytes) noexcept;

    /**
     * キャッシュ末尾から実デコミットし、保持量を指定値以下へ縮小する。
     *
     * @param TargetCachedBytes 許容するキャッシュ保持量。エントリ単位で縮小するため厳密一致は保証しない。
     * @return 成功なら空の TResult、VirtualFree 失敗ならエラー。
     */
    TResult<void> TrimDecommitCache(usize TargetCachedBytes) noexcept;

    /**
     * キャッシュ中の全範囲を直ちに実デコミットする。
     *
     * @return 成功なら空の TResult、VirtualFree 失敗ならエラー。
     */
    TResult<void> FlushDecommitCache() noexcept;

    /**
     * 予約の先頭アドレスを返す。
     *
     * @return 予約先頭 (未予約なら nullptr)。
     */
    void* Base() const noexcept
    {
        return m_Base;
    }

    /**
     * 予約の総容量を返す。
     *
     * @return 予約バイト数。
     */
    usize Capacity() const noexcept
    {
        return m_Capacity;
    }

    /**
     * 現在利用中のコミット済みバイト数を返す。
     *
     * @details 既存 API 互換のため名称を維持する。遅延デコミットキャッシュ分は含まない。
     * @return 利用中のコミット済みバイト数。
     */
    usize Committed() const noexcept
    {
        return m_ActiveCommittedBytes;
    }

    /** 現在利用中のコミット済みバイト数を返す。 */
    usize ActiveCommittedBytes() const noexcept
    {
        return m_ActiveCommittedBytes;
    }

    /** 遅延デコミットキャッシュが物理的に保持しているバイト数を返す。 */
    usize CachedCommittedBytes() const noexcept
    {
        return m_CachedCommittedBytes;
    }

    /** OS 上で実際にコミット状態にある合計バイト数を返す。 */
    usize ActualCommittedBytes() const noexcept
    {
        return m_ActiveCommittedBytes + m_CachedCommittedBytes;
    }

    /** 遅延デコミットキャッシュ全体の最大保持バイト数を返す。 */
    usize MaximumCachedDecommitBytes() const noexcept
    {
        return m_MaximumCachedDecommitBytes;
    }

    /** 遅延デコミットキャッシュの有効範囲数を返す。 */
    u32 DecommitCacheEntryCount() const noexcept
    {
        return m_DecommitCacheCount;
    }

    /** 遅延デコミットキャッシュの累積ヒット回数を返す。 */
    u64 DecommitCacheHitCount() const noexcept
    {
        return m_DecommitCacheHits;
    }

    /** 遅延デコミットキャッシュの累積ミス回数を返す。 */
    u64 DecommitCacheMissCount() const noexcept
    {
        return m_DecommitCacheMisses;
    }

    /**
     * LRU キャッシュのヒット回数を返す (プロファイラ用)。
     *
     * @return これまでの LRU ヒット数。
     */
    u32 LruHitCount() const noexcept
    {
        constexpr u64 Maximum = static_cast<u64>(~static_cast<u32>(0));
        return static_cast<u32>(m_DecommitCacheHits > Maximum ? Maximum : m_DecommitCacheHits);
    }

    /**
     * LRU キャッシュのミス回数を返す (プロファイラ用)。
     *
     * @return これまでの LRU ミス数。
     */
    u32 LruMissCount() const noexcept
    {
        constexpr u64 Maximum = static_cast<u64>(~static_cast<u32>(0));
        return static_cast<u32>(m_DecommitCacheMisses > Maximum ? Maximum : m_DecommitCacheMisses);
    }

private:
    /** LRU キャッシュのエントリ数。 */
    static constexpr u32 kDecommitCacheCapacity = 16;

    /** 圧縮せず完全な範囲を保持し、アドレスやページ数の切り捨てを防ぐ。 */
    struct DecommitCacheEntry {
        usize offset = 0;
        usize size = 0;
    };

    /**
     * デコミット範囲を LRU 先頭に挿入する (満杯なら末尾を実 decommit して空ける)。
     *
     * @param Offset 予約先頭からのオフセット。
     * @param Size 範囲のバイト数。
     * @return 成功なら空の TResult、エビクトの VirtualFree 失敗ならエラー。
     */
    TResult<void> InsertDecommitCache(usize Offset, usize Size) noexcept;

    /**
     * LRU から一致エントリを取り出す (再 Commit のヒット判定)。
     *
     * @param Offset 予約先頭からのオフセット。
     * @param Size 範囲のバイト数。
     * @return 一致して取り出せたら true (ヒット)、なければ false (ミス)。
     */
    bool TakeDecommitCache(usize Offset, usize Size) noexcept;

    /** 指定範囲がキャッシュ中の範囲と一部でも重なるかを返す。 */
    bool OverlapsDecommitCache(usize Offset, usize Size) const noexcept;

    /** 指定したキャッシュエントリを実デコミットし、配列から除去する。 */
    TResult<void> EvictDecommitCacheEntry(u32 Index) noexcept;

    /** 指定したキャッシュエントリを OS 操作なしで配列から除去する。 */
    void RemoveDecommitCacheEntry(u32 Index) noexcept;

    /** 全メンバを未予約状態へ戻す。 */
    void ResetState() noexcept;

    /** 予約の先頭アドレス。 */
    void* m_Base = nullptr;

    /** 予約の総容量。 */
    usize m_Capacity = 0;

    /** コミット済みバイト数。 */
    usize m_ActiveCommittedBytes = 0;

    /** 遅延デコミットキャッシュが物理的に保持しているバイト数。 */
    usize m_CachedCommittedBytes = 0;

    /** 遅延デコミットキャッシュ全体の最大保持バイト数。 */
    usize m_MaximumCachedDecommitBytes = kDefaultMaximumCachedDecommitBytes;

    /** デコミット待ちの LRU キャッシュ (再 Commit でヒットすれば再利用)。 */
    DecommitCacheEntry m_DecommitCache[kDecommitCacheCapacity]{};

    /** LRU の有効エントリ数。 */
    u32 m_DecommitCacheCount = 0;

    /** LRU ヒット数 (統計)。 */
    u64 m_DecommitCacheHits = 0;

    /** LRU ミス数 (統計)。 */
    u64 m_DecommitCacheMisses = 0;
};

/**
 * Non-Temporal ストアで領域をゼロクリアする (L1/L2 を汚染せず DDR へ直書き)。
 *
 * @details x64 では CPU と OS の AVX 対応を実行時に確認し、32B 整列かつ 256B 以上なら
 * アンロールした _mm256_stream_si256 版を使う。端数、未整列、AVX 非対応 CPU、ARM64 は
 * memset にフォールバックする。
 * @param Destination ゼロクリア先。
 * @param Size バイト数。
 */
void VmZeroFastNT(void* Destination, usize Size) noexcept;

} // namespace acs
