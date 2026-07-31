// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace acs {

/**
 * 型付きイベントが呼び出す関数。
 * @param User 呼び出し元が登録した値。
 * @param Values 配信する値。
 */
template<typename... Arguments>
using TEventCallback = void (*)(void* User, Arguments... Values) noexcept;

} // namespace acs
