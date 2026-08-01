// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace acs {

/** GameFramework 同梱サブシステムを正規登録簿へ冪等に登録する。 */
[[nodiscard]] bool AcsRegisterGameFrameworkSubsystems() noexcept;

} // namespace acs
