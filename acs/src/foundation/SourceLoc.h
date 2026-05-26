// SPDX-License-Identifier: Apache-2.0
// std::source_location 相当をコンパイラ組み込みで実装する
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"

namespace acs {

class FSourceLoc {
public:
    constexpr FSourceLoc() noexcept = default;

    constexpr const char* File()     const noexcept { return _file; }
    constexpr const char* Function() const noexcept { return _func; }
    constexpr u32         FLine()     const noexcept { return _line; }
    constexpr u32         Column()   const noexcept { return _col;  }

    // デフォルト引数として渡すと呼び出し位置をキャプチャする
    static consteval FSourceLoc Current(
        const char* file = __builtin_FILE(),
        const char* func = __builtin_FUNCTION(),
        u32 line         = __builtin_LINE(),
#if ACS_COMPILER_MSVC || ACS_COMPILER_CLANG
        u32 col          = __builtin_COLUMN()
#else
        u32 col          = 0  // GCC は __builtin_COLUMN を持たない
#endif
    ) noexcept {
        FSourceLoc s;
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
