// =============================================================================
// ACS Foundation — プラットフォーム固有ヘッダの集約
// -----------------------------------------------------------------------------
// 役割: <windows.h> など重量級の OS ヘッダを 1 箇所に閉じ込め、他のヘッダを
//       軽量に保つ。WIN32_LEAN_AND_MEAN / NOMINMAX を必ず先行定義することで、
//       ヘッダ汚染（min/max マクロ衝突など）を未然に防ぐ。
// =============================================================================
#pragma once

#include "foundation/Compiler.h"

#if ACS_PLATFORM_WINDOWS
    // 不要な Win32 サブシステム（GDI / WINSOCK 旧版など）を取り込まない
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    // <windows.h> が定義する min/max マクロを抑制（C++ の std::min と衝突するため）
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif
