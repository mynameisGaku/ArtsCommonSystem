// SPDX-License-Identifier: Apache-2.0
// スタックトレース取得とシンボル化（CaptureStackBackTrace + DbgHelp）
#pragma once

#include "foundation/Types.h"

namespace acs {

// 取得する最大フレーム数
inline constexpr u32 kStackTraceMaxFrames = 64;

// 1 フレームの情報
struct StackFrame {
    void*       address;       // 命令ポインタ
    u64         line;          // ソース行番号（取得失敗時は 0）
    char        symbol[256];   // 関数名（取得失敗時は "??? @ 0xADDR"）
    char        file[256];     // ソースファイルパス
};

class StackTrace {
public:
    StackTrace() noexcept = default;

    // 現在のスタックを取得（skip フレーム分は除外）
    void Capture(u32 skip = 1) noexcept;

    // 取得済みアドレスをシンボル/ファイル/行に解決（内部ロックでスレッドセーフ）
    void Resolve() noexcept;

    u32              FrameCount() const noexcept { return _count; }
    const StackFrame& Frame(u32 i) const noexcept { return _frames[i]; }

    // 1 行ずつテキストを sink に渡す
    using Sink = void (*)(void* user, const char* line, usize len);
    void Print(Sink sink, void* user) const noexcept;

private:
    u32        _count = 0;                         // 取得したフレーム数
    bool       _resolved = false;                  // Resolve 済みか
    void*      _addrs[kStackTraceMaxFrames] = {};  // 生のフレームアドレス
    StackFrame _frames[kStackTraceMaxFrames] = {}; // 解決後の情報
};

} // namespace acs
