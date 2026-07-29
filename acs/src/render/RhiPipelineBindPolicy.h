// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "render/RhiPipelineBindDomain.h"

namespace acs {

/** 領域ごとのパイプライン束縛判定をコンパイル時に特殊化する。 */
template<ERhiPipelineBindDomain Domain>
struct TRhiPipelineBindPolicy {
    /** パイプライン種別を受理するか返す。 */
    static constexpr bool Accepts(bool is_compute) noexcept {
        if constexpr (Domain == ERhiPipelineBindDomain::Compute) return is_compute;
        return !is_compute;
    }

    /** 現在値と要求値から再束縛が必要か返す。 */
    template<typename Pipeline>
    static constexpr bool NeedsBind(const Pipeline* current, const Pipeline* requested) noexcept {
        return requested != nullptr && current != requested;
    }
};

} // namespace acs
