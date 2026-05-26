// SPDX-License-Identifier: Apache-2.0
// HelloEasy — acs::easy レイヤを使った最も簡単な入門サンプル。
//
// クラス継承や Result 型を一切使わず、関数を呼ぶだけで 2D ゲームが書ける。
// 本ファイルでは「main 関数の本体」だけを HelloEasyApp::Run() に切り出し、
// main.cpp は呼び出し 1 行だけにしている (sample 38 と同じ分割パターン)。
#pragma once

namespace hello00 {

// アプリケーションを起動して、ウィンドウが閉じられるまでメインループを回す。
// 戻り値はプロセス終了コード (0 = 正常終了)。
struct HelloEasyApp {
    static int Run() noexcept;
};

} // namespace hello00
