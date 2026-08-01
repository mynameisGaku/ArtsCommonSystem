// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar X — IOpenXrBridge (XR / AR / MR seam)
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
//   ・**FXrPose は euler 角で簡素化**: quaternion を表現する型を本 header で
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
//   ・**ml_err::kSub_NotImplemented と subcode 番号を揃える**: MlRuntime.h と
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

/**
 * 想定する XR backend プラットフォームの列挙。
 *
 * @details
 * Unknown は「Init 時に明示せず、backend に自動検出させる」用途。stub では常に
 * Unknown のまま留まり、具象 backend は実行時に選ばれた SDK 種別を反映する。
 */
enum class EXrPlatform : u8 {
    /** 未指定 / 検出失敗 (stub のデフォルト)。 */
    Unknown        = 0,

    /** Meta Quest 2 / 3 / Pro (Oculus / Meta XR SDK)。 */
    MetaQuest      = 1,

    /** Valve Index (OpenVR / SteamVR)。 */
    ValveIndex     = 2,

    /** HTC Vive 系 (OpenVR / Vive Wave)。 */
    HtcVive        = 3,

    /** ByteDance Pico Neo 3 / 4 (PICO XR SDK)。 */
    PicoNeo        = 4,

    /** Apple Vision Pro (VisionOS / ARKit)。 */
    AppleVisionPro = 5,

    /** Sony PlayStation VR2 (PS VR2 SDK)。 */
    PsVr2          = 6,

    /** Microsoft Windows Mixed Reality。 */
    WindowsMR      = 7,

    /** Steam Link (network streaming to mobile / standalone HMD)。 */
    SteamLink      = 8,
};

/**
 * HMD / コントローラの位置 + 向き。
 *
 * @details
 * orientation_euler は (pitch, yaw, roll) の順でラジアン格納する。math 依存最小化の
 * ため quaternion 表現は本 header では避ける。精密な向き計算をしたい上位は自分で
 * euler → quat / matrix へ変換する。
 */
struct FXrPose {
    /** world-space 位置 (m)。 */
    acs::FVec3 position           = acs::FVec3::Zero();

    /** 向きの euler 角 (pitch, yaw, roll) [rad]。 */
    acs::FVec3 orientation_euler  = acs::FVec3::Zero();

    /** ライブトラッキング由来かどうか (false なら position/orientation は原点で無効)。 */
    bool       tracked            = false;
};

/**
 * 左右コントローラの 1 フレーム分の入力 snapshot。
 *
 * @details
 * プラットフォーム差を吸収する最小公倍数 (6DoF pose、トリガ、grip、主要 2 ボタン、
 * スティック) のみを公開する。ハンドトラッキング / 触覚 / フェイストラッキング等の
 * より細かい input は別の構造体として後段で追加する。
 */
struct FXrControllerState {
    /** 6DoF コントローラポーズ。 */
    FXrPose    pose;

    /** 人差し指トリガのアナログ値 [0, 1]。 */
    f32       trigger    = 0.0f;

    /** グリップボタンのアナログ値 [0, 1]。 */
    f32       grip       = 0.0f;

    /** 主要ボタン A (or X)。 */
    bool      button_a   = false;

    /** 主要ボタン B (or Y)。 */
    bool      button_b   = false;

    /** 親指スティック / トラックパッド入力 [-1, 1] x 2。 */
    acs::FVec2 thumbstick = acs::FVec2::Zero();
};

/**
 * XR runtime / HMD seam の抽象 interface。
 *
 * @details
 * 実装の差し替えで具象 backend を選ぶ。stub は全 method を「初期化失敗 / 原点ポーズ /
 * passthrough 非対応」として返し、上位の fallback 経路を強制する。
 * ライフタイム契約: Init(platform) → (Tick / HeadPose / LeftController /
 * RightController / IsPassthroughSupported / SetPassthrough)* → Shutdown()。
 * スレッド契約: 1 instance はシングルスレッドでのみ使う (XR ランタイムは render /
 * game thread の同時 query を通常許さない)。
 */
class IOpenXrBridge {
public:
    /** 派生 backend を正しく破棄するための仮想デストラクタ。 */
    virtual ~IOpenXrBridge() noexcept = default;

