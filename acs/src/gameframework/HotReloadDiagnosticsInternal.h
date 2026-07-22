// SPDX-License-Identifier: Apache-2.0
// FHotReloadWatcher の native 失敗経路を決定論的に検証するための内部 API。
#pragma once

#include "foundation/Types.h"

namespace acs::game::internal {

#if defined(ACS_GAMEFRAMEWORK_TEST_HOOKS) && !defined(ACS_GAME_SHIPPING)

// 次の TryTick に 1 回だけ注入する native watcher の状態。
//
// 製品 API からは使用しない。SyntheticBuffer 系は設定時に byte 列を内部固定長
// buffer へコピーするため、呼び出し側 buffer の寿命には依存しない。
enum class EHotReloadNativeFaultForTesting : u8 {
    None,
    NativeOverflow,
    RearmFailure,
    SyntheticBuffer,
    SyntheticBufferConversionOutOfMemory,
};

// 次の有効な TryTick へ native 状態を 1 回だけ設定する。
//
// SyntheticBuffer 系では bytes と byte_count が必須。それ以外では両方を空にする。
// 未消費の注入が既にある場合や引数が不正な場合は false を返し、既存注入を維持する。
bool ConfigureHotReloadNativeFaultForTesting(
    EHotReloadNativeFaultForTesting fault,
    const void* bytes = nullptr,
    usize byte_count = 0u) noexcept;

// 未消費の native 失敗注入を破棄する。
void ResetHotReloadNativeFaultForTesting() noexcept;

#endif

} // namespace acs::game::internal
