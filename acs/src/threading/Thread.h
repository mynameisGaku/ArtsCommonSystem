// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Threading — スレッド生成・操作（std::thread 代替）
// -----------------------------------------------------------------------------
// CreateThread を直接ラップ。STL の std::thread と異なり例外を投げず、失敗は
// TResult<FThread, FErrorCode> で返す。
//
// 主な機能:
//   FThread::Spawn     — エントリ関数を別スレッドで起動
//   FThread::Join      — スレッド終了を待機
//   FThread::Detach    — スレッドハンドルを切り離す
//   SleepMs / Yield   — 現在スレッドの停止・譲渡
//   HardwareConcurrency — 論理 CPU 数（FThreadPool のデフォルト worker 数等で使用）
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "threading/ThreadId.h"

namespace acs {

// スレッドのエントリポイント関数型。user は Spawn 時の任意ポインタ。
using ThreadEntry = void (*)(void* user);

// スレッド生成オプション
struct ThreadConfig {
    const wchar_t* name        = nullptr;  // デバッガ表示用の名前
    u32            stack_bytes = 0;        // スタックサイズ (0 = OS デフォルト)
    i32            priority    = 0;        // 優先度 (-2..+2, THREAD_PRIORITY_*)
    u64            affinity    = 0;        // CPU アフィニティマスク (0 = ピン留めなし)
};

// =============================================================================
// FThread — RAII スレッドハンドル
// =============================================================================
class FThread {
public:
    FThread() noexcept = default;
    ~FThread() noexcept;                    // ハンドルが残っていれば CloseHandle

    // コピー不可、ムーブ可
    FThread(const FThread&) = delete;
    FThread& operator=(const FThread&) = delete;
    FThread(FThread&& other) noexcept;
    FThread& operator=(FThread&& other) noexcept;

    // スレッドを起動。失敗時は Err（Memory / OS カテゴリ）。
    static TResult<FThread> Spawn(ThreadEntry entry, void* user,
                                const ThreadConfig& cfg = {}) noexcept;

    bool Joinable() const noexcept { return m_Handle != nullptr; }
    void Join() noexcept;                  // 終了まで待機（その後ハンドル解放）
    void Detach() noexcept;                // ハンドルだけ閉じてスレッドは継続

    ThreadId Id() const noexcept { return m_Id; }

private:
    void*    m_Handle = nullptr;            // HANDLE (CloseHandle 対象)
    ThreadId m_Id     = {};
};

// ---- フリー関数 ----------------------------------------------------------
void SleepMs(u32 ms) noexcept;             // 指定ミリ秒スリープ
void Yield() noexcept;                     // 同優先度の他スレッドへ譲渡
u32  HardwareConcurrency() noexcept;       // 論理 CPU 数

} // namespace acs
