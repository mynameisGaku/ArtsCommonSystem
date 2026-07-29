// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

#include <cstddef>

namespace acs {

/**
 * 世代付きハンドルの物理配置情報を公開する trait。
 *
 * @details 各ハンドル固有の無効値・生存判定・再利用規則は統一せず、ABI 監査に必要な
 * identity と generation の位置・幅だけを特殊化側から提供する。
 * @tparam T 検査対象のハンドル型。
 */
template<typename T>
struct TGenerationHandleLayoutTraits {
    /** 対象型に物理配置 trait が定義されているか。 */
    static constexpr bool kAvailable = false;
};

} // namespace acs
