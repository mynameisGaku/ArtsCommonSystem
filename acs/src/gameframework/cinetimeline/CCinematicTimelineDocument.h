// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"
#include "gameframework/cinetimeline/FCinematicTimelineKeyframe.h"
#include "memory/Allocator.h"

namespace acs::game {
class CCinematicsDirector;
namespace cinetimeline {

/** 編集キー列と時間を検証付きで所有する非多相の文書です。 */
class CCinematicTimelineDocument {
public:
    /** 編集時間の初期値です。 */
    static constexpr f32 kDefaultDurationSec = 10.0f;
    /** 受け入れる時間の下限です。 */
    static constexpr f32 kMinDurationSec = 0.1f;

    /** 既定アロケーターで空の文書を作ります。 */
    CCinematicTimelineDocument() noexcept = default;
    /** 指定アロケーターで空の文書を作ります。 */
    explicit CCinematicTimelineDocument(IAllocator& allocator) noexcept;
    /** 保持列を解放します。 */
    ~CCinematicTimelineDocument() noexcept = default;
    /** 文書の複製を禁止します。 */
    CCinematicTimelineDocument(const CCinematicTimelineDocument&) = delete;
    /** 文書の複製代入を禁止します。 */
    CCinematicTimelineDocument& operator=(const CCinematicTimelineDocument&) = delete;
    /** 文書の移動を禁止します。 */
    CCinematicTimelineDocument(CCinematicTimelineDocument&&) = delete;
    /** 文書の移動代入を禁止します。 */
    CCinematicTimelineDocument& operator=(CCinematicTimelineDocument&&) = delete;

    /** キー列と時間を初期値へ戻します。 */
    void Reset() noexcept;
    /** 現在の明示的な再生時間を返します。 */
    f32 DurationSec() const noexcept { return m_DurationSec; }
    /** 有限な再生時間を設定し、失敗時は文書を変更しません。 */
    bool TrySetDurationSec(f32 duration_sec) noexcept;
    /** 保持しているキー数を返します。 */
    u32 KeyframeCount() const noexcept { return static_cast<u32>(m_Keyframes.Num()); }
    /** キー列を読み取り専用の範囲で返します。 */
    TSpan<const FCinematicTimelineKeyframe> Keyframes() const noexcept { return m_Keyframes.AsSpan(); }
    /** 検証済みキーを安定順で追加し、失敗時は列を変更しません。 */
    bool TryAdd(const FCinematicTimelineKeyframe& keyframe, u32* out_index = nullptr) noexcept;
    /** 指定位置のキーを削除し、範囲外では失敗します。 */
    bool TryRemove(u32 index) noexcept;
    /** キーを検証して置換し、時刻変更時は安定順へ再配置します。 */
    bool TryReplace(u32 index, const FCinematicTimelineKeyframe& keyframe, u32* out_index = nullptr) noexcept;
    /** 再生列を一時構築してディレクターへ反映し、失敗時は既存状態を保ちます。 */
    bool TryBakeTo(::acs::game::CCinematicsDirector& director) noexcept;

private:
    /** 種類に応じた有限値と時間範囲を検証します。 */
    bool ValidateKeyframe(const FCinematicTimelineKeyframe& keyframe) const noexcept;
    /** 編集キーを保持する列です。 */
    TArray<FCinematicTimelineKeyframe> m_Keyframes;
    /** 明示的な再生時間です。 */
    f32 m_DurationSec = kDefaultDurationSec;
};

}
}
