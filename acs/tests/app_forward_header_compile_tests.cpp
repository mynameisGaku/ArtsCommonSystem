// SPDX-License-Identifier: Apache-2.0
#include "app/Forward.h"

#include <type_traits>

/** Appの正規名と旧公開名が単独headerから同じ型として見えることを固定する。 */
static_assert(std::is_same_v<acs::CApplication, acs::FApplication>);
