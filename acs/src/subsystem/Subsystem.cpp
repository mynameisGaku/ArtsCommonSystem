// SPDX-License-Identifier: Apache-2.0
#include "subsystem/Subsystem.h"

namespace acs {

ASubsystem::~ASubsystem() noexcept = default;

void ASubsystem::OnTickFrame(const FSubsystemFrameContext& context) noexcept
{
    OnTick(context.scaled_delta_seconds);
}

} // namespace acs
