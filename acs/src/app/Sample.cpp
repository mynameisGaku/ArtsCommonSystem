// acs::Sample 実装 — デフォルトフォント解決
#include "app/Sample.h"
#include "render/Font.h"

namespace acs::Sample {

const wchar_t* DefaultUIFontPath() noexcept {
#if ACS_PLATFORM_WINDOWS
    return L"C:\\Windows\\Fonts\\meiryo.ttc";
#elif ACS_PLATFORM_MACOS
    return L"/System/Library/Fonts/Helvetica.ttc";
#elif ACS_PLATFORM_LINUX
    return L"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
#else
    return L"";
#endif
}

Result<void> TryLoadDefaultUIFont(Font& font, IRhiDevice& device, f32 size_px) noexcept {
    // OS 別の候補リスト。最初に LoadFromFile が成功したら採用。
    static const wchar_t* const kCandidates[] = {
#if ACS_PLATFORM_WINDOWS
        L"C:\\Windows\\Fonts\\meiryo.ttc",
        L"C:\\Windows\\Fonts\\msgothic.ttc",
        L"C:\\Windows\\Fonts\\YuGothM.ttc",
        L"C:\\Windows\\Fonts\\arial.ttf",
        L"C:\\Windows\\Fonts\\segoeui.ttf",
#elif ACS_PLATFORM_MACOS
        L"/System/Library/Fonts/Helvetica.ttc",
        L"/System/Library/Fonts/HiraginoSans-W3.ttc",
        L"/Library/Fonts/Arial.ttf",
#elif ACS_PLATFORM_LINUX
        L"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        L"/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        L"/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        L"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
#endif
        nullptr,
    };

    for (const wchar_t* const* p = kCandidates; *p; ++p) {
        auto r = font.LoadFromFile(device, *p, size_px);
        if (r.IsOk()) return Ok();
    }
    return ACS_ERR(Asset, 5, "no default UI font found on this platform");
}

} // namespace acs::Sample
