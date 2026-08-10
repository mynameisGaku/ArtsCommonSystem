// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "asset/Asset.h"
#include "asset/cinematics/FCinematicEvent.h"
#include "container/Array.h"
#include "foundation/Result.h"
#include "memory/SharedPtr.h"

namespace acs::asset {

/** 検証済みイベント列と明示時間を所有する不変アセットです。 */
class ACinematicAsset final : public AAsset {
public:
    ACS_ASSET_TYPE("FCinematicAsset")

    /** assetが保持できる最大イベント数です。 */
    static constexpr u32 kMaxEvents = 4096u;

    /** DialogueとMusicが保持できる最大UTF-8 bytes数です。 */
    static constexpr u32 kMaxTextBytes = 65535u;

    /** 入力イベントとdurationを検証してアセットを生成し、不正値または確保失敗時はerrorを返します。 */
    static TResult<TSharedPtr<ACinematicAsset>> TryCreate(TArray<FCinematicEvent>&& events, f32 duration_sec) noexcept;

    /** 検証済みイベント列を読み取り専用で返します。 */
    const TArray<FCinematicEvent>& Events() const noexcept
    {
        return m_Events;
    }

    /** イベント数を返します。 */
    u32 EventCount() const noexcept
    {
        return static_cast<u32>(m_Events.Num());
    }

    /** 保存された明示時間を返します。 */
    f32 DurationSec() const noexcept
    {
        return m_DurationSec;
    }

private:
    template<typename T, typename... Args>
    friend TSharedPtr<T> acs::MakeShared(Args&&...) noexcept;
    template<typename T, typename... Args>
    friend TSharedPtr<T> acs::MakeSharedIn(IAllocator&, Args&&...) noexcept;

    /** 検証済み配列と時間だけを受け取って初期化します。 */
    ACinematicAsset(TArray<FCinematicEvent>&& events, f32 duration_sec) noexcept;

    /** イベントと明示時間の不変条件を確認します。 */
    static bool Validate(const TArray<FCinematicEvent>& events, f32 duration_sec) noexcept;

    /** 検証済みイベントを所有します。 */
    TArray<FCinematicEvent> m_Events;

    /** イベント末尾以上の明示再生時間です。 */
    f32 m_DurationSec = 0.0f;
};

} // namespace acs::asset
