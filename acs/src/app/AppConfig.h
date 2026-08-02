// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "foundation/Log.h"
#include "memory/MemorySystem.h"

namespace acs {

/**
 * CApplication::Run に渡す起動オプション一式。
 *
 * @details
 * ウィンドウ・ロガー・メモリシステム・スレッドプール・レンダラ・背景色の初期値を
 * まとめて保持する集約 struct。すべてのフィールドに既定値があり、`FAppConfig cfg{};`
 * のまま Run に渡せば標準設定で起動できる。
 */
struct FAppConfig {
    /** ウィンドウタイトル。 */
    const wchar_t* title       = L"ACS Application";

    /** クライアント領域の幅 (ピクセル)。 */
    u32            width       = 1280;

    /** クライアント領域の高さ (ピクセル)。 */
    u32            height      = 720;

    /** ウィンドウのリサイズを許可するか。 */
    bool           resizable   = true;

    /** 垂直同期 (VSync) を有効にするか。 */
    bool           vsync       = true;

    /** BeginFrame で背景色クリアを行うか。 */
    bool           start_clear = true;

    /** ロガーの最小出力重要度 (これ未満は破棄)。 */
    ELogSeverity    log_severity = ELogSeverity::Info;

    /** ログ出力先ファイルパス (nullptr でファイル出力なし)。 */
    const wchar_t* log_file     = nullptr;

    /** メモリシステム設定 (nullptr で既定設定を使う)。 */
    const FMemorySystemConfig* memory = nullptr;

    /** CThreadPool のワーカースレッド数 (0 でハードウェア並列数から自動決定)。 */
    u32            worker_count = 0;

    /** レンダラの GPU デバッグレイヤを有効にするか (Debug ビルド時に有効推奨)。 */
    bool           enable_gpu_debug = false;

    /**
     * ウィンドウ最小化中に待機する上限時間 (ミリ秒)。
     *
     * Win32 メッセージ到着時は即座に起床するため、復帰・入力遅延を増やさず
     * バックグラウンド CPU 使用率を下げる。0 は継続ポーリングを明示的に維持する。
     */
    u32            minimized_wait_ms = 100;

    /** 背景クリア色の赤成分 (0..1)。 */
    f32            clear_r = 0.1f;

    /** 背景クリア色の緑成分 (0..1)。 */
    f32            clear_g = 0.12f;

    /** 背景クリア色の青成分 (0..1)。 */
    f32            clear_b = 0.16f;

    /** 背景クリア色のアルファ成分 (0..1)。 */
    f32            clear_a = 1.0f;
};

} // acs 名前空間
