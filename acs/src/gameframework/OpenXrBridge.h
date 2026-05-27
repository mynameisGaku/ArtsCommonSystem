// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar X — FOpenXrBridge (XR / AR / MR seam)
//
// 役割:
//   Pillar X (XR / AR / MR + 新興プラットフォーム) のうち、OpenXR ランタイム及び
//   各 HMD ベンダ固有 SDK (Meta Quest / Valve Index / HTC Vive / Pico Neo /
//   Apple Vision Pro / PS VR2 / Windows MR / Steam Link) への接続を
//   **interface seam** として隔離する。
//
//   ゲームロジック側は `IOpenXrBridge&` 越しに HMD / コントローラポーズや
//   passthrough 制御を叩く前提で、具象 backend (OpenXR Khronos loader / Oculus SDK
//   / OpenVR / Sony PS VR2 SDK / VisionOS ARKit 等) は後段の独立モジュール
//   ("acs_openxr" / "acs_oculus" / "acs_psvr2" 等) で SDK 同梱と差し替えを行う。
//   本 header はその「窓口」だけを定義するスケルトンで、Phase X-1 では
//   stub 実装のみを提供する。
//
// なぜ seam で隔離するか:
//   1. **SDK 依存とライセンスの隔離**:
//      OpenXR loader / Oculus SDK / OpenVR / Sony PS VR2 SDK / VisionOS ARKit は
//      それぞれ別の SDK / NDA / 配布制限を持ち、GameFramework 本体に直接
//      リンクすると配布形態 (OSS 公開 / クローズ SDK 同梱) が縛られる。
//      interface だけを本層に置き、具象は別モジュール (オプションリンク) に
//      追い出す。
//   2. **HMD 接続前提のないビルドが普通**:
//      開発機や CI マシンに HMD が刺さっていないのは日常的なため、stub が
//      day-0 から「常に Init 失敗 / pose は原点」を返すことで、上位は
//      フォールバックパス (= 通常の 2D ディスプレイ向け描画) を最初から
//      書かざるを得なくなる (= 設計強制)。
//   3. **プラットフォーム差の吸収**:
//      OpenXR / Oculus / SteamVR / PS VR2 / VisionOS は API 形状もポーズ系も
//      コントローラスキーマも全く違うが、上位ゲームコードからは「ヘッド
//      ポーズ + 左右コントローラ state + passthrough on/off」だけ見えれば
//      足りる。本 interface はその最小公倍数だけを公開する。
//   4. **テスト容易性**:
//      `IOpenXrBridge` の純粋仮想ポインタを差し替えるだけで mock backend に
//      置換でき、HMD を持たない CI で上位層のロジック試験を回せる。
//
// 設計選択 (Phase X-1):
//   ・**XrPose は euler 角で簡素化**: quaternion を表現する型を本 header で
//     新規導入すると math モジュールへの依存が増える。XR の生 API は quaternion
//     を返すが、ゲーム層がポーズを扱う最小用途 (カメラ向き / コントローラ
//     向き) は euler で十分。具象 backend が quat → euler を変換する。
//     精度問題 (gimbal lock) が出る用途のために、Phase X-2 で `XrPoseQuat` を
//     別途追加する余地を残す。
//   ・**`<string>` 不使用 / STL 不使用**: ACS 全体方針に沿う。プラットフォーム
//     名や device 名はすべて enum で表現し、文字列は ACS_ERR の static literal
//     のみ。
//   ・**`acs::TResult<T, FErrorCode>` で例外なし**: stub は Init() で NotImplemented
//     を返し、上位はその時点で fallback パスへ。
//   ・**非コピー / 非ムーブ**: singleton 運用前提の seam。具象 backend も
//     instance を 1 つだけ持つことを想定。
//   ・**ml_err::kSub_NotImplemented と subcode 番号を揃える**: FMlRuntime.h と
//     同じく `Generic + subcode 99` を NotImplemented の規約として共有し、
//     上位は category 横断で「未実装エラー」を一律で扱える。
//
// 範囲外 (Phase X-2+ で):
//   ・XrPoseQuat (quaternion 表現) / 速度 / 加速度トラッキング
//   ・hand tracking / eye tracking / face tracking
//   ・haptic feedback / room boundary / anchor / spatial mapping
//   ・複数同時接続 (HMD + 外部カメラ) / mixed reality capture
//   ・実 SDK 連携 (`OpenXrBridgeKhronos` / `OpenXrBridgeOculus` 等)
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game {

// =============================================================================
// EXrPlatform — 想定する XR backend プラットフォーム列挙
// -----------------------------------------------------------------------------
// `Unknown` は「Init 時に明示せず、backend に自動検出させる」用途。stub では
// 常に Unknown のまま留まる。具象 backend は実行時に選ばれた SDK 種別を
// 反映する。
// =============================================================================
enum class EXrPlatform : u8 {
    Unknown        = 0,   // 未指定 / 検出失敗 (stub のデフォルト)
    MetaQuest      = 1,   // Meta Quest 2 / 3 / Pro (Oculus / Meta XR SDK)
    ValveIndex     = 2,   // Valve Index (OpenVR / SteamVR)
    HtcVive        = 3,   // HTC Vive 系 (OpenVR / Vive Wave)
    PicoNeo        = 4,   // ByteDance Pico Neo 3 / 4 (PICO XR SDK)
    AppleVisionPro = 5,   // Apple Vision Pro (VisionOS / ARKit)
    PsVr2          = 6,   // Sony PlayStation VR2 (PS VR2 SDK)
    WindowsMR      = 7,   // Microsoft Windows Mixed Reality
    SteamLink      = 8,   // Steam Link (network streaming to mobile / standalone HMD)
};

// =============================================================================
// XrPose — HMD / コントローラの位置 + 向き
// -----------------------------------------------------------------------------
// `orientation_euler` は (pitch, yaw, roll) の順で **ラジアン** で格納する。
// quaternion 表現を本 header では避ける (math 依存の最小化、簡素化)。
// 上位が精密な向き計算をしたい場合は、自分で euler → quat / matrix へ変換する。
// =============================================================================
struct XrPose {
    acs::FVec3 position           = acs::FVec3::Zero();  // world-space 位置 (m)
    acs::FVec3 orientation_euler  = acs::FVec3::Zero();  // (pitch, yaw, roll) [rad]
};

// =============================================================================
// XrControllerState — 左右コントローラの 1 フレーム分の入力 snapshot
// -----------------------------------------------------------------------------
// プラットフォーム差を吸収する最小公倍数:
//   ・pose       : 6DoF 位置 + 向き
//   ・trigger    : 人差し指トリガ [0, 1] アナログ
//   ・grip       : 中指 grip ボタン [0, 1] アナログ
//   ・button_a/b : 親指側の主要 2 ボタン (Quest=A/B または X/Y、Index=A/B、
//                  Vive=メニュー/grip 兼用、PS VR2=○/△ にマップ)
//   ・thumbstick : 親指スティック / トラックパッド入力 ([-1, 1] x 2)
// より細かい input (ハンドトラッキング / 触覚 / フェイストラッキング) は
// Phase X-2 で別の構造体として追加する。
// =============================================================================
struct XrControllerState {
    XrPose    pose;                           // 6DoF コントローラポーズ
    f32       trigger    = 0.0f;              // [0, 1] 人差し指トリガ
    f32       grip       = 0.0f;              // [0, 1] グリップボタン
    bool      button_a   = false;             // 主要ボタン A (or X)
    bool      button_b   = false;             // 主要ボタン B (or Y)
    acs::FVec2 thumbstick = acs::FVec2::Zero(); // [-1, 1] アナログスティック
};

// =============================================================================
// IOpenXrBridge — XR runtime / HMD seam の抽象 interface
// -----------------------------------------------------------------------------
// 実装の差し替えで具象 backend を選ぶ。stub は全 method を「初期化失敗 /
// 原点ポーズ / passthrough 非対応」として返し、上位の fallback 経路を強制する。
//
// ライフタイム契約:
//   Init(platform) → (Tick / HeadPose / LeftController / RightController /
//   IsPassthroughSupported / SetPassthrough)* → Shutdown()
//   ・Init() 未呼出での pose 取得は実装定義 (stub は原点 pose を返す)。
//   ・Shutdown() 後の再 Init() は実装定義 (stub は常に NotImplemented)。
//
// スレッド契約:
//   ・1 つの IOpenXrBridge instance は **シングルスレッドでのみ** 使う。
//     XR ランタイムは通常 render thread / game thread の二系統からの
//     同時 query を許さない。並列化したい場合は backend 側で予約された
//     query API (OpenXR の `xrLocateViews` 等) を直接叩くこと。
// =============================================================================
class IOpenXrBridge {
public:
    virtual ~IOpenXrBridge() noexcept = default;

    // backend を初期化 (XR ランタイム接続 / セッション開始 / リファレンス空間設定)。
    // `platform == Unknown` のときは backend が利用可能な SDK を自動検出する。
    virtual TResult<void> Init(EXrPlatform platform = EXrPlatform::Unknown) noexcept = 0;

    // backend を破棄 (セッション終了 / loader unload)。Init 前に呼んでも安全。
    virtual void Shutdown() noexcept = 0;

    // Init() 成功後かつ Shutdown() 前なら true。
    virtual bool IsInitialized() const noexcept = 0;

    // 現在 active な backend プラットフォーム。Init 前 / Shutdown 後 / stub は Unknown。
    virtual EXrPlatform ActivePlatform() const noexcept = 0;

    // 直近 Tick 時点の HMD ポーズ。Init 前 / stub は原点 (zero pose) を返す。
    virtual XrPose HeadPose() const noexcept = 0;

    // 直近 Tick 時点の左 / 右コントローラ state。未接続側はゼロ state を返す。
    virtual XrControllerState LeftController()  const noexcept = 0;
    virtual XrControllerState RightController() const noexcept = 0;

    // ポーズ / 入力の取り込みを進める。XR ランタイムのイベントポンプ相当。
    // ゲームループから毎フレーム呼ぶこと。dt は実時間秒 (実装によっては使わない)。
    virtual void Tick(f32 dt) noexcept = 0;

    // backend が passthrough (MR モード) をサポートするか。stub は常に false。
    virtual bool IsPassthroughSupported() const noexcept = 0;

    // passthrough の on/off を要求。非サポート / 未初期化なら no-op で構わない。
    virtual void SetPassthrough(bool on) noexcept = 0;

protected:
    IOpenXrBridge() noexcept = default;
    IOpenXrBridge(const IOpenXrBridge&)            = delete;
    IOpenXrBridge& operator=(const IOpenXrBridge&) = delete;
    IOpenXrBridge(IOpenXrBridge&&)                 = delete;
    IOpenXrBridge& operator=(IOpenXrBridge&&)      = delete;
};

// =============================================================================
// OpenXrBridgeStub — 「常に未初期化 / 原点 pose」を返す stub
// -----------------------------------------------------------------------------
// SDK 同梱前の状態でも上位層が「HMD は刺さっていない」想定で fallback
// (= 通常の 2D ディスプレイ向け描画) を書けるようにするための placeholder。
//
// 仕様:
//   ・`Init()` は NotImplemented を返す (副作用なし、m_Initialized は false のまま)。
//   ・`HeadPose()` / `LeftController()` / `RightController()` は zero-initialized
//     な pose / state を返す。
//   ・`ActivePlatform()` は常に `Unknown`。
//   ・`IsPassthroughSupported()` は常に false。`SetPassthrough()` は no-op。
//   ・`Tick()` / `Shutdown()` は副作用なし。
// =============================================================================
class OpenXrBridgeStub final : public IOpenXrBridge {
public:
    OpenXrBridgeStub() noexcept = default;
    ~OpenXrBridgeStub() noexcept override = default;

    TResult<void>      Init(EXrPlatform platform = EXrPlatform::Unknown) noexcept override;
    void              Shutdown()                                      noexcept override;
    bool              IsInitialized()                           const noexcept override { return m_Initialized; }
    EXrPlatform        ActivePlatform()                          const noexcept override { return EXrPlatform::Unknown; }
    XrPose            HeadPose()                                const noexcept override { return XrPose{}; }
    XrControllerState LeftController()                          const noexcept override { return XrControllerState{}; }
    XrControllerState RightController()                         const noexcept override { return XrControllerState{}; }
    void              Tick(f32 dt)                                    noexcept override;
    bool              IsPassthroughSupported()                  const noexcept override { return false; }
    void              SetPassthrough(bool on)                         noexcept override;

private:
    bool m_Initialized = false;  // Init は NotImplemented で失敗するので常に false
};

// =============================================================================
// global stub アクセサ
// -----------------------------------------------------------------------------
// process 内で 1 個だけ存在する静的 stub への参照を返す。Phase X-1 では
// `FGame` / `Scene` 側からの XR 問い合わせはすべてこの 1 つを通る。
// Phase X-2 以降、具象 backend が選ばれるとアクセサは差し替えられる。
//
// ※ static 単一インスタンス = process lifetime。スレッド安全性は呼び出し側責務。
// =============================================================================
IOpenXrBridge& GetXrStub() noexcept;

// FMlRuntime.h と subcode 番号を揃え、上位は category 横断で「未実装」を
// 一律で扱えるようにする (FSaveSlot.h と同じく `Generic + subcode 99` 規約)。
namespace xr_err {
    // 「未実装」: stub だから / Phase X-2 まで backend 未統合だから返される。
    inline constexpr u16 kSub_NotImplemented = 99;
} // namespace xr_err

} // namespace acs::game
