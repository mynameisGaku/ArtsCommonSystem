// SPDX-License-Identifier: Apache-2.0
#include "memory/AObject.h"

namespace {

/** AObject.hだけをインクルードして宣言できる検査用派生型。 */
class AHeaderOnlyObject final : public acs::AObject {};

/** 正式名から旧互換名へのポインタ変換を検証する。 */
constexpr acs::FObject* AsLegacyObject(acs::AObject* Object) noexcept { return Object; }

/** 旧互換名から正式名へのポインタ変換を検証する。 */
constexpr acs::AObject* AsCanonicalObject(acs::FObject* Object) noexcept { return Object; }

static_assert(AsCanonicalObject(AsLegacyObject(nullptr)) == nullptr);
static_assert(sizeof(AHeaderOnlyObject) >= sizeof(acs::AObject));

} // namespace
