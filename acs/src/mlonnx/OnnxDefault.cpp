// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// OnnxDefault — FOnnxMlRuntime を gameframework の既定 MlRuntime provider へ結線する
// -----------------------------------------------------------------------------
// gameframework は ACS::MlOnnx に依存できない (循環依存) ため、結線は backend
// 側 (本 TU) から `acs::game::SetMlRuntimeProvider()` を呼んで行う。アプリは起動時
// に一度 `acs::mlonnx::InstallOnnxAsDefault()` を呼ぶだけで、以降は backend 非
// 依存に `acs::game::GetDefaultMlRuntime()` で実 ONNX Runtime backend を取得できる。
//
// provider が返す runtime はプロセス共有 singleton。初回アクセス時に Init() を 1 回
// 走らせ、すぐ使える状態で返す (FOnnxMlRuntime::Init は冪等なので二度呼んでも安全)。
// =============================================================================
#include "mlonnx/OnnxMlRuntime.h"
#include "gameframework/MlRuntime.h"

namespace acs::mlonnx {

acs::game::IMlRuntime& GetDefaultOnnxMlRuntime() noexcept {
    // Meyers singleton。プロセス内 1 個の ONNX runtime を既定として共有する。
    static FOnnxMlRuntime s_runtime;
    // 初回のみ Init。ACS のこの層は単一スレッド初期化を前提とするため、素朴な
    // フラグで十分 (function-local static の構築自体は thread-safe)。
    static bool s_inited = false;
    if (!s_inited) {
        (void)s_runtime.Init();  // 失敗しても runtime 参照は返す (呼び出し側が結果で判断)
        s_inited = true;
    }
    return s_runtime;
}

void InstallOnnxAsDefault() noexcept {
    acs::game::SetMlRuntimeProvider(&GetDefaultOnnxMlRuntime);
}

} // namespace acs::mlonnx
