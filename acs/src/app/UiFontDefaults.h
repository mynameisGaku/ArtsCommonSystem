// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"

namespace acs {

class FFont;
class IRhiDevice;

namespace UiFontDefaults {

/**
 * OS 既定 UI フォント候補を優先順に読み込む。
 * @param font 読み込み結果を保持する呼び出し側所有のフォント。
 * @param device グリフ atlas を作成する RHI device。
 * @param size_px 読み込むフォントの pixel size。
 * @param atlas_size グリフ atlas 一辺の pixel 数。
 * @param include_cjk 日本語を含む CJK グリフを atlas へ含める指定。
 * @return 最初に読み込めた候補では成功、全候補の失敗時は Asset error。
 */
TResult<void> TryLoad(FFont& font, IRhiDevice& device, f32 size_px = 18.0f, u32 atlas_size = 1024u, bool include_cjk = false) noexcept;

} // namespace UiFontDefaults
} // namespace acs
