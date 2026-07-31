// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "event/MulticastDelegate.h"

namespace acs {

/** 引数なしの複数処理を登録順または優先順に呼び出す。 */
using FSimpleMulticastDelegate = TMulticastDelegate<void()>;

} // namespace acs
