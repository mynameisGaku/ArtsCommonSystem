// SPDX-License-Identifier: Apache-2.0
// std::source_location 相当をコンパイラ組み込みで実装する
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"

namespace acs {

class FSourceLoc {
public:
    constexpr FSourceLoc() noexcept = default;

    constexpr const char* File()     const noexcept { return m_File; }
    constexpr const char* Function() const noexcept { return m_Func; }
    constexpr u32         Line()     const noexcept { return m_Line; }
    constexpr u32         Column()   const noexcept { return m_Col;  }

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
        s.m_File = file;
        s.m_Func = func;
        s.m_Line = line;
        s.m_Col  = col;
        return s;
    }

private:
    const char* m_File = "";
    const char* m_Func = "";
    u32         m_Line = 0;
    u32         m_Col  = 0;
};

} // namespace acs
