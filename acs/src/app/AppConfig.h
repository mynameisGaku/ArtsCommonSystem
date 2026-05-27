// SPDX-License-Identifier: Apache-2.0
// アプリケーション初期化オプション
#pragma once

#include "foundation/Types.h"
#include "foundation/Log.h"
#include "memory/MemorySystem.h"

namespace acs {

struct FAppConfig {
    // ウィンドウ
    const wchar_t* title       = L"ACS FApplication";
    u32            width       = 1280;
    u32            height      = 720;
    bool           resizable   = true;
    bool           vsync       = true;
    bool           start_clear = true;     // BeginFrame で背景色クリアするか

    // ロガー
    ELogSeverity    log_severity = ELogSeverity::Info;
    const wchar_t* log_file     = nullptr;

    // メモリシステム（nullptr で既定設定を使う）
    const MemorySystemConfig* memory = nullptr;

    // FThreadPool ワーカー数（0 で自動）
    u32            worker_count = 0;

    // レンダラ
    bool           enable_gpu_debug = false;  // Debug ビルド時に有効推奨

    // 背景色（BeginFrame で使用）
    f32            clear_r = 0.1f;
    f32            clear_g = 0.12f;
    f32            clear_b = 0.16f;
    f32            clear_a = 1.0f;
};

} // namespace acs
