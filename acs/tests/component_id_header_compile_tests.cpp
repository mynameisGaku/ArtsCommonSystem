// SPDX-License-Identifier: Apache-2.0
#include "ecs/ComponentId.h"

/** 単独includeで署名を生成する検査用コンポーネント。 */
struct FHeaderOnlyComponent {};

static_assert(sizeof(acs::FComponentTypeId) == sizeof(acs::ComponentTypeId));
static_assert(sizeof(acs::FComponentSignatureId) == sizeof(acs::ComponentSignatureId));
static_assert(acs::GetComponentSignatureId<FHeaderOnlyComponent>() == acs::TComponentTypeTraits<FHeaderOnlyComponent>::Signature);
