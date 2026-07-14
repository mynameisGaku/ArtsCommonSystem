// SPDX-License-Identifier: Apache-2.0
// スタックトレース取得とシンボル化（CaptureStackBackTrace + DbgHelp）
#pragma once

#include "foundation/Types.h"

namespace acs {

/** 1 回のキャプチャで取得する最大フレーム数。 */
inline constexpr u32 kStackTraceMaxFrames = 64;

/**
 * 解決済みの 1 スタックフレーム情報。
 */
struct StackFrame {
    /** 命令ポインタ (フレームのアドレス)。 */
    void*       address;

    /** ソース行番号 (取得失敗時は 0)。 */
    u64         line;

    /** 関数名 (取得失敗時は "??? @ 0xADDR")。 */
    char        symbol[256];

    /** ソースファイルパス (取得失敗時は空文字列)。 */
    char        file[256];
};

/**
 * スタックトレースの取得とシンボル化を行うクラス。
 *
 * @details
 * Capture で生のフレームアドレスを取り、Resolve で DbgHelp によりシンボル / ファイル /
 * 行に解決し、Print で 1 行ずつ整形して sink に渡す。
 */
class StackTrace {
public:
    /** 空状態で構築する (フレーム未取得)。 */
    StackTrace() noexcept = default;

    /**
     * 現在のコールスタックを取得する (まだシンボル解決はしない)。
     *
     * @param skip 先頭から除外するフレーム数 (既定 1、自分自身を飛ばす)。
     */
    void Capture(u32 skip = 1) noexcept;

    /**
     * 取得済みアドレスをシンボル / ファイル / 行番号に解決する。
     *
     * @details DbgHelp は非スレッドセーフのため内部 SRWLOCK で直列化する。多重呼び出しは無害。
     */
    void Resolve() noexcept;

    /**
     * DbgHelp の共有シンボルリゾルバを終了する。
     *
     * @details
     * Resolve が遅延初期化した `SymInitialize` と対になる終了 API。未初期化時と
     * 多重呼び出しは no-op で、終了後に Resolve を呼ぶと再初期化される。
     */
    static void ShutdownSymbolResolver() noexcept;

    /**
     * DbgHelp の共有シンボルリゾルバが初期化済みかを返す。
     *
     * @return SymInitialize が成功済みで ShutdownSymbolResolver 前なら true。
     */
    static bool IsSymbolResolverInitialized() noexcept;

    /**
     * 取得したフレーム数を返す。
     *
     * @return Capture で取得できたフレーム数。
     */
    u32              FrameCount() const noexcept { return m_Count; }

    /**
     * i 番目のフレーム情報を返す。
     *
     * @param i フレームインデックス (0 <= i < FrameCount())。
     * @return i 番目のフレーム情報。
     */
    const StackFrame& Frame(u32 i) const noexcept { return m_Frames[i]; }

    /** 整形済みの 1 行を受け取る出力シンク関数の型。 */
    using Sink = void (*)(void* user, const char* line, usize len);

    /**
     * 各フレームを 1 行ずつ整形して sink に渡す。
     *
     * @param sink 各行を受け取るコールバック。
     * @param user sink にそのまま渡されるコンテキストポインタ。
     */
    void Print(Sink sink, void* user) const noexcept;

private:
    /** 取得したフレーム数。 */
    u32        m_Count = 0;

    /** Resolve 済みかどうか。 */
    bool       m_Resolved = false;

    /** 生のフレームアドレス配列。 */
    void*      m_Addrs[kStackTraceMaxFrames] = {};

    /** 解決後のフレーム情報配列。 */
    StackFrame m_Frames[kStackTraceMaxFrames] = {};
};

} // namespace acs
