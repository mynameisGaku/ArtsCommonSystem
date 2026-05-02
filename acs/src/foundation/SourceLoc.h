// ACS Foundation — Source location (no <source_location>).
// We re-implement the C++20 std::source_location interface using compiler
// builtins. This avoids dragging in the STL header while preserving the
// same ergonomic default-argument capture pattern.
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"

namespace acs {

class SourceLoc {
public:
    constexpr SourceLoc() noexcept = default;

    constexpr const char* File()     const noexcept { return _file; }
    constexpr const char* Function() const noexcept { return _func; }
    constexpr u32         Line()     const noexcept { return _line; }
    constexpr u32         Column()   const noexcept { return _col;  }

    static consteval SourceLoc Current(
        const char* file = __builtin_FILE(),
        const char* func = __builtin_FUNCTION(),
        u32 line         = __builtin_LINE(),
#if ACS_COMPILER_MSVC || ACS_COMPILER_CLANG
        u32 col          = __builtin_COLUMN()
#else
        u32 col          = 0
#endif
    ) noexcept {
        SourceLoc s;
        s._file = file;
        s._func = func;
        s._line = line;
        s._col  = col;
        return s;
    }

private:
    const char* _file = "";
    const char* _func = "";
    u32         _line = 0;
    u32         _col  = 0;
};

} // namespace acs
