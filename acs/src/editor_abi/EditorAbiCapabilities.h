// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace acs::editor_abi {

/**
 * Version of the additive editor-host ABI contract.
 *
 * A provider at version N supports every query contract from 1 through N.
 * Individual optional surfaces are negotiated through ECapability instead of
 * being inferred from a product-version string.
 */
inline constexpr std::uint32_t kContractVersion = 1u;

enum class ECapability : std::uint64_t {
    FrameResultContract   = 1ull << 0u,
    IncrementalStartup    = 1ull << 1u,
    ProfilerV3            = 1ull << 2u,
    UnifiedSceneDocument  = 1ull << 3u,
    MaterialPreviewQuality = 1ull << 4u,
    SubstrateGraph        = 1ull << 5u,
    InteractiveWater3D    = 1ull << 6u,
    ResizeResultContract  = 1ull << 7u,
};

[[nodiscard]] constexpr std::uint64_t CapabilityBit(
    ECapability capability) noexcept
{
    return static_cast<std::uint64_t>(capability);
}

inline constexpr std::uint64_t kCapabilities =
    CapabilityBit(ECapability::FrameResultContract) |
    CapabilityBit(ECapability::IncrementalStartup) |
    CapabilityBit(ECapability::ProfilerV3) |
    CapabilityBit(ECapability::UnifiedSceneDocument) |
    CapabilityBit(ECapability::MaterialPreviewQuality) |
    CapabilityBit(ECapability::SubstrateGraph) |
    CapabilityBit(ECapability::InteractiveWater3D) |
    CapabilityBit(ECapability::ResizeResultContract);

inline constexpr std::uint64_t kRequiredManagedHostCapabilities =
    CapabilityBit(ECapability::FrameResultContract) |
    CapabilityBit(ECapability::IncrementalStartup) |
    CapabilityBit(ECapability::ResizeResultContract);

[[nodiscard]] constexpr bool IsCompatible(
    std::uint32_t requested_version,
    std::uint64_t required_capabilities,
    std::uint32_t provided_version = kContractVersion,
    std::uint64_t provided_capabilities = kCapabilities) noexcept
{
    return requested_version != 0u &&
           requested_version <= provided_version &&
           (provided_capabilities & required_capabilities) ==
               required_capabilities;
}

} // namespace acs::editor_abi
