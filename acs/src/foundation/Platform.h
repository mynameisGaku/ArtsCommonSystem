// ACS Foundation — Platform-specific narrow includes.
// Centralizes <windows.h> inclusion so other headers stay clean.
#pragma once

#include "foundation/Compiler.h"

#if ACS_PLATFORM_WINDOWS
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif
