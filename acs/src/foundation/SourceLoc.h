// SPDX-License-Identifier: Apache-2.0
// std::source_location 相当をコンパイラ組み込みで実装する
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"

namespace acs {

/**
 * ソースコード上の発生位置 (ファイル・関数・行・列) を保持する値型。
 *
 * @details
 * std::source_location 相当をコンパイラ組み込み (__builtin_FILE 等) で実装する。
 * Current() をデフォルト引数として渡すと、呼び出し位置を自動でキャプチャできる。
 */
class FSourceLoc {
public:
    /** 空の位置情報 (ファイル・関数は空文字列、行・列は 0) を構築する。 */
    constexpr FSourceLoc() noexcept = default;

    /**
     * ソースファイルパスを返す。
     *
     * @return ファイルパス文字列 (未設定なら空文字列)。
     */
    constexpr const char* File()     const noexcept { return m_File; }

    /**
     * 関数名を返す。
     *
     * @return 関数名文字列 (未設定なら空文字列)。
     */
    constexpr const char* Function() const noexcept { return m_Func; }

    /**
     * 行番号を返す。
     *
     * @return 行番号 (未設定なら 0)。
     */
    constexpr u32         Line()     const noexcept { return m_Line; }

    /**
     * 列番号を返す。
     *
     * @return 列番号 (未設定または GCC では 0)。
     */
    constexpr u32         Column()   const noexcept { return m_Col;  }

    /**
     * 呼び出し位置をキャプチャした FSourceLoc を生成する。
     *
     * @details
     * 各引数のデフォルト値がコンパイラ組み込みで埋まるため、引数を渡さずに呼ぶと
     * 呼び出し箇所のファイル・関数・行・列が入る。col は GCC では __builtin_COLUMN が
     * ないため常に 0。
     * @param file ソースファイルパス (既定: 呼び出し箇所のファイル)。
     * @param func 関数名 (既定: 呼び出し箇所の関数)。
     * @param line 行番号 (既定: 呼び出し箇所の行)。
     * @param col 列番号 (既定: 呼び出し箇所の列、GCC では 0)。
     * @return 呼び出し位置を反映した FSourceLoc。
     */
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

    /**
     * 外部ライブラリなどが渡すファイル・関数・行・列から位置情報を明示生成する。
     *
     * @details 文字列は FSourceLoc より長く生存する必要がある。デバッグ情報を持たない引数は空文字列または 0 でよい。
     * @param file ソースファイルパス。
     * @param function 関数名または外部ライブラリの割り当て説明。
     * @param line 行番号。
     * @param column 列番号。
     * @return 指定値を保持する FSourceLoc。
     */
    static constexpr FSourceLoc Create(const char* file, const char* function, u32 line, u32 column = 0) noexcept
    {
        FSourceLoc location;
        location.m_File = file ? file : "";
        location.m_Func = function ? function : "";
        location.m_Line = line;
        location.m_Col = column;
        return location;
    }

private:
    /** ソースファイルパス (既定は空文字列)。 */
    const char* m_File = "";

    /** 関数名 (既定は空文字列)。 */
    const char* m_Func = "";

    /** 行番号 (既定は 0)。 */
    u32         m_Line = 0;

    /** 列番号 (既定は 0)。 */
    u32         m_Col  = 0;
};

} // namespace acs
