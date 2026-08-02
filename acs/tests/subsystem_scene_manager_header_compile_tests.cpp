// SPDX-License-Identifier: Apache-2.0
#include "gameframework/SceneManager.h"

/** SceneManager.hだけで正規ownerと描画型を宣言できることを固定する。 */
void AcceptSceneManagerHeader(
    acs::CSceneManager&, acs::CGame&, acs::FRenderContext&) noexcept;