    /**
     * backend を初期化する (XR ランタイム接続 / セッション開始 / リファレンス空間設定)。
     *
     * @param platform 接続先プラットフォーム (Unknown なら利用可能な SDK を自動検出)。
     * @return 成功なら空の TResult、失敗ならエラー (stub は常に NotImplemented)。
     */
    virtual TResult<void> Init(EXrPlatform platform = EXrPlatform::Unknown) noexcept = 0;

    /** backend を破棄する (セッション終了 / loader unload。Init 前に呼んでも安全)。 */
    virtual void Shutdown() noexcept = 0;

    /**
     * 初期化済みかを返す。
     *
     * @return Init() 成功後かつ Shutdown() 前なら true。
     */
    virtual bool IsInitialized() const noexcept = 0;

    /**
     * ヘッド / コントローラのポーズが現在ライブトラッキングされているかを返す。
     *
     * @details
     * false の場合 HeadPose()/LeftController()/RightController() は原点 (未トラッキングの
     * ゼロポーズ) を返すため、消費側はゼロポーズを有効な姿勢として使わず fallback すること。
     * IsInitialized() とは別概念で、instance/loader 生成成功とトラッキング有効性は異なる
     * (例: session loop 未確立の backend は IsInitialized()==true でも IsTracking()==false)。
     * 既定実装は false (stub / トラッキング非対応 backend 用の安全側)。
     * @return ライブトラッキング中なら true。
     */
    virtual bool IsTracking() const noexcept { return false; }

    /**
     * 現在 active な backend プラットフォームを返す。
     *
     * @return active なプラットフォーム (Init 前 / Shutdown 後 / stub は Unknown)。
     */
    virtual EXrPlatform ActivePlatform() const noexcept = 0;

    /**
     * 直近 Tick 時点の HMD ポーズを返す。
     *
     * @return HMD ポーズ (Init 前 / stub は原点 zero pose)。
     */
    virtual FXrPose HeadPose() const noexcept = 0;

    /**
     * 直近 Tick 時点の左コントローラ state を返す。
     *
     * @return 左コントローラ state (未接続ならゼロ state)。
     */
    virtual FXrControllerState LeftController()  const noexcept = 0;

    /**
     * 直近 Tick 時点の右コントローラ state を返す。
     *
     * @return 右コントローラ state (未接続ならゼロ state)。
     */
    virtual FXrControllerState RightController() const noexcept = 0;

    /**
     * ポーズ / 入力の取り込みを進める (XR ランタイムのイベントポンプ相当)。
     *
     * @details ゲームループから毎フレーム呼ぶこと。
     * @param dt 実時間の経過秒 (実装によっては使わない)。
     */
    virtual void Tick(f32 dt) noexcept = 0;

    /**
     * backend が passthrough (MR モード) をサポートするかを返す。
     *
     * @return passthrough 対応なら true (stub は常に false)。
     */
    virtual bool IsPassthroughSupported() const noexcept = 0;

    /**
     * passthrough の on/off を要求する。
     *
     * @details 非サポート / 未初期化なら no-op で構わない。
     * @param on true で passthrough を有効化、false で無効化。
     */
    virtual void SetPassthrough(bool on) noexcept = 0;

protected:
    /** 派生クラス専用の既定構築。 */
    IOpenXrBridge() noexcept = default;

    /** コピー禁止 (seam は singleton 運用前提)。 */
    IOpenXrBridge(const IOpenXrBridge&)            = delete;

    /** コピー代入も禁止。 */
    IOpenXrBridge& operator=(const IOpenXrBridge&) = delete;

    /** ムーブ禁止。 */
    IOpenXrBridge(IOpenXrBridge&&)                 = delete;

    /** ムーブ代入も禁止。 */
    IOpenXrBridge& operator=(IOpenXrBridge&&)      = delete;
};

/**
 * 「常に未初期化 / 原点 pose」を返す IOpenXrBridge の stub 実装。
 *
 * @details
 * SDK 同梱前でも上位層が「HMD は刺さっていない」想定で fallback (= 通常の 2D
 * ディスプレイ向け描画) を書けるようにする placeholder。Init() は NotImplemented
 * を返し (m_Initialized は false のまま)、pose / state は zero-initialized、
 * ActivePlatform() は Unknown、passthrough は非対応、Tick()/Shutdown() は副作用なし。
 */
