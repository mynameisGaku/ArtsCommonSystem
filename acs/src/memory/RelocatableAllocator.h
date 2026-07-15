// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — FRelocatableAllocator (ハンドルベース再配置/デフラグ可能アロケータ)
// -----------------------------------------------------------------------------
// 生ポインタは「動かせない」ため、デフラグ (compaction) には間接参照が必須。
// 利用側は確保時に **ハンドル** を受け取り、Resolve(handle) で現在のポインタを得る。
// Compact() は生存ブロックを前方に詰めて外部断片化を解消し、各ハンドルの参照先を更新する
// (Compact 後はポインタが変わるので Resolve を取り直すこと)。
//
// 用途: 長時間セッションで確保/解放を繰り返す可変サイズデータ (アセットのストリーミング
// バッファ、テキスト/シリアライズ作業領域 等) を、断片化させずに詰め直したいケース。
//
// スレッド安全性: **本クラスは内部同期しない**。Compact は全ポインタを無効化するため、
// Resolve したポインタを保持したまま他スレッドが Compact すると破綻する。単一スレッドで
// 使うか、利用側で「Resolve したポインタを握っている間は Compact しない」ことを保証すること。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"

namespace acs {

class FAllocator;

/** 再配置可能確保を指すハンドル (index = エントリ番号、generation = 世代で use-after-free を検出)。 */
struct FRelocHandle {
    /** ハンドルテーブル内のエントリ番号。 */
    u32  index      = 0;

    /** 世代 (0 = 無効。Free→再 Alloc で ++ され旧ハンドルを無効化する)。 */
    u64  generation = 0;

    /**
     * ハンドルが有効かを返す。
     *
     * @return generation が非 0 なら true。
     */
    bool IsValid() const noexcept { return generation != 0u; }
};

/**
 * 2 つのハンドルが同一かを返す。
 *
 * @param a 比較対象 A。
 * @param b 比較対象 B。
 * @return index と generation が一致すれば true。
 */
inline bool operator==(FRelocHandle a, FRelocHandle b) noexcept {
    return a.index == b.index && a.generation == b.generation;
}

/**
 * 2 つのハンドルが異なるかを返す。
 *
 * @param a 比較対象 A。
 * @param b 比較対象 B。
 * @return 一致しなければ true。
 */
inline bool operator!=(FRelocHandle a, FRelocHandle b) noexcept { return !(a == b); }

/**
 * ハンドル間接参照でデフラグ (compaction) を可能にする再配置アロケータ。
 *
 * @details
 * 生ポインタは動かせないため、確保時に世代付きハンドルを返し、Resolve(handle) で現在のポインタを
 * 得る。Alloc は bump 方式、Free は生存ビットを下ろすだけ (gap は残る)。Compact() が生存ブロックを
 * アドレス順に前方へ詰めて外部断片化を解消し、各ハンドルの参照先を更新する (呼び出し後はポインタが
 * 変わるので Resolve を取り直す)。内部同期はしない — 単一スレッドで使うか、Resolve したポインタを
 * 握っている間は Compact しないことを利用側が保証すること。
 */
class FRelocatableAllocator {
public:
    /** 未初期化状態で構築する (使用前に Init を呼ぶこと)。 */
    FRelocatableAllocator() noexcept = default;

    /** Shutdown を呼んでアリーナとテーブルを backing に返して破棄する。 */
    ~FRelocatableAllocator() noexcept;

    /** コピー禁止 (アリーナとハンドルテーブルを単独所有するため)。 */
    FRelocatableAllocator(const FRelocatableAllocator&) = delete;

    /** コピー代入も禁止。 */
    FRelocatableAllocator& operator=(const FRelocatableAllocator&) = delete;

    /**
     * アリーナとハンドルテーブルを backing から確保して初期化する。
     *
     * @details アリーナ・エントリ配列・Compact 用作業配列の 3 つを確保する。多重 Init や引数不正・確保失敗はエラー。
     * @param capacity_bytes アリーナの総容量。
     * @param max_handles 同時に存在できるハンドルの上限数。
     * @param backing 各種確保元 (nullptr なら DefaultAllocator)。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    TResult<void> Init(usize capacity_bytes, u32 max_handles, FAllocator* backing = nullptr) noexcept;

    /** 確保済みのアリーナ・テーブルを backing に返し、未初期化状態へ戻す。 */
    void Shutdown() noexcept;

    /**
     * size バイトを確保してハンドルを返す。
     *
     * @details
     * bump カーソルを進めて確保する。末尾に入らないが詰めれば入る場合は自動的に Compact してから
     * 確保する。容量不足・ハンドル枯渇・引数不正時は無効ハンドル。
     * @param size 確保するバイト数 (4GiB 未満)。
     * @param alignment 要求アライメント (2 のべき乗、既定 16)。
     * @return 確保を指すハンドル (失敗時は IsValid()==false)。
     */
    FRelocHandle Alloc(usize size, usize alignment = 16) noexcept;

