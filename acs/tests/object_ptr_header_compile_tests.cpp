// SPDX-License-Identifier: Apache-2.0
#include "memory/ObjectPtr.h"

namespace {

/** ObjectPtr.h旧入口とFObject旧名だけで宣言できる検査用オブジェクト。 */
class AObjectPtrHeaderObject final : public acs::FObject {};

static_assert(sizeof(acs::TObjectPtr<AObjectPtrHeaderObject>) == sizeof(void*) * 2u);
static_assert(sizeof(acs::TWeakObjectPtr<AObjectPtrHeaderObject>) == sizeof(void*) * 2u);

} // namespace
