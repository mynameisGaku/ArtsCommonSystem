// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Threading — 排他 FMutex（Win32 SRWLOCK ベース）
// -----------------------------------------------------------------------------
// SRWLOCK の Exclusive モードを使用した最小限の FMutex。
// std::mutex 相当だが、より軽量（CRITICAL_SECTION 不使用、初期化フリー）。
//
// SRWLOCK のサイズは void* 1 個分なので、<windows.h> を露出させずに
// void* 配列として持つ。実装側でキャスト。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"

namespace acs {

class FMutex {
public:
    FMutex() noexcept;                    // SRWLOCK を初期化
    ~FMutex() noexcept = default;         // SRWLOCK は明示的解放不要

    // FMutex はコピー不可（std::mutex と同様）
    FMutex(const FMutex&) = delete;
    FMutex& operator=(const FMutex&) = delete;

    void Lock()    noexcept;             // 排他ロック取得（ブロッキング）
    bool TryLock() noexcept;             // 取得試行（取れたら true、取れなければ false で即時帰還）
    void Unlock()  noexcept;             // ロック解除

private:
    // SRWLOCK 実体。<windows.h> をヘッダで取り込まないため void* で持つ。
    void* m_Srw[1];
};

} // namespace acs
