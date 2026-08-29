// SPDX-License-Identifier: Apache-2.0
#ifndef ACS_RENDER_RENDERER_FRAME_END_RESULT_H
#define ACS_RENDER_RENDERER_FRAME_END_RESULT_H

#include "foundation/Types.h"

namespace acs {

/** GPU提出と画面提示を分離して保持するフレーム終了結果。 */
struct FRendererFrameEndResult {
    /** BeginFrameごとに発行し、記録候補との対応を一意にする提出ID。 */
    u64 submission_id = 0u;

    /** 記録済み命令をGPUキューへ投入できた場合はtrue。 */
    bool submitted = false;

    /** GPU提出後にスワップチェーンを画面へ提示できた場合はtrue。 */
    bool presented = false;
};

} // namespace acs

#endif // ACS_RENDER_RENDERER_FRAME_END_RESULT_H
