// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "math/Math.h"
#include "math/Vec.h"

namespace acs::game {

/**
 * 被ダメージ時の視覚フィードバックを集中管理する純粋 state holder。
 *
 * @details
 * 赤エッジオーバーレイ・方向矢印 (off-screen damage indicator)・death cam の 3 要素を
 * 一箇所で扱う。自身は描画もカメラ操作も行わず、各 accessor を描画/UI/カメラ制御コードが
 * pull して使う副作用ゼロ設計。赤エッジは累積加算 → 線形 decay、方向矢印は最後の被弾源で
 * 上書き、death cam は明示的な trigger/exit で制御する。state 重複を避けるため
 * 非コピー・非ムーブ。
 */
class CDamageFeedback {
public:
    /** 全 state を初期値 (フィードバックなし) で構築する。 */
    CDamageFeedback() noexcept = default;

    /** 破棄する。 */
    ~CDamageFeedback() noexcept = default;

    /** コピー禁止 (state 重複で death cam の整合性が壊れるため)。 */
    CDamageFeedback(const CDamageFeedback&)            = delete;

    /** コピー代入も禁止。 */
    CDamageFeedback& operator=(const CDamageFeedback&) = delete;

    /** ムーブ禁止。 */
    CDamageFeedback(CDamageFeedback&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CDamageFeedback& operator=(CDamageFeedback&&)      = delete;

    /**
     * ダメージを 1 件受け取り、赤エッジ強度を加算し方向矢印を更新する。
     *
     * @details
     * 赤エッジは amount * 0.1 を加算して [0,1] clamp。amount <= 0 は何もしない (回復は別系統)。
     * source_direction が零ベクトル相当のときは矢印を更新しない (環境ダメージ等の無方向ヒット)、
     * 非零なら正規化して保持し不透明度を 1.0 にリセットする。
     * @param amount 受けたダメージ量 (任意スケール、0.1 倍で赤エッジに加算)。
     * @param source_direction 攻撃源 → プレイヤー方向ベクトル (正規化は内部で実行。既定は無方向)。
     */
    void TakeDamage(f32 amount, FVec2 source_direction = FVec2{0.0f, 0.0f}) noexcept;

    /**
     * 各エフェクトの decay と death cam progress を 1 ステップ進める。
     *
     * @details real-time dt で毎フレーム呼ばれる前提。dt が負なら 0 扱いで state を破壊しない。
     * @param dt 前フレームからの経過秒。
     */
    void Tick(f32 dt) noexcept;

    /**
     * 画面端を赤く染める現在の強度を返す (描画 overlay が pull)。
     *
     * @return 赤エッジ強度 [0,1] (0 = なし、1 = 完全赤被せ)。
     */
    f32 ScreenEdgeRedIntensity() const noexcept { return m_RedIntensity; }

    /**
     * 方向矢印を描画すべきかを返す。
     *
     * @return 矢印の不透明度が 0 より大きければ true。
     */
    bool HasDirectionalIndicator() const noexcept { return m_DirIntensity > 0.0f; }

    /**
     * 矢印が指す方向ベクトルを返す (UI 層が pull)。
     *
     * @details HasDirectionalIndicator() == false のときの値は未定義扱い (利用側でガードすること)。
     * @return 正規化済みの矢印方向。
     */
    FVec2 DirectionalIndicator() const noexcept { return m_DirVec; }

    /**
     * 方向矢印の不透明度を返す。
     *
     * @return 矢印の不透明度 [0,1]。
     */
    f32 DirectionalIndicatorAlpha() const noexcept { return m_DirIntensity; }

    /**
     * death cam 演出を開始する (致命傷検知時に呼ぶ)。
     *
     * @details 既に active なら killer_pos のみ更新し progress は仕切り直さない。
     * @param killer_pos カメラを向ける対象の world 座標 (致命傷を与えた相手の位置)。
     */
    void TriggerDeathCam(FVec3 killer_pos) noexcept;

    /**
     * 現在 death cam 演出中かを返す。
     *
     * @return death cam が active なら true。
     */
    bool IsDeathCamActive() const noexcept { return m_DeathCamActive; }

    /**
     * カメラが LookAt すべき world 座標を返す。
     *
     * @return trigger 時の killer 位置。
     */
    FVec3 DeathCamTarget() const noexcept { return m_KillerPos; }

    /**
     * death cam の dramatic zoom progress を返す。
     *
     * @details Tick で 0 → 1 へ 2 秒で進み、その後は 1.0 で維持。zoom / DoF のイージングに使う。
     * @return progress [0,1]。
     */
    f32 DeathCamProgress() const noexcept { return m_DeathCamT; }

    /** death cam を明示的に解除する (例: リスポーン処理開始時)。progress も 0 に戻す。 */
    void ExitDeathCam() noexcept;

    /** 全 state を初期値に戻す (シーン切替 / respawn 用の一括クリア)。 */
    void Reset() noexcept;

private:
    /** 画面端を赤く染める強度 [0,1]。TakeDamage で加算、Tick で 2.0/s decay。 */
    f32 m_RedIntensity = 0.0f;

    /** 矢印の不透明度 [0,1]。TakeDamage(dir != 0) で 1.0、Tick で 2.0/s decay。 */
    f32  m_DirIntensity = 0.0f;

    /** 正規化済みの矢印方向ベクトル。最後に向きが指定された TakeDamage で更新。 */
    FVec2 m_DirVec       = FVec2{0.0f, 0.0f};

    /** death cam が現在 active かどうか。 */
    bool m_DeathCamActive = false;

    /** death cam の dramatic zoom progress [0,1]。Trigger 時 0、Tick で 0.5/s 加算。 */
    f32  m_DeathCamT      = 0.0f;

    /** カメラが見るべき killer 位置 (trigger 時にスナップ、以後 caller が読む)。 */
    FVec3 m_KillerPos       = FVec3{0.0f, 0.0f, 0.0f};

    /** 累積ダメージ総量。Reset で 0 に戻る。最近のダメージ密度を読む将来拡張用フック。 */
    f32 m_RecentDamageTotal = 0.0f;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FDamageFeedback = CDamageFeedback;

} // namespace acs::game
