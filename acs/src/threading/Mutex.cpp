// =============================================================================
// ACS Threading — Mutex 実装
// -----------------------------------------------------------------------------
// Win32 SRWLOCK の薄いラッパ。SRWLOCK は静的初期化フリーかつ非常に軽量。
// =============================================================================
#include "threading/Mutex.h"
#include "foundation/Platform.h"

// SRWLOCK は今のところ void* と同じサイズ。サイズ変更が将来あれば
// ストレージレイアウトを再検討する必要がある（コンパイル時検出）。
static_assert(sizeof(SRWLOCK) == sizeof(void*),
              "SRWLOCK shape changed — Mutex storage layout must be updated");

namespace acs {

// SRWLOCK_INIT 相当の初期化を呼ぶ。
Mutex::Mutex() noexcept {
    InitializeSRWLock(reinterpret_cast<SRWLOCK*>(&_srw[0]));
}

// 排他ロック（書き込み側）取得
void Mutex::Lock() noexcept {
    AcquireSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&_srw[0]));
}

// 排他ロック取得試行
bool Mutex::TryLock() noexcept {
    return TryAcquireSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&_srw[0])) != 0;
}

// 排他ロック解除
void Mutex::Unlock() noexcept {
    ReleaseSRWLockExclusive(reinterpret_cast<SRWLOCK*>(&_srw[0]));
}

} // namespace acs
