// SPDX-License-Identifier: Apache-2.0
// FLocalization 実装
#include "platform/Localization.h"
#include "foundation/Move.h"

namespace acs {

const char* FLocalization::Tr(const char* key) const noexcept {
    if (!key) return "";
    if (_active.Has(key))   return _active.GetString(key, "");
    if (_fallback.Has(key)) return _fallback.GetString(key, "");
    return key;
}

bool FLocalization::Has(const char* key) const noexcept {
    if (!key) return false;
    return _active.Has(key) || _fallback.Has(key);
}

void FLocalization::Swap() noexcept {
    FStorage tmp = Move(_active);
    _active     = Move(_fallback);
    _fallback   = Move(tmp);
}

} // namespace acs
