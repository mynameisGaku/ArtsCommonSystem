// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar U — FMlRuntime / Upscaler stub 実装
//
// Phase U-1 では具象 ML backend (ONNX Runtime / DirectML / NNAPI / CoreML /
// TFLite) と Upscaler backend (FSR / DLSS / XeSS) はいずれも未統合のため、
// 本ファイルは「常に失敗 / Off を返す」最小実装だけを提供する。
//
// 上位層への効果:
//   ・`IMlRuntime` を叩いた瞬間に NotImplemented を返すので、ML 経路に
//     依存したコードは day-0 で **必ず fallback パスを書く** ことを強制される
//     (= 配布パッケージに ML SDK が含まれない / GPU が DirectML 非対応の
//      環境でも、ゲームは "AI off" で起動できる)。
//   ・`IUpscaler` は `Init(Off)` だけ成功するため、レンダラはまずネイティブ
//     解像度で動くと仮定して書く。FSR/DLSS/XeSS が後段で差し込まれた時に
//     `ActiveKind()` を見て低解像度描画 → 拡大の path に切り替える。
//
// 決定論ゾーン違反検出 (将来):
//   Phase U-2 で、`FGame::Tick()` の固定タイムステップ内で `RunInference` が
//   呼ばれたら debug build で assert する仕掛けを `FGame.cpp` 側に入れる予定。
//   本 stub では何も検出しないが、コメントとして契約を明示しておく。
#include "gameframework/MlRuntime.h"

namespace acs::game {

// =============================================================================
// MlRuntimeStub — 全 method NotImplemented
// -----------------------------------------------------------------------------
// `ACS_ERR(Generic, kSub_NotImplemented, ...)` を返す。ErrCategory に
// `NotImplemented` 専用カテゴリは無いため、FSaveSlot.h と同じ「Generic +
// subcode 99」の規約に揃える。message は静的文字列リテラル (ACS の方針通り)。
// =============================================================================
TResult<void> MlRuntimeStub::Init() noexcept {
    return ACS_ERR(Generic, ml_err::kSub_NotImplemented,
                   "MlRuntimeStub::Init: ML backend not integrated (Phase U-1 stub)");
}

void MlRuntimeStub::Shutdown() noexcept {
    // 何も保持していないので no-op。Init が常に失敗する以上、ここに来ても
    // 解放対象は存在しない。
}

TResult<MlModelHandle> MlRuntimeStub::LoadModel(const char* /*model_path*/) noexcept {
    return ACS_ERR(Generic, ml_err::kSub_NotImplemented,
                   "MlRuntimeStub::LoadModel: ML backend not integrated (Phase U-1 stub)");
}

TResult<void> MlRuntimeStub::UnloadModel(MlModelHandle /*h*/) noexcept {
    return ACS_ERR(Generic, ml_err::kSub_NotImplemented,
                   "MlRuntimeStub::UnloadModel: ML backend not integrated (Phase U-1 stub)");
}

TResult<void> MlRuntimeStub::RunInference(MlModelHandle /*h*/,
                                         const f32* /*inputs*/,  u32 /*in_count*/,
                                         f32*       /*outputs*/, u32 /*out_count*/) noexcept {
    return ACS_ERR(Generic, ml_err::kSub_NotImplemented,
                   "MlRuntimeStub::RunInference: ML backend not integrated (Phase U-1 stub)");
}

// =============================================================================
// FUpscalerStub — Off のみ受け付ける
// -----------------------------------------------------------------------------
// `EUpscalerKind::Off` への Init は「無効化を選んだ」状態として成功扱い。
// それ以外 (FSR / DLSS / XeSS / Custom) は SDK 未統合なので NotImplemented。
// =============================================================================
TResult<void> FUpscalerStub::Init(EUpscalerKind k) noexcept {
    if (k == EUpscalerKind::Off) {
        m_Kind = EUpscalerKind::Off;
        return Ok();
    }
    // SDK 同梱前提の kind が要求された → 上位は fallback パスを取るべき。
    return ACS_ERR(Generic, ml_err::kSub_NotImplemented,
                   "FUpscalerStub::Init: requested upscaler backend not integrated (Phase U-1 stub)");
}

// =============================================================================
// global stub アクセサ
// -----------------------------------------------------------------------------
// 関数内 static で遅延構築 (Meyers singleton)。process lifetime のオブジェクト
// として 1 個だけ存在し、`GetMlRuntimeStub()` / `GetUpscalerStub()` を呼んだ
// すべての箇所が同じ instance を共有する。
//
// スレッド安全性:
//   C++11 以降、関数内 static の初期化は thread-safe (magic statics)。
//   ただし stub 自身は状態を持つ (FUpscalerStub::m_Kind) ため、複数スレッドから
//   同時アクセスする呼び出し側は外部同期を取る必要がある。
// =============================================================================
IMlRuntime& GetMlRuntimeStub() noexcept {
    static MlRuntimeStub instance;
    return instance;
}

IUpscaler& GetUpscalerStub() noexcept {
    static FUpscalerStub instance;
    return instance;
}

} // namespace acs::game
