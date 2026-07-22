// SPDX-License-Identifier: Apache-2.0
// HelloEasy — acs::easy レイヤを使った最も簡単な入門サンプル。
//
// クラス継承や TResult 型を一切使わず、関数を呼ぶだけで 2D ゲームが書ける。
#pragma once

namespace hello00 {

// 戻り値はプロセス終了コード (0 = 正常終了)。
struct FHelloEasyApp {
    static int Run() noexcept;
};

} // namespace hello00
