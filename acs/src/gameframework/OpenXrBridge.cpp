// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar X — FOpenXrBridge stub 実装
//
// Phase X-1 では具象 XR backend (OpenXR Khronos loader / Oculus / OpenVR /
// PS VR2 / VisionOS) はいずれも未統合のため、本ファイルは「常に Init 失敗
// (NotImplemented) / pose 原点 / passthrough 非対応」を返す最小実装だけを提供する。
//
// 上位層への効果:
//   ・`IOpenXrBridge::Init()` を叩いた瞬間に NotImplemented を返すので、XR 経路に
//     依存したコードは day-0 で **必ず fallback パス** (通常の 2D ディスプレイ
//     向け描画) を書くことを強制される (= HMD を持たない開発機 / CI でも
//     ゲームは "VR off" で起動できる)。
//   ・`HeadPose()` / `LeftController()` / `RightController()` は zero-initialized
//     な struct を返す。`XrPose` / `XrControllerState` のデフォルトコンストラクタ
//     が原点 / ニュートラル値で初期化するため、stub 側で明示の値設定は不要。
//   ・`SetPassthrough(true)` を呼ばれても passthrough は非サポートなので no-op。
//
// 設計のポイント:
//   ・stub は副作用なし。`Init()` が失敗するので `_initialized` は常に false。
//   ・`Tick()` は callback pump を持たないので何もしない。
//   ・`GetXrStub()` は Meyer's singleton。スレッド初回構築は C++11 以降の規格で
//     保証されているため、追加同期は不要。
//   ・NotImplemented の subcode は `xr_err::kSub_NotImplemented = 99` を用い、
//     FMlRuntime.h / FSaveSlot.h と「Generic + subcode 99」規約を共有する。
//
// 決定論ゾーン:
//   XR ランタイムは入力デバイスのドライバ層に依存するため、ティックレベルの
//   再現性は保証できない。本 seam を経由した値 (HeadPose / Controller) は
//   決定論ゾーン (replay / netcode) へ持ち込んではいけない。stub では問題に
//   ならないが、Phase X-2 の具象 backend ではコメントとして契約を継承する。

#include "gameframework/OpenXrBridge.h"

#include "foundation/Error.h"

namespace acs::game {

// =============================================================================
// OpenXrBridgeStub::Init — NotImplemented
// -----------------------------------------------------------------------------
// `platform` は受け取るが stub では一切使わない (Phase X-2 で具象 backend が
// 選ばれるまでは検出も接続もしないため)。`(void)platform` で未使用警告を抑止。
// `_initialized` は false のまま据え置く (Init 失敗時に initialized=true にすると
// 上位の IsInitialized 判定が壊れるため)。
// =============================================================================
TResult<void> OpenXrBridgeStub::Init(EXrPlatform platform) noexcept {
    (void)platform;  // stub は platform 自動検出も特定指定も実装していない
    return ACS_ERR(Generic, xr_err::kSub_NotImplemented,
                   "OpenXrBridgeStub::Init: XR backend not integrated (Phase X-1 stub)");
}

// =============================================================================
// OpenXrBridgeStub::Shutdown — no-op
// -----------------------------------------------------------------------------
// Init が常に失敗する以上、解放対象は存在しない。`_initialized` を false に
// 戻すだけにする (将来 Init 成功する具象 backend に置き換わったときの
// 防御として副作用最小で残す)。
// =============================================================================
void OpenXrBridgeStub::Shutdown() noexcept {
    _initialized = false;
}

// =============================================================================
// OpenXrBridgeStub::Tick — no-op
// -----------------------------------------------------------------------------
// stub は XR ランタイムイベントを持たないので、`dt` を受け取って捨てるだけ。
// 上位コードが `Tick(dt)` を毎フレーム呼ぶ慣習を維持できるよう、引数シグネチャ
// だけ提供する。
// =============================================================================
void OpenXrBridgeStub::Tick(f32 dt) noexcept {
    (void)dt;
}

// =============================================================================
// OpenXrBridgeStub::SetPassthrough — no-op
// -----------------------------------------------------------------------------
// stub は `IsPassthroughSupported()` で false を返すので、`SetPassthrough(true)`
// が呼ばれても安全に無視する。エラーは返さず、上位の MR モード切替コードが
// "passthrough が黙って効かなかった" 状況で fallback 表示 (= passthrough なしの
// 通常 VR / 通常 2D) を選べるようにする。
// =============================================================================
void OpenXrBridgeStub::SetPassthrough(bool on) noexcept {
    (void)on;
}

// =============================================================================
// GetXrStub — global stub アクセサ
// -----------------------------------------------------------------------------
// 関数内 static で遅延構築 (Meyers singleton)。process lifetime のオブジェクト
// として 1 個だけ存在し、すべての呼び出し箇所が同じ instance を共有する。
//
// スレッド安全性:
//   C++11 以降、関数内 static の初期化は thread-safe (magic statics)。
//   ただし stub 自身は状態 (`_initialized`) を持つため、複数スレッドから
//   同時に Init/Shutdown を呼ぶ場合は呼び出し側で同期を取る必要がある。
//   (stub は Init が必ず失敗するので競合は発生しないが、Phase X-2 の具象
//    backend に差し替わったときの契約として明示しておく。)
// =============================================================================
IOpenXrBridge& GetXrStub() noexcept {
    static OpenXrBridgeStub instance;
    return instance;
}

} // namespace acs::game
