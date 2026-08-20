// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace acs::game {

class FInputStateSnapshot;

/** CGameの描画フレーム入力を取得し、次の固定更新まで蓄積する差し替え境界。 */
class IInputFrameSource {
public:
    /** 派生した入力ソースを基底ポインターから安全に破棄する。 */
    virtual ~IInputFrameSource() noexcept = default;

    /** 現在フレームの入力を所有snapshotとして取得し、失敗時は出力を変更しない。 */
    virtual bool TryCaptureFrameInput(FInputStateSnapshot& output) noexcept = 0;
};

} // namespace acs::game
