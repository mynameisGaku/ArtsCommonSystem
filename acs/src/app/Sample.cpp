// acs::Sample 実装 — デフォルトフォント解決
#include "app/Sample.h"
#include "render/Font.h"

namespace acs::Sample {

const wchar_t* DefaultUIFontPath() noexcept {
    return L"C:\\Windows\\Fonts\\meiryo.ttc";
}

Result<void> TryLoadDefaultUIFont(Font& font, IRhiDevice& device, f32 size_px,
                                   u32 atlas_size, bool include_cjk) noexcept {
    // Windows 標準フォント候補。最初に LoadFromFile が成功したら採用。
    static const wchar_t* const kCandidates[] = {
        L"C:\\Windows\\Fonts\\meiryo.ttc",
        L"C:\\Windows\\Fonts\\msgothic.ttc",
        L"C:\\Windows\\Fonts\\YuGothM.ttc",
        L"C:\\Windows\\Fonts\\arial.ttf",
        L"C:\\Windows\\Fonts\\segoeui.ttf",
        nullptr,
    };

    for (const wchar_t* const* p = kCandidates; *p; ++p) {
        auto r = font.LoadFromFile(device, *p, size_px, atlas_size, include_cjk);
        if (r.IsOk()) return Ok();
    }
    return ACS_ERR(Asset, 5, "no default UI font found on this platform");
}

} // namespace acs::Sample
