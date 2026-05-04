// =============================================================================
// ACS Threading — 排他 Mutex（Win32 SRWLOCK ベース）
// -----------------------------------------------------------------------------
// SRWLOCK の Exclusive モードを使用した最小限の Mutex。
// std::mutex 相当だが、より軽量（CRITICAL_SECTION 不使用、初期化フリー）。
//
// SRWLOCK のサイズは void* 1 個分なので、<windows.h> を露出させずに
// void* 配列として持つ。実装側でキャスト。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"

namespace acs {

class Mutex {
public:
    Mutex() noexcept;                    // SRWLOCK を初期化
    ~Mutex() noexcept = default;         // SRWLOCK は明示的解放不要

    // Mutex はコピー不可（std::mutex と同様）
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void Lock()    noexcept;             // 排他ロック取得（ブロッキング）
    bool TryLock() noexcept;             // 取得試行（取れたら true、取れなければ false で即時帰還）
    void Unlock()  noexcept;             // ロック解除

private:
    // 内部実装は OS 別 (.cpp で SRWLOCK / pthread_mutex_t)。
    // pthread_mutex_t は glibc/musl/macOS で 40〜64 byte。広めに 8 ポインタ分確保。
    void* _impl[8];
};

} // namespace acs
