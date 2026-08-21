// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs::game {

/** 3D collision world内のshapeを世代付きで識別する32bit handle。 */
struct FCollisionShapeId3D {
    /** pack済み値。0は無効、下位24bitはslot index、上位8bitはgeneration。 */
    u32 Packed = 0u;

    /** 無効handleを構築する。 */
    constexpr FCollisionShapeId3D() noexcept = default;

    /**
     * slot indexとgenerationからhandleを構築する。
     *
     * @param index shape slotのindex。
     * @param generation slot再利用を識別する世代番号。
     */
    constexpr FCollisionShapeId3D(u32 index, u8 generation) noexcept
        : Packed((index & 0x00FFFFFFu) | (static_cast<u32>(generation) << 24u))
    {
    }

    /** slot indexを返す。 */
    constexpr u32 Index() const noexcept
    {
        return Packed & 0x00FFFFFFu;
    }

    /** generationを返す。 */
    constexpr u8 Generation() const noexcept
    {
        return static_cast<u8>(Packed >> 24u);
    }

    /** 無効値でなければtrueを返す。world内で生存しているかはIsAliveで別に確認する。 */
    constexpr bool IsValid() const noexcept
    {
        return Packed != 0u;
    }

    /** indexとgenerationが等しければtrueを返す。 */
    constexpr bool operator==(FCollisionShapeId3D other) const noexcept
    {
        return Packed == other.Packed;
    }

    /** indexまたはgenerationが異なればtrueを返す。 */
    constexpr bool operator!=(FCollisionShapeId3D other) const noexcept
    {
        return Packed != other.Packed;
    }
};

} // namespace acs::game
