// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace acs::game {

class FInputStateSnapshot;

/** FGame の一フレーム分の固定更新入力を取得する差し替え境界。 */
class IInputFrameSource {
public:
    /** 派生した入力ソースを基底ポインターから安全に破棄する。 */
    virtual ~IInputFrameSource() noexcept = default;

    /**
     * 現在フレームの入力を所有 snapshot として取得する。
     * @param output 取得した入力の書き込み先。失敗時は変更しない。
     * @return 入力を取得できた場合は true。
     */
    virtual bool TryCaptureFrameInput(FInputStateSnapshot& output) noexcept = 0;
};

} // namespace acs::game
