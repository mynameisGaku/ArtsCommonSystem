// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/Hash.h"
#include "container/String.h"

namespace acs {

/**
 * FString と FStringView を同じ byte hash へ写す異種検索用 hasher。
 *
 * THashMap の H 引数へ指定すると、FindAs(FStringView) で一時 FString を作らない。
 */
struct FStringHasher {
    /**
     * 所有文字列の全 byte を hash 化する。
     *
     * @param Value hash 化する所有文字列。
     * @return HashBytes と同じ 64bit hash。
     */
    ACS_FORCEINLINE u64 operator()(const FString& Value) const noexcept
    {
        return HashBytes(Value.Data(), Value.Size());
    }

    /**
     * 文字列 view の全 byte を hash 化する。
     *
     * @param Value hash 化する非所有 view。
     * @return 所有文字列 overload と同じ 64bit hash。
     */
    ACS_FORCEINLINE u64 operator()(FStringView Value) const noexcept
    {
        return HashBytes(Value.Data(), Value.Size());
    }
};

} // namespace acs
