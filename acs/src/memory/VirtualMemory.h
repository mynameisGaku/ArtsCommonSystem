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
inline constexpr usize kVmSmallPageSize  = 16 * 1024;

/** 論理的な中ページサイズ (128 KiB)。 */
inline constexpr usize kVmMediumPageSize = 128 * 1024;

/** 論理的なセグメントサイズ (8 MiB)。 */
inline constexpr usize kVmSegmentSize    = 8 * 1024 * 1024;

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
 * @param addr 判定するアドレス。
 * @param alignment アライメント (2 のべき乗)。
 * @return 整列していれば true。
 */
bool  VmIsAligned(uptr addr, usize alignment) noexcept;

/** マップ済み領域を 8 バイトに圧縮した記述子 (LRU エントリ用)。 */
struct mapped_t {
    /** 仮想アドレス (64KiB 整列前提で上位ビットをパック)。 */
    u64 packed_virtual_addr : 44;

    /** ページ数。 */
    u64 page_count          : 16;

    /** スパース管理フラグ。 */
    u64 sparse              :  1;

    /** その他フラグ (予約)。 */
    u64 misc                :  3;
};
static_assert(sizeof(mapped_t) == 8, "mapped_t must be 8 bytes");

/** 連続ページ群を 8 バイトに圧縮した記述子。 */
struct page_t {
    /** 連続するページ数。 */
    u64 continuous_page_count : 12;

    /** その他フラグ (予約)。 */
    u64 misc                  :  4;

    /** 仮想アドレス (ページ整列前提でパック)。 */
    u64 packed_virtual_addr   : 48;
};
static_assert(sizeof(page_t) == 8, "page_t must be 8 bytes");

/**
 * 仮想アドレス範囲の予約を表す RAII ハンドル。
 *
 * @details
 * Reserve(N) で N バイトの仮想範囲を予約し (物理ページ未割当)、デストラクタ/Release で返す。
 * Commit(offset, size) で必要な部分だけ物理ページを割り当てる。Decommit は実 VirtualFree を
 * すぐ行わず、まず内部 LRU キャッシュ (16 エントリ) に入れ、再 Commit がヒットすればシステム
 * コールを省略する。エビクト時とリリース時に実際に decommit する。move のみ可能 (copy 禁止)。
 */
class VmReservation {
public:
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
     * @param o 所有権を奪う元 (奪取後は空)。
     */
    VmReservation(VmReservation&& o) noexcept;

    /**
     * ムーブ代入する (自身の予約を解放してから奪う)。
     *
     * @param o 所有権を奪う元 (奪取後は空)。
     * @return *this。
     */
    VmReservation& operator=(VmReservation&& o) noexcept;

    /**
     * 仮想範囲を予約する (VirtualAlloc MEM_RESERVE、物理ページ未割当)。
     *
     * @param capacity_bytes 予約サイズ (内部で予約粒度に切り上げ)。
     * @return 予約ハンドルを持つ TResult (失敗時は OS エラー)。
     */
    static TResult<VmReservation> Reserve(usize capacity_bytes) noexcept;

    /** 予約を解放する (LRU を全て実 decommit してから MEM_RELEASE、多重呼び出し安全)。 */
    void Release() noexcept;

    /**
     * 指定範囲に物理ページを割り当てる (MEM_COMMIT)。
     *
     * @details LRU ヒット時は VirtualAlloc を省略して再利用する。範囲が予約外ならエラー。
     * @param offset 予約先頭からのオフセット。
     * @param size コミットするバイト数。
     * @return 成功なら空の TResult、範囲外/OS 失敗ならエラー。
     */
    TResult<void> Commit  (usize offset, usize size) noexcept;

    /**
     * 指定範囲の物理ページを返却する。
     *
     * @details 実 VirtualFree は行わず LRU に入れる (エビクト時に実 decommit)。範囲が予約外ならエラー。
     * @param offset 予約先頭からのオフセット。
     * @param size デコミットするバイト数。
     * @return 成功なら空の TResult、範囲外ならエラー。
     */
    TResult<void> Decommit(usize offset, usize size) noexcept;

    /**
     * 予約の先頭アドレスを返す。
     *
     * @return 予約先頭 (未予約なら nullptr)。
     */
    void* Base()      const noexcept { return m_Base; }

    /**
     * 予約の総容量を返す。
     *
     * @return 予約バイト数。
     */
    usize Capacity()  const noexcept { return m_Capacity; }

    /**
     * 現在コミット済みのバイト数を返す。
     *
     * @return コミット済みバイト数。
     */
    usize Committed() const noexcept { return m_Committed; }

    /**
     * LRU キャッシュのヒット回数を返す (プロファイラ用)。
     *
     * @return これまでの LRU ヒット数。
     */
    u32   LruHitCount()  const noexcept { return m_LruHits; }

    /**
     * LRU キャッシュのミス回数を返す (プロファイラ用)。
     *
     * @return これまでの LRU ミス数。
     */
    u32   LruMissCount() const noexcept { return m_LruMisses; }

private:
    /** LRU キャッシュのエントリ数。 */
    static constexpr u32 kLruEntries = 16;

    /**
     * デコミット範囲を LRU 先頭に挿入する (満杯なら末尾を実 decommit して空ける)。
     *
     * @param offset 予約先頭からのオフセット。
     * @param page_count ページ数。
     */
    void  LruInsert(u64 offset, u32 page_count) noexcept;

    /**
     * LRU から一致エントリを取り出す (再 Commit のヒット判定)。
     *
     * @param offset 予約先頭からのオフセット。
     * @param page_count ページ数。
     * @return 一致して取り出せたら true (ヒット)、なければ false (ミス)。
     */
    bool  LruTake(u64 offset, u32 page_count) noexcept;

    /** LRU 内の全エントリを実 decommit して空にする。 */
    void  LruEvictAll() noexcept;

    /** 予約の先頭アドレス。 */
    void*       m_Base       = nullptr;

    /** 予約の総容量。 */
    usize       m_Capacity   = 0;

    /** コミット済みバイト数。 */
    usize       m_Committed  = 0;

    /** デコミット待ちの LRU キャッシュ (再 Commit でヒットすれば再利用)。 */
    mapped_t    m_Lru[kLruEntries] {};

    /** LRU の有効エントリ数。 */
    u32         m_LruCount = 0;

    /** LRU ヒット数 (統計)。 */
    u32         m_LruHits = 0;

    /** LRU ミス数 (統計)。 */
    u32         m_LruMisses = 0;
};

/**
 * Non-Temporal ストアで領域をゼロクリアする (L1/L2 を汚染せず DDR へ直書き)。
 *
 * @details 32B 整列かつ 256B 倍数ならアンロールした _mm256_stream_si256 版を使い、それ以外や端数は memset にフォールバックする。
 * @param dst ゼロクリア先。
 * @param size バイト数。
 */
void VmZeroFastNT(void* dst, usize size) noexcept;

} // namespace acs
