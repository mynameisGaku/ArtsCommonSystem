// プラットフォーム固有の OS ヘッダを 1 箇所に閉じ込め、他のヘッダを軽量に保つ
#pragma once

#include "foundation/Compiler.h"

#if ACS_PLATFORM_WINDOWS
    // 不要な Win32 サブシステムを取り込まない
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    // <windows.h> の min/max マクロを抑制（std::min と衝突するため）
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif
