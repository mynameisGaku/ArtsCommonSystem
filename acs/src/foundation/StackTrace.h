// =============================================================================
// ACS Foundation — スタックトレース取得とシンボル化
// -----------------------------------------------------------------------------
// Win32 CaptureStackBackTrace でフレームアドレスを取得し、DbgHelp の Sym*
// 関数でシンボル/ファイル/行へ変換する。
//
// スレッド安全性:
//   - CaptureStackBackTrace 自体はスレッドセーフ
//   - DbgHelp の Sym* 系はスレッドセーフではない（MS 公式ドキュメント明記）
//     -> 内部 SRWLOCK で直列化する
// =============================================================================
#pragma once

#include "foundation/Types.h"

namespace acs {

// 取得する最大フレーム数（深さ）
inline constexpr u32 kStackTraceMaxFrames = 64;

// 1 フレームの情報
struct StackFrame {
    void*       address;       // 命令ポインタ (PC)
    u64         line;          // ソース行番号（取得失敗時は 0）
    char        symbol[256];   // 関数名（取得失敗時は "??? @ 0xADDR"）
    char        file[256];     // ソースファイルパス
};

// ---------------------------------------------------------------------------
// StackTrace — 呼び出しスタックのスナップショット
// ---------------------------------------------------------------------------
// 使い方:
//   StackTrace st;
//   st.Capture();
//   st.Resolve();
//   st.Print(MyWriter, user_ptr);
class StackTrace {
public:
    StackTrace() noexcept = default;

    // 現在のスタックを取得する。skip フレーム分は除外する
    // (例: 1 を渡すと Capture() 自身のフレームを除外)。
    void Capture(u32 skip = 1) noexcept;

    // 取得済みのアドレスをシンボル/ファイル/行に解決する。
    // スレッドセーフ（内部ロックで直列化）。
    void Resolve() noexcept;

    u32              FrameCount() const noexcept { return _count; }      // 取得フレーム数
    const StackFrame& Frame(u32 i) const noexcept { return _frames[i]; } // i 番目のフレーム

    // 1 行ずつテキストを sink に渡す。Panic 出力やロガーで使用する。
    using Sink = void (*)(void* user, const char* line, usize len);
    void Print(Sink sink, void* user) const noexcept;

private:
    u32        _count = 0;                         // 実際に取得したフレーム数
    bool       _resolved = false;                  // Resolve 済みか
    void*      _addrs[kStackTraceMaxFrames] = {};  // 生のフレームアドレス
    StackFrame _frames[kStackTraceMaxFrames] = {}; // 解決後の情報
};

} // namespace acs
