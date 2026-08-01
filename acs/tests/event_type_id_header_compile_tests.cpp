// SPDX-License-Identifier: Apache-2.0
#include "event/EventTypeId.h"

/** 単独includeから得た最大の有効通路番号。 */
constexpr acs::FEventTypeId kHeaderOnlyEventTypeId = acs::kMaxEventTypes - 1u;
static_assert(acs::IsValidEventTypeId(kHeaderOnlyEventTypeId));
static_assert(sizeof(acs::FEventTypeId) == sizeof(acs::EventTypeId));
