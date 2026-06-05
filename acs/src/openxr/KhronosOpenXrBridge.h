// SPDX-License-Identifier: Apache-2.0
// Real Khronos OpenXR loader backend for acs::game::IOpenXrBridge.
#pragma once

#include "gameframework/OpenXrBridge.h"

namespace acs::openxr {

/** OpenXR ランタイム未インストール時のエラー subcode。 */
inline constexpr acs::u16 kSubOpenXrRuntimeUnavailable = 301;

/** OpenXR instance 生成失敗時のエラー subcode。 */
inline constexpr acs::u16 kSubOpenXrInitFailed         = 302;

/**
 * Khronos OpenXR loader をバックエンドとする IOpenXrBridge の実装。
 *
 * @details
 * Init() で xrEnumerateInstanceExtensionProperties / xrCreateInstance を呼び、
 * loader/instance seam だけを所有する。session loop (xrCreateSession +
 * frame/view/action loop) は graphics binding と HMD ランタイムを要求するため本
 * backend では未実装で、HeadPose()/LeftController()/RightController() は常に原点
 * (未トラッキングのゼロポーズ) を返し IsTracking() は常に false を返す。passthrough
 * 拡張 (XR_FB_passthrough / XR_HTC_passthrough) の有無のみ検出する。
 */
class FKhronosOpenXrBridge final : public acs::game::IOpenXrBridge {
public:
    /** 空状態で構築する (instance は Init で生成)。 */
    FKhronosOpenXrBridge() noexcept = default;

    /** 破棄する (Shutdown を呼んで instance を解放)。 */
    ~FKhronosOpenXrBridge() noexcept override;

    /**
     * OpenXR instance を生成して backend を初期化する。
     *
     * @details
     * 既に初期化済みなら何もせず成功を返す。ランタイム未インストールなら
     * kSubOpenXrRuntimeUnavailable、instance 生成失敗なら kSubOpenXrInitFailed の
     * エラーを返す。session は生成しないため、成功しても実トラッキングは始まらない。
     * @param platform 接続先プラットフォーム (記録するのみで自動検出には使わない)。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    acs::TResult<void> Init(acs::game::EXrPlatform platform = acs::game::EXrPlatform::Unknown) noexcept override;

    /** instance を破棄し全状態をゼロへ戻す (Init 前に呼んでも安全)。 */
    void Shutdown() noexcept override;

    /**
     * 初期化済みかを返す。
     *
     * @return Init() 成功後かつ Shutdown() 前なら true。
     */
    bool IsInitialized() const noexcept override { return m_bInitialized; }

    /**
     * 記録された backend プラットフォームを返す。
     *
     * @return Init() で渡されたプラットフォーム (未初期化なら Unknown)。
     */
    acs::game::EXrPlatform ActivePlatform() const noexcept override { return m_Platform; }

    /**
     * 直近 Tick 時点の HMD ポーズを返す。
     *
     * @return HMD ポーズ (session 未実装のため常に原点ゼロポーズ)。
     */
    acs::game::XrPose HeadPose() const noexcept override { return m_HeadPose; }

    /**
     * 直近 Tick 時点の左コントローラ state を返す。
     *
     * @return 左コントローラ state (session 未実装のため常にゼロ state)。
     */
    acs::game::XrControllerState LeftController() const noexcept override { return m_Left; }

    /**
     * 直近 Tick 時点の右コントローラ state を返す。
     *
     * @return 右コントローラ state (session 未実装のため常にゼロ state)。
     */
    acs::game::XrControllerState RightController() const noexcept override { return m_Right; }

    /**
     * ポーズ / 入力の取り込みを進める。
     *
     * @details
     * session loop が未実装のため実トラッキングは行わず、初期化済みなら一度だけ未実装の
     * 警告ログを出し、HeadPose / 左右コントローラを毎フレーム原点ゼロポーズへ保つ。
     * @param dt 実時間の経過秒 (本 backend では使わない)。
     */
    void Tick(acs::f32 dt) noexcept override;

    /**
     * passthrough (MR モード) をサポートするかを返す。
     *
     * @return Init 時に XR_FB_passthrough / XR_HTC_passthrough を検出していれば true。
     */
    bool IsPassthroughSupported() const noexcept override { return m_bPassthroughSupported; }

    /**
     * passthrough の on/off を要求する。
     *
     * @details サポートされている場合のみ要求フラグを更新する (非対応なら常に無効のまま)。
     * @param b_on true で passthrough を有効化、false で無効化。
     */
    void SetPassthrough(bool b_on) noexcept override;

    /**
     * 実トラッキングが生きているかを返す。
     *
     * @details
     * 本 backend は loader/instance seam のみを所有し session loop は未実装なので、
     * 現状は常に false を返す。HeadPose()/LeftController()/RightController() が返す
     * ゼロポーズを「有効なトラッキング値」と誤認しないために、消費側 (IOpenXrBridge&
     * 越しも含む) はこのフラグを見て fallback を選ぶこと。
     * @return 実トラッキング中なら true (本 backend では常に false)。
     */
    bool IsTracking() const noexcept override { return m_bSessionTracking; }

private:
    /** XrInstance (公開 header の TU では opaque に保つ)。 */
    void* m_Instance = nullptr;

    /** 直近 Tick の HMD ポーズ (session 未実装のため常に原点)。 */
    acs::game::XrPose m_HeadPose{};

    /** 直近 Tick の左コントローラ state。 */
    acs::game::XrControllerState m_Left{};

    /** 直近 Tick の右コントローラ state。 */
    acs::game::XrControllerState m_Right{};

    /** Init で記録されたプラットフォーム。 */
    acs::game::EXrPlatform m_Platform = acs::game::EXrPlatform::Unknown;

    /** 初期化済みフラグ (instance 生成成功で true)。 */
    bool m_bInitialized = false;

    /** passthrough 拡張が利用可能かどうか。 */
    bool m_bPassthroughSupported = false;

    /** passthrough の有効化が要求されているかどうか。 */
    bool m_bPassthroughRequested = false;

    /**
     * 実セッションが確立してポーズが更新されているか。
     *
     * @details session loop は本 TU では未実装なので常に false。
     */
    bool m_bSessionTracking = false;

    /** Tick() の「session loop 未実装」警告を一度だけ出すためのラッチ。 */
    bool m_bTickWarned = false;
};

/**
 * プロセス共有 singleton の実 Khronos bridge を返す。
 *
 * @details
 * 初回アクセス時に Init() を 1 回走らせる (失敗しても参照は返すので、呼び出し側は
 * IsInitialized() を見て fallback を判断する)。
 * @return プロセス共有の実 Khronos OpenXR bridge への参照。
 */
acs::game::IOpenXrBridge& GetDefaultKhronosOpenXrBridge() noexcept;

/**
 * gameframework の既定 provider に GetDefaultKhronosOpenXrBridge を登録する。
 *
 * @details
 * gameframework は本モジュール (ACS::OpenXr) に依存できない (循環依存) ため、結線は
 * backend 側 (本モジュール) から acs::game::SetOpenXrBridgeProvider() を呼んで行う。
 * アプリは起動時に一度これを呼ぶだけで、以降は backend 非依存に
 * acs::game::GetDefaultOpenXrBridge() で実 Khronos OpenXR bridge を取得できる。
 */
void InstallOpenXrAsDefault() noexcept;

} // namespace acs::openxr
