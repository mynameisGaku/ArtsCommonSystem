// SPDX-License-Identifier: Apache-2.0
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

    // <windows.h> が定義する一般名のマクロを除去（ACS の関数名と衝突するため）
    #ifdef Yield
        #undef Yield                // Thread::Yield と衝突
    #endif
    #ifdef CreateDirectory
        #undef CreateDirectory      // FileSystem::CreateDirectory と衝突
    #endif
    #ifdef CreateFile
        #undef CreateFile
    #endif
    #ifdef SendMessage
        #undef SendMessage
    #endif
    #ifdef PostMessage
        #undef PostMessage
    #endif
    #ifdef GetMessage
        #undef GetMessage
    #endif
    #ifdef DrawText
        #undef DrawText                 // SpriteBatch::DrawText と衝突
    #endif
    #ifdef GetObject
        #undef GetObject
    #endif
    #ifdef LoadImage
        #undef LoadImage
    #endif
    #ifdef GetFirstChild
        #undef GetFirstChild
    #endif
#endif
