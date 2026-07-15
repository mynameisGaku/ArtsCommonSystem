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

acs::game::IMlRuntime& GetDefaultOnnxMlRuntime() noexcept
{
    // Meyers singleton。プロセス内 1 個の ONNX runtime を既定として共有する。
    static FOnnxMlRuntime Runtime;
    // local static の初期化ガードに Init も含め、並行した初回取得で二重初期化しない。
    static const bool bInitializationSucceeded = Runtime.Init().IsOk();
    (void)bInitializationSucceeded;
    return Runtime;
}

void InstallOnnxAsDefault() noexcept {
    acs::game::SetMlRuntimeProvider(&GetDefaultOnnxMlRuntime);
}

} // namespace acs::mlonnx
