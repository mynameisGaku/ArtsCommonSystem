// SPDX-License-Identifier: Apache-2.0
// ACS Threading — FThread 実装
//
// CreateThread のラッパ。トランポリン関数でスレッド名設定 → ユーザー関数呼び出し
// → コンテキスト解放 を行う。
#include "threading/Thread.h"
#include "foundation/Platform.h"
#include "foundation/Move.h"

namespace acs {

namespace {

/** CreateThread に渡す起動コンテキスト (Trampoline 内で消費・解放される一時ヒープ確保)。 */
struct StartCtx {
    /** スレッドで実行するユーザー関数。 */
    ThreadEntry entry;

    /** ユーザー関数に渡す任意データ。 */
    void*       user;

    /** デバッガ表示用のスレッド名 (nullptr 可)。 */
    const wchar_t* name;
};

/**
 * 各スレッドが最初に実行するトランポリン関数。
 *
 * @details
 * エントリ関数を呼ぶ前にスレッド名を設定し、起動コンテキストを解放してから
 * ユーザー関数を呼ぶ。CreateThread の lpStartAddress として渡される。
 * @param arg StartCtx へのポインタ (この関数内で HeapFree される)。
 * @return 常に 0。
 */
DWORD WINAPI Trampoline(LPVOID arg) noexcept {
    StartCtx* ctx = static_cast<StartCtx*>(arg);
    const ThreadEntry e = ctx->entry;
    void* const u       = ctx->user;
    const wchar_t* const name = ctx->name;
    if (name) ::SetThreadDescription(::GetCurrentThread(), name);
    ::HeapFree(::GetProcessHeap(), 0, ctx);  // ctx はもう不要
    e(u);                                     // ユーザー関数本体
    return 0;
}

} // namespace

/** 現在のスレッドの ID を返す (GetCurrentThreadId のラッパ)。 */
ThreadId CurrentThreadId() noexcept {
    return ThreadId{ static_cast<u32>(::GetCurrentThreadId()) };
}

/** 現在のスレッドを指定ミリ秒スリープさせる。 */
void SleepMs(u32 ms) noexcept { ::Sleep(static_cast<DWORD>(ms)); }

/** 現在のスレッドが CPU を放棄し、同優先度の他スレッドへ譲渡する。 */
void Yield() noexcept         { ::SwitchToThread(); }

/** 論理 CPU 数を返す (取得不能なら 1)。 */
u32 HardwareConcurrency() noexcept {
    SYSTEM_INFO si {};
    ::GetSystemInfo(&si);
    return si.dwNumberOfProcessors == 0 ? 1 : si.dwNumberOfProcessors;
}

/** ハンドルが残っていれば閉じる (Detach 相当、スレッド終了は待たない)。 */
FThread::~FThread() noexcept {
    if (m_Handle) ::CloseHandle(m_Handle);
}

/** ムーブ構築。ハンドル所有権を移譲し、相手を空にする。 */
FThread::FThread(FThread&& other) noexcept : m_Handle(other.m_Handle), m_Id(other.m_Id) {
    other.m_Handle = nullptr;
    other.m_Id     = {};
}

/** ムーブ代入。自身のハンドルを閉じてから所有権を移譲する。 */
FThread& FThread::operator=(FThread&& other) noexcept {
    if (this == &other) return *this;
    if (m_Handle) ::CloseHandle(m_Handle);
    m_Handle       = other.m_Handle;
    m_Id           = other.m_Id;
    other.m_Handle = nullptr;
    other.m_Id     = {};
    return *this;
}

/** スレッドを生成して起動する。Trampoline 経由で entry を呼ぶ。 */
TResult<FThread> FThread::Spawn(ThreadEntry entry, void* user, const ThreadConfig& cfg) noexcept {
    if (!entry) return ACS_ERR(Threading, 1, "FThread::Spawn called with null entry");

    // ユーザー関数情報を保持する一時オブジェクトをヒープに確保
    auto* ctx = static_cast<StartCtx*>(::HeapAlloc(::GetProcessHeap(), 0, sizeof(StartCtx)));
    if (!ctx) return ACS_ERR(Memory, 1, "FThread::Spawn HeapAlloc failed");
    ctx->entry = entry;
    ctx->user  = user;
    ctx->name  = cfg.name;

    DWORD tid = 0;
    HANDLE h = ::CreateThread(nullptr,
                              static_cast<SIZE_T>(cfg.stack_bytes),
                              &Trampoline, ctx, 0, &tid);
    if (!h) {
        const DWORD err = ::GetLastError();
        ::HeapFree(::GetProcessHeap(), 0, ctx);
        return ACS_ERR_OS(OS, 1, "CreateThread failed", err);
    }
    // 任意設定: 優先度とアフィニティ
    if (cfg.priority != 0) ::SetThreadPriority(h, cfg.priority);
    if (cfg.affinity != 0) ::SetThreadAffinityMask(h, static_cast<DWORD_PTR>(cfg.affinity));

    FThread t;
    t.m_Handle = h;
    t.m_Id     = ThreadId{ static_cast<u32>(tid) };
    return TResult<FThread>(OkInit, Move(t));
}

/** スレッド終了までブロックして待機し、ハンドルを閉じる。 */
void FThread::Join() noexcept {
    if (!m_Handle) return;
    ::WaitForSingleObject(m_Handle, INFINITE);
    ::CloseHandle(m_Handle);
    m_Handle = nullptr;
}

/** ハンドルだけ閉じてスレッドは独立して継続させる。 */
void FThread::Detach() noexcept {
    if (m_Handle) {
        ::CloseHandle(m_Handle);
        m_Handle = nullptr;
    }
}

} // namespace acs
