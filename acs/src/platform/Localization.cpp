// SPDX-License-Identifier: Apache-2.0
// Localization 実装
#include "platform/Localization.h"
#include "foundation/Move.h"

namespace acs {

const char* Localization::Tr(const char* key) const noexcept {
    if (!key) return "";
    if (m_Active.Has(key))   return m_Active.GetString(key, "");
    if (m_Fallback.Has(key)) return m_Fallback.GetString(key, "");
    return key;
}

bool Localization::Has(const char* key) const noexcept {
    if (!key) return false;
    return m_Active.Has(key) || m_Fallback.Has(key);
}

void Localization::Swap() noexcept {
    Storage tmp = Move(m_Active);
    m_Active     = Move(m_Fallback);
    m_Fallback   = Move(tmp);
}

} // namespace acs
