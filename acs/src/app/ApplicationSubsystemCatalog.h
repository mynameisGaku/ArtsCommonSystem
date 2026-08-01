// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace acs {

/**
 * App 組み込みサブシステムを明示的かつ冪等に登録する。
 * 容量不足では false を返す。成功済み登録は保持され、後続呼び出しで不足分を再試行できる。
 */
bool AcsRegisterApplicationSubsystems() noexcept;

} // namespace acs
