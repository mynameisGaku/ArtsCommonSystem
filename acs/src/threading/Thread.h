// SPDX-License-Identifier: Apache-2.0
// ACS Threading — スレッド生成・操作（std::thread 代替）
//
// CreateThread を直接ラップ。STL の std::thread と異なり例外を投げず、失敗は
// TResult<FThread, FErrorCode> で返す。
//
// 主な機能:
//   FThread::Spawn     — エントリ関数を別スレッドで起動
//   FThread::Join      — スレッド終了を待機
//   FThread::Detach    — スレッドハンドルを切り離す
//   SleepMs / Yield   — 現在スレッドの停止・譲渡
//   HardwareConcurrency — 論理 CPU 数（FThreadPool のデフォルト worker 数等で使用）
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "threading/ThreadId.h"

namespace acs {

/** スレッドのエントリポイント関数型。引数 user には Spawn 時の任意ポインタが渡る。 */
using ThreadEntry = void (*)(void* user);

/** スレッド生成オプション。 */
struct ThreadConfig {
    /** デバッガ表示用のスレッド名 (nullptr で無名)。 */
    const wchar_t* name        = nullptr;

    /** スタックサイズ (バイト、0 = OS デフォルト)。 */
    u32            stack_bytes = 0;

    /** スレッド優先度 (-2..+2、THREAD_PRIORITY_* 相当、0 = 通常)。 */
    i32            priority    = 0;

    /** CPU アフィニティマスク (0 = ピン留めなし)。 */
    u64            affinity    = 0;
};

/**
 * RAII で OS スレッドハンドルを所有するスレッド型 (std::thread 代替)。
 *
 * @details
 * CreateThread を直接ラップする。コピー不可・ムーブ可で、デストラクタはハンドルが
 * 残っていれば CloseHandle する (Detach 相当で、スレッドの終了は待たない)。
 */
class FThread {
public:
    /** 空のハンドル (スレッドなし) を構築する。 */
    FThread() noexcept = default;

    /** ハンドルが残っていれば CloseHandle する (スレッド終了は待たない)。 */
    ~FThread() noexcept;

    /** コピー禁止 (OS ハンドルを単独所有するため)。 */
    FThread(const FThread&) = delete;

    /** コピー代入も禁止。 */
    FThread& operator=(const FThread&) = delete;

    /**
     * ムーブ構築する。ハンドル所有権を奪い、相手を空にする。
     *
     * @param other ムーブ元 (実行後は空ハンドルになる)。
     */
    FThread(FThread&& other) noexcept;

    /**
     * ムーブ代入する。自身が持つハンドルを閉じてから所有権を奪う。
     *
     * @param other ムーブ元 (実行後は空ハンドルになる)。
     * @return 自身への参照。
     */
    FThread& operator=(FThread&& other) noexcept;

    /**
     * 新しいスレッドを起動する。
     *
     * @details トランポリン経由で entry を呼ぶ。entry が null なら Threading エラー。
     * @param entry スレッドで実行するエントリ関数。
     * @param user entry に渡す任意ポインタ。
     * @param cfg スレッド生成オプション (名前・スタック・優先度・アフィニティ)。
     * @return 起動した FThread。失敗時は Memory / OS / Threading カテゴリのエラー。
     */
    static TResult<FThread> Spawn(ThreadEntry entry, void* user,
                                const ThreadConfig& cfg = {}) noexcept;

    /**
     * Join 可能か (有効なハンドルを保持しているか) を返す。
     *
     * @return 有効なハンドルを持つなら true。
     */
    bool Joinable() const noexcept { return m_Handle != nullptr; }

    /** スレッド終了までブロックして待機し、その後ハンドルを閉じる (空ハンドルなら何もしない)。 */
    void Join() noexcept;

    /** ハンドルだけ閉じてスレッドは独立して継続させる (終了は待たない)。 */
    void Detach() noexcept;

    /**
     * このスレッドの ID を返す。
     *
     * @return スレッド ID (空ハンドルなら既定値)。
     */
    ThreadId Id() const noexcept { return m_Id; }

private:
    /** OS スレッドハンドル (CloseHandle 対象、null = スレッドなし)。 */
    void*    m_Handle = nullptr;

    /** 生成したスレッドの ID。 */
    ThreadId m_Id     = {};
};

/**
 * 現在のスレッドを指定ミリ秒スリープさせる。
 *
 * @param ms スリープするミリ秒数。
 */
void SleepMs(u32 ms) noexcept;

/** 現在のスレッドが CPU を放棄し、同優先度の実行可能な他スレッドへ譲渡する。 */
void Yield() noexcept;

/**
 * 論理 CPU 数を返す。
 *
 * @return 論理プロセッサ数 (取得できなければ 1)。
 */
u32  HardwareConcurrency() noexcept;

} // namespace acs