    /**
     * ハンドルの確保を解放する。
     *
     * @details 生存ビットを下ろしてエントリを空きリストへ戻すだけ (ブロックは Compact まで gap として残る)。無効/解放済みハンドルは静かに無視する。
     * @param h 解放するハンドル。
     */
    void  Free(FRelocHandle h) noexcept;

    /**
     * ハンドルの現在のポインタを返す。
     *
     * @details 次の Compact まで有効 (Compact 後は取り直すこと)。無効/解放済みハンドルは nullptr。
     * @param h 解決するハンドル。
     * @return 現在のポインタ (無効なら nullptr)。
     */
    void* Resolve(FRelocHandle h) const noexcept;

    /**
     * ハンドルが指す確保のサイズを返す。
     *
     * @param h 対象ハンドル。
     * @return 確保サイズ (無効なら 0)。
     */
    usize SizeOf(FRelocHandle h)  const noexcept;

    /**
     * ハンドルが現在有効かを返す。
     *
     * @param h 検証するハンドル。
     * @return 生存中で世代も一致すれば true。
     */
    bool  ValidateHandle(FRelocHandle h) const noexcept;

    /**
     * 生存ブロックを前方へ詰めて外部断片化を解消する。
     *
     * @details 生存エントリをアドレス昇順にスライドして gap を埋め、各ハンドルの参照先を更新する。呼び出し後は Resolve を取り直すこと。
     * @return 回収した high-water バイト数。
     */
    usize Compact() noexcept;

    /**
     * アリーナの総容量を返す。
     *
     * @return Init で確保したバイト数。
     */
    usize Capacity()  const noexcept { return m_Capacity; }

    /**
     * 生存ペイロードの総バイト数を返す。
     *
     * @return 生存中の確保サイズ合計 (gap を含まない)。
     */
    usize Used()      const noexcept { return m_LiveBytes; }

    /**
     * bump カーソル位置 (high-water) を返す。
     *
     * @return 現在のカーソル位置 (解放済み gap を含む。Compact で下がる)。
     */
    usize HighWater() const noexcept { return m_Cursor; }

    /**
     * 生存ハンドル数を返す。
     *
     * @return 現在生存中の確保の数。
     */
    u32   LiveCount() const noexcept { return m_LiveCount; }

private:
    /** ハンドルテーブルの 1 エントリ (確保 1 件のメタデータ)。 */
    struct Entry {
        /** payload のアリーナ内ポインタ (Compact で更新)。 */
        u8*  ptr;

        /** 確保サイズ (バイト)。 */
        u32  size;

        /** 確保時のアライメント。 */
        u32  align;

        /** 現在の世代 (ハンドルの generation と照合)。 */
        u64  generation;

        /** 生存中か (false なら空きエントリ)。 */
        bool live;

        /** 空きエントリの単方向リストの次インデックス。 */
        u32  next_free;
    };

    /**
     * ハンドルから生存エントリを解決する。
     *
     * @param h 解決するハンドル。
     * @param out 解決したエントリへのポインタを返す。
     * @return 生存中で世代一致なら true (out 有効)、それ以外は false。
     */
    bool ResolveEntry(FRelocHandle h, Entry*& out) const noexcept;

    /** アリーナの先頭 (backing から確保)。 */
    u8*         m_Base       = nullptr;

    /** アリーナの総容量。 */
    usize       m_Capacity   = 0;

    /** bump カーソル位置 (= high water、gap 込み)。 */
    usize       m_Cursor     = 0;

    /** 生存ペイロードの総バイト数。 */
    usize       m_LiveBytes  = 0;

    /** ハンドルテーブル (max_handles 個の Entry)。 */
    Entry*      m_Entries     = nullptr;

    /** ハンドルテーブルの容量。 */
    u32         m_MaxHandles = 0;

    /** 空きエントリリストの先頭インデックス (kNoIndex で空)。 */
    u32         m_FreeHead   = 0xFFFFFFFFu;

    /** 生存ハンドル数。 */
    u32         m_LiveCount  = 0;

    /** Compact 用の作業配列 (生存エントリをアドレス順に並べる、max_handles 個)。 */
    u32*        m_Order      = nullptr;

    /** 各種確保元アロケータ。 */
    FAllocator* m_Backing    = nullptr;

    /** Shutdown/Init をまたいでも単調に進めるハンドル世代。 */
    u64         m_NextGeneration = 1u;
};

} // namespace acs
