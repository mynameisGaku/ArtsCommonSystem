// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs::asset {

/** .cine のイベント種別を表す値です。 */
enum class ECinematicEventKind : u8 {
    /** 経過時間だけを表す待機イベント。 */
    Wait = 0,
    /** カメラ位置・倍率・補間時間を表すイベント。 */
    MoveCamera = 1,
    /** 所有文字列を表示する会話イベント。 */
    Dialogue = 2,
    /** 所有文字列とフェード時間を持つ音楽イベント。 */
    Music = 3,
    /** 整数識別子を通知するイベント。 */
    FireEvent = 4
};

} // namespace acs::asset
