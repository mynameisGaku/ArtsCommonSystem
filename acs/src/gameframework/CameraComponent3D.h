// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gameframework/AComponent.h"
#include "gameframework/Scene3DSerialize.h"

#include <cmath>
#include <cstring>

namespace acs::game {

/**
 * Runtime representation of one authored ACS3D CAM3D record.
 *
 * The owning ANode supplies the live hierarchical pose. This component keeps
 * only stable identity, selection metadata, and projection parameters.
 */
class ACameraComponent3D final : public AComponent {
public:
    ACS_GAME_COMPONENT_KIND(ACameraComponent3D)

    /**
     * Commit a fully validated authored state.
     *
     * Invalid/non-terminated identities and non-finite optics are rejected
     * without modifying the component. This keeps programmatic graph edits as
     * safe as the checked ACS3D parser.
     */
    bool TrySetAuthoredState(const FScene3DCameraState& state) noexcept {
        u32 id_length = 0u;
        for (; id_length <= kScene3DSerializeMaxCameraIdBytes; ++id_length) {
            const char value = state.StableId[id_length];
            if (value == '\0') break;
            const bool alpha =
                (value >= 'A' && value <= 'Z')
                || (value >= 'a' && value <= 'z');
            const bool digit = value >= '0' && value <= '9';
            if (!alpha && !digit
                && (id_length == 0u
                    || (value != '_' && value != '.' && value != '-'))) {
                return false;
            }
        }
        if (id_length == 0u || id_length > kScene3DSerializeMaxCameraIdBytes
            || (state.Projection != EScene3DCameraProjection::Perspective
                && state.Projection != EScene3DCameraProjection::Orthographic)
            || state.Priority < -1000000 || state.Priority > 1000000
            || !std::isfinite(state.FovYDegrees)
            || state.FovYDegrees < 1.0f || state.FovYDegrees > 179.0f
            || !std::isfinite(state.OrthographicHeight)
            || state.OrthographicHeight < 0.001f
            || state.OrthographicHeight > 1000000.0f
            || !std::isfinite(state.NearPlane)
            || state.NearPlane < 0.0001f
            || state.NearPlane > 1000000.0f
            || !std::isfinite(state.FarPlane)
            || state.FarPlane <= state.NearPlane
            || state.FarPlane > 1000000000.0f) {
            return false;
        }

        std::memcpy(
            m_StableId, state.StableId,
            static_cast<usize>(id_length + 1u));
        m_Priority = state.Priority;
        m_Projection = state.Projection;
        m_ActivePreferred = state.IsActivePreferred;
        m_FovYDegrees = state.FovYDegrees;
        m_OrthographicHeight = state.OrthographicHeight;
        m_NearPlane = state.NearPlane;
        m_FarPlane = state.FarPlane;
        return true;
    }

    const char* StableId() const noexcept { return m_StableId; }
    i32 Priority() const noexcept { return m_Priority; }
    EScene3DCameraProjection Projection() const noexcept {
        return m_Projection;
    }
    bool IsActivePreferred() const noexcept { return m_ActivePreferred; }
    f32 FovYDegrees() const noexcept { return m_FovYDegrees; }
    f32 OrthographicHeight() const noexcept {
        return m_OrthographicHeight;
    }
    f32 NearPlane() const noexcept { return m_NearPlane; }
    f32 FarPlane() const noexcept { return m_FarPlane; }

private:
    char m_StableId[kScene3DSerializeMaxCameraIdBytes + 1u]{};
    i32 m_Priority = 0;
    EScene3DCameraProjection m_Projection =
        EScene3DCameraProjection::Perspective;
    bool m_ActivePreferred = false;
    f32 m_FovYDegrees = 60.0f;
    f32 m_OrthographicHeight = 10.0f;
    f32 m_NearPlane = 0.05f;
    f32 m_FarPlane = 1000.0f;
};

} // namespace acs::game
