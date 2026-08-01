// SPDX-License-Identifier: Apache-2.0
#include "foundation/LogSinkSubscription.h"

#include "foundation/Log.h"

namespace acs {

/** 保持中の購読を解除する。 */
FLogSinkSubscription::~FLogSinkSubscription() noexcept
{
    Reset();
}

/** 購読の所有権を移す。 */
FLogSinkSubscription::FLogSinkSubscription(FLogSinkSubscription&& other) noexcept : m_Handle(other.m_Handle)
{
    other.m_Handle = {};
}

/** 現在の購読を解除して所有権を移す。 */
FLogSinkSubscription& FLogSinkSubscription::operator=(FLogSinkSubscription&& other) noexcept
{
    if (this == &other) return *this;
    Reset();
    m_Handle = other.m_Handle;
    other.m_Handle = {};
    return *this;
}

/** 保持中の購読が現在の Logger 世代でも有効かを返す。 */
bool FLogSinkSubscription::IsValid() const noexcept
{
    return CLogger::IsSinkSubscribed(m_Handle);
}

/** 保持中の購読を解除し、解除できたかを返す。 */
bool FLogSinkSubscription::Reset() noexcept
{
    const FLogSinkHandle handle = m_Handle;
    m_Handle = {};
    return CLogger::UnsubscribeSink(handle);
}

} // namespace acs
