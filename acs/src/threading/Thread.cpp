// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Threading — FThread 実装
// -----------------------------------------------------------------------------
// CreateThread のラッパ。トランポリン関数で TLS 設定 → ユーザー関数呼び出し
// → コンテキスト解放 を行う。
// =============================================================================
#include "threading/Thread.h"
#include "foundation/Platform.h"
#include "foundation/Move.h"

namespace acs {

namespace {

// CreateThread に渡すコンテキスト（一時的にヒープ確保）
struct StartCtx {
    ThreadEntry entry;       // ユーザー関数
    void*       user;        // ユーザーデータ
    const wchar_t* name;     // デバッガ名（任意）
};

// 全スレッドの最初に実行される関数。エントリ関数を呼ぶ前に
// スレッド名を設定し、コンテキストを解放する。
DWORD WINAPI Trampoline(LPVOID arg) noexcept {
    StartCtx* ctx = static_cast<StartCtx*>(arg);
    ThreadEntry e = ctx->entry;
    void* u       = ctx->user;
    const wchar_t* name = ctx->name;
    if (name) ::SetThreadDescription(::GetCurrentThread(), name);
    ::HeapFree(::GetProcessHeap(), 0, ctx);  // ctx はもう不要
    e(u);                                     // ユーザー関数本体
    return 0;
}

} // namespace

FThreadId CurrentThreadId() noexcept {
    return FThreadId{ static_cast<u32>(::GetCurrentThreadId()) };
}

void SleepMs(u32 ms) noexcept { ::Sleep(static_cast<DWORD>(ms)); }
void Yield() noexcept         { ::SwitchToThread(); }

u32 HardwareConcurrency() noexcept {
    SYSTEM_INFO si {};
    ::GetSystemInfo(&si);
    return si.dwNumberOfProcessors == 0 ? 1 : si.dwNumberOfProcessors;
}

// デストラクタ: ハンドルが残っていれば閉じる（Detach 相当）
FThread::~FThread() noexcept {
    if (_handle) ::CloseHandle(_handle);
}

// ムーブ: ハンドル所有権を移譲
FThread::FThread(FThread&& other) noexcept : _handle(other._handle), _id(other._id) {
    other._handle = nullptr;
    other._id     = {};
}
FThread& FThread::operator=(FThread&& other) noexcept {
    if (this == &other) return *this;
    if (_handle) ::CloseHandle(_handle);
    _handle       = other._handle;
    _id           = other._id;
    other._handle = nullptr;
    other._id     = {};
    return *this;
}

// スレッドを生成して起動する。Trampoline 経由で entry を呼ぶ。
TResult<FThread> FThread::Spawn(ThreadEntry entry, void* user, const FThreadConfig& cfg) noexcept {
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
        DWORD err = ::GetLastError();
        ::HeapFree(::GetProcessHeap(), 0, ctx);
        return ACS_ERR_OS(OS, 1, "CreateThread failed", err);
    }
    // 任意設定: 優先度とアフィニティ
    if (cfg.priority != 0) ::SetThreadPriority(h, cfg.priority);
    if (cfg.affinity != 0) ::SetThreadAffinityMask(h, static_cast<DWORD_PTR>(cfg.affinity));

    FThread t;
    t._handle = h;
    t._id     = FThreadId{ static_cast<u32>(tid) };
    return TResult<FThread>(OkInit, Move(t));
}

// スレッド終了まで待機し、ハンドルを閉じる。
void FThread::Join() noexcept {
    if (!_handle) return;
    ::WaitForSingleObject(_handle, INFINITE);
    ::CloseHandle(_handle);
    _handle = nullptr;
}

// ハンドルだけ閉じてスレッドは独立して継続する。
void FThread::Detach() noexcept {
    if (_handle) {
        ::CloseHandle(_handle);
        _handle = nullptr;
    }
}

} // namespace acs
