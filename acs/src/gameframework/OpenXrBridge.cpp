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
//     な struct を返す。`FXrPose` / `FXrControllerState` のデフォルトコンストラクタ
//     が原点 / ニュートラル値で初期化するため、stub 側で明示の値設定は不要。
//   ・`SetPassthrough(true)` を呼ばれても passthrough は非サポートなので no-op。
//
// 設計のポイント:
//   ・stub は副作用なし。`Init()` が失敗するので `m_Initialized` は常に false。
//   ・`Tick()` は callback pump を持たないので何もしない。
//   ・`GetXrStub()` は Meyer's singleton。スレッド初回構築は C++11 以降の規格で
//     保証されているため、追加同期は不要。
//   ・NotImplemented の subcode は `xr_err::kSub_NotImplemented = 99` を用い、
//     FMlRuntime.h / TSaveSlot.h と「Generic + subcode 99」規約を共有する。
//
// 決定論ゾーン:
//   XR ランタイムは入力デバイスのドライバ層に依存するため、ティックレベルの
//   再現性は保証できない。本 seam を経由した値 (HeadPose / Controller) は
//   決定論ゾーン (replay / netcode) へ持ち込んではいけない。stub では問題に
//   ならないが、Phase X-2 の具象 backend ではコメントとして契約を継承する。

#include "gameframework/OpenXrBridge.h"

#include "foundation/Error.h"
#include "threading/Atomic.h"

namespace acs::game {

/** 常に NotImplemented を返す (platform は使わず、m_Initialized は false のまま)。 */
TResult<void> FOpenXrBridgeStub::Init(EXrPlatform platform) noexcept {
    (void)platform;  // stub は platform 自動検出も特定指定も実装していない
    return ACS_ERR(Generic, xr_err::kSub_NotImplemented,
                   "OpenXrBridgeStub::Init: XR backend not integrated (Phase X-1 stub)");
}

/** m_Initialized を false に戻すだけの no-op (解放対象なし)。 */
void FOpenXrBridgeStub::Shutdown() noexcept {
    m_Initialized = false;
}

/** dt を受け取って捨てるだけの no-op (XR イベントポンプを持たない)。 */
void FOpenXrBridgeStub::Tick(f32 dt) noexcept {
    (void)dt;
}

/** on を無視する no-op (passthrough 非対応のため安全に捨てる)。 */
void FOpenXrBridgeStub::SetPassthrough(bool on) noexcept {
    (void)on;
}

/** 関数内 static で遅延構築する共有 stub への参照を返す (Meyers singleton)。 */
IOpenXrBridge& GetXrStub() noexcept {
    static FOpenXrBridgeStub instance;
    return instance;
}

namespace {
    /** 登録済みの既定 bridge provider (未登録なら nullptr = stub に縮退)。 */
    TAtomic<OpenXrBridgeProvider> g_OpenXrBridgeProvider{nullptr};
} // namespace

/** 既定 bridge provider を後勝ちで登録する。 */
void SetOpenXrBridgeProvider(OpenXrBridgeProvider Provider) noexcept
{
    g_OpenXrBridgeProvider.Store(Provider, EMemoryOrder::Release);
}

/** provider 登録済みならその実 bridge、未登録なら stub を返す。 */
IOpenXrBridge& GetDefaultOpenXrBridge() noexcept
{
    const OpenXrBridgeProvider Provider = g_OpenXrBridgeProvider.Load(EMemoryOrder::Acquire);
    return Provider ? Provider() : GetXrStub();
}

} // namespace acs::game