class COpenXrBridgeStub final : public IOpenXrBridge {
public:
    /** stub を構築する (副作用なし)。 */
    COpenXrBridgeStub() noexcept = default;

    /** stub を破棄する (解放対象なし)。 */
    ~COpenXrBridgeStub() noexcept override = default;

    /**
     * 常に NotImplemented を返す (XR backend 未統合)。
     *
     * @param platform 受け取るが使わない (stub は検出も接続もしない)。
     * @return 常に Generic + NotImplemented エラー。
     */
    TResult<void>      Init(EXrPlatform platform = EXrPlatform::Unknown) noexcept override;

    /** m_Initialized を false に戻すだけの no-op。 */
    void              Shutdown()                                      noexcept override;

    /**
     * 初期化済みかを返す。
     *
     * @return Init が必ず失敗するため常に false。
     */
    bool              IsInitialized()                           const noexcept override { return m_Initialized; }

    /**
     * active なプラットフォームを返す。
     *
     * @return 常に Unknown。
     */
    EXrPlatform        ActivePlatform()                          const noexcept override { return EXrPlatform::Unknown; }

    /**
     * HMD ポーズを返す。
     *
     * @return zero-initialized な原点ポーズ。
     */
    FXrPose            HeadPose()                                const noexcept override { return FXrPose{}; }

    /**
     * 左コントローラ state を返す。
     *
     * @return zero-initialized な state。
     */
    FXrControllerState LeftController()                          const noexcept override { return FXrControllerState{}; }

    /**
     * 右コントローラ state を返す。
     *
     * @return zero-initialized な state。
     */
    FXrControllerState RightController()                         const noexcept override { return FXrControllerState{}; }

    /**
     * 入力取り込みを進める (stub は何もしない)。
     *
     * @param dt 受け取るが使わない。
     */
    void              Tick(f32 dt)                                    noexcept override;

    /**
     * passthrough 対応かを返す。
     *
     * @return 常に false。
     */
    bool              IsPassthroughSupported()                  const noexcept override { return false; }

    /**
     * passthrough の on/off を要求する (stub は no-op)。
     *
     * @param on 受け取るが使わない。
     */
    void              SetPassthrough(bool on)                         noexcept override;

private:
    /** 初期化済みフラグ (Init は NotImplemented で失敗するので常に false)。 */
    bool m_Initialized = false;
};

/**
 * process 内に 1 個だけ存在する静的 stub への参照を返す。
 *
 * @details
 * 静的単一インスタンス (process lifetime) を共有する。スレッド安全性は呼び出し側責務。
 * @return 共有 stub bridge への参照。
 */
IOpenXrBridge& GetXrStub() noexcept;

/**
 * 既定 IOpenXrBridge を返す provider 関数ポインタ型。
 *
 * @details
 * gameframework は実 backend モジュール (循環依存になる) に直接依存できないため、実
 * backend 側がこの型の関数を SetOpenXrBridgeProvider() で登録し、ゲームコードは
 * GetDefaultOpenXrBridge() 経由で backend 非依存に既定 bridge を取得する。
 */
using OpenXrBridgeProvider = IOpenXrBridge& (*)() noexcept;

/**
 * 既定 bridge provider を登録する (実 backend モジュールの Install* から呼ぶ)。
 *
 * @details 後勝ちで上書きする。
 * @param provider 既定 bridge を返す関数 (nullptr 登録で stub に戻す)。
 */
void SetOpenXrBridgeProvider(OpenXrBridgeProvider provider) noexcept;

/**
 * 既定 IOpenXrBridge を返す。
 *
 * @return provider 登録済みならその実 bridge、未登録なら GetXrStub()。
 */
IOpenXrBridge& GetDefaultOpenXrBridge() noexcept;

/**
 * XR seam のエラー subcode を集める namespace。
 *
 * @details MlRuntime.h / SaveSlot.h と subcode 番号を揃え、上位は category 横断で
 * 「未実装」を一律に扱える (Generic + subcode 99 規約)。
 */
namespace xr_err {
    /** 「未実装」subcode (stub / backend 未統合のときに返される)。 */
    inline constexpr u16 kSub_NotImplemented = 99;
} // namespace xr_err

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FOpenXrBridgeStub = COpenXrBridgeStub;

} // namespace acs::game
