// SPDX-License-Identifier: Apache-2.0
#include "app/UiFontDefaults.h"

#include "render/Font.h"

namespace acs::UiFontDefaults {

/** UI フォントの探索順を固定する Windows 標準 path。 */
static constexpr const wchar_t* kCandidatePaths[]{L"C:\\Windows\\Fonts\\meiryo.ttc", L"C:\\Windows\\Fonts\\msgothic.ttc", L"C:\\Windows\\Fonts\\YuGothM.ttc", L"C:\\Windows\\Fonts\\arial.ttf", L"C:\\Windows\\Fonts\\segoeui.ttf"};

/** 候補を優先順に読み込み、全候補の失敗時は font を有効化しない。 */
TResult<void> TryLoad(FFont& font, IRhiDevice& device, f32 size_px, u32 atlas_size, bool include_cjk) noexcept
{
    for (const wchar_t* path : kCandidatePaths) {
        // 現在の候補を呼び出し側所有のフォントへ読み込んだ結果。
        const auto result = font.LoadFromFile(device, path, size_px, atlas_size, include_cjk);
        if (result.IsOk()) return Ok();
    }
    return ACS_ERR(Asset, 5, "no default UI font found on this platform");
}

} // namespace acs::UiFontDefaults
