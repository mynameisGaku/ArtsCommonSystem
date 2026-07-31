// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "event/Delegate.h"

namespace acs {

/** 引数なしの処理を関数または対象へ結び付ける。 */
using FSimpleDelegate = TDelegate<void()>;

} // namespace acs
