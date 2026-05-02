// =============================================================================
// ACS Foundation — ソースコード位置情報（<source_location> 代替）
// -----------------------------------------------------------------------------
// std::source_location と同等の API を、コンパイラ組み込み (__builtin_FILE
// など) を直接使って実装する。デフォルト引数 SourceLoc::Current() を用いる
// ことで、呼び出し元のファイル / 行 / 関数を自動キャプチャ可能。
//
// 用途: Assert / Panic / Logger / ErrorCode などのエラー報告で、
//       「どこで」発生したかを記録する。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"

namespace acs {

class SourceLoc {
public:
    // デフォルト構築は無効値（File/Function は空文字列、Line/Column は 0）。
    constexpr SourceLoc() noexcept = default;

    // ---- 取得 ----
    constexpr const char* File()     const noexcept { return _file; }  // ソースファイルパス
    constexpr const char* Function() const noexcept { return _func; }  // 関数名
    constexpr u32         Line()     const noexcept { return _line; }  // 行番号
    constexpr u32         Column()   const noexcept { return _col;  }  // 列番号

    // ---- 呼び出し元位置の取得 (consteval) ----
    // デフォルト引数として渡すと、呼び出し時点のソース位置がキャプチャされる。
    // 例:  void Foo(SourceLoc loc = SourceLoc::Current()) { ... }
    static consteval SourceLoc Current(
        const char* file = __builtin_FILE(),
        const char* func = __builtin_FUNCTION(),
        u32 line         = __builtin_LINE(),
#if ACS_COMPILER_MSVC || ACS_COMPILER_CLANG
        u32 col          = __builtin_COLUMN()  // GCC は __builtin_COLUMN を持たない
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
    const char* _file = "";  // 文字列リテラルへのポインタ（コピーしない）
    const char* _func = "";
    u32         _line = 0;
    u32         _col  = 0;
};

} // namespace acs
