// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game {

/**
 * 天候種別。
 *
 * @details
 * レンダラ / 粒子 / 音響側で参照する。save 互換性のため新しい値は末尾に追加し、
 * 既存値の数値と意味は変更しない。
 */
enum class EWeatherKind : u8 {
    /** 快晴 / 通常。 */
    Clear     = 0,

    /** 曇り。 */
    Cloudy    = 1,

    /** 雨。 */
    Rain      = 2,

    /** 豪雨。 */
    HeavyRain = 3,

    /** 雪。 */
    Snow      = 4,

    /** 嵐 (強風 + 豪雨)。 */
    Storm     = 5,

    /** 霧。 */
    Fog       = 6,

    /** 砂嵐 (強風 + 視界不良)。 */
    Sandstorm = 7,
};

/**
 * 天候モードを保持し、現在 → 目標天候の線形遷移と描画修飾係数を提供するシステム。
 *
 * @details
 * CAmbientDirector (時刻補間) と直交し、天候ごとの ambient 倍率 / 粒子密度 /
 * sky tint / 風強さ / 霧密度を 1 つの LUT から引いて current/target 間で Lerp する。
 * non-copy / non-move で AScene 等に値メンバとして持たせ、Tick(dt) で遷移を進める。
 */
class CWeatherSystem {
public:
    /** 既定状態 (Clear、遷移完了済み、風向き東) で構築する。 */
    CWeatherSystem() noexcept = default;

    /** 破棄する (リソースなし)。 */
    ~CWeatherSystem() noexcept = default;

    /** コピー禁止 (Manager 系と統一)。 */
    CWeatherSystem(const CWeatherSystem&)            = delete;

    /** コピー代入も禁止。 */
    CWeatherSystem& operator=(const CWeatherSystem&) = delete;

    /** ムーブ禁止 (Manager 系と統一)。 */
    CWeatherSystem(CWeatherSystem&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CWeatherSystem& operator=(CWeatherSystem&&)      = delete;

    /**
     * 目標天候を設定し、遷移を開始する。
     *
     * @details
     * 既に同一天候かつ完了状態なら遷移をスキップして即完了状態に揃える。遷移中の
     * 再呼出は現 target を current に固定してから新 target へ向け再スタートする。
     * @param kind 目標とする天候。
     * @param transition_duration 遷移に掛ける秒数 (<=0 は即時切替、既定 5.0)。
     */
    void SetWeather(EWeatherKind kind, f32 transition_duration = 5.0f) noexcept;

    /**
     * 現在の (遷移元) 天候を返す。
     *
     * @return current の EWeatherKind。
     */
    EWeatherKind CurrentWeather() const noexcept { return m_Current; }

    /**
     * 目標 (遷移先) 天候を返す。
     *
     * @return target の EWeatherKind。
     */
    EWeatherKind TargetWeather()  const noexcept { return m_Target; }

    /**
     * 遷移の進行度を返す。
     *
     * @return [0, 1] の進行度。1 で遷移完了 (current == target に snap 済み)。
     */
    f32 TransitionT() const noexcept { return m_TransitionT; }

    /**
     * 遷移を 1 フレーム分進める。
     *
     * @details
     * dt < 0 は 0 にクランプ。完了状態なら何もしない。transition_elapsed が
     * duration に達したら current を target にスナップし transition_t を 1 に固定する。
     * @param dt 経過秒 (リアル秒)。
     */
    void Tick(f32 dt) noexcept;

    /**
     * ambient 輝度倍率を返す (current → target を transition_t で Lerp)。
     *
     * @return ambient 倍率 (1.0 = 通常輝度、Storm / Sandstorm 中は 0.5 まで暗化)。
     */
    f32 AmbientLightMultiplier() const noexcept;

    /**
     * 粒子密度倍率を返す (current → target を transition_t で Lerp)。
     *
     * @return 粒子密度倍率 (Clear = 0、雨 / 雪 / 嵐中で増加)。
     */
    f32 ParticleDensity() const noexcept;

    /**
     * sky color に乗算する補正を返す (current → target を transition_t で Lerp)。
     *
     * @return sky tint 乗算係数 (1,1,1 = 無補正、Sandstorm 時は黄褐色など)。
     */
    FVec3 SkyTintMultiplier() const noexcept;

    /**
     * 風の強さを返す (current → target を transition_t で Lerp)。
     *
     * @return 風強さ [0, 1] (Storm = 1.0、Clear = 0.0、Rain / Snow は中間値)。
     */
    f32 WindStrength() const noexcept;

    /**
     * 霧密度倍率を返す (current → target を transition_t で Lerp)。
     *
     * @return 霧密度倍率 (1 = 通常、Fog / Sandstorm で大きくなる)。
     */
    f32 FogDensityMultiplier() const noexcept;

    /**
     * 風向きを設定する (天候とは独立)。
     *
     * @param dir 風向きベクトル。
     */
    void SetWindDirection(FVec2 dir) noexcept { m_WindDir = dir; }

    /**
     * 現在の風向きを返す。
     *
     * @return 設定済みの風向きベクトル。
     */
    FVec2 WindDirection() const noexcept { return m_WindDir; }

    /** 初期化直後の状態 (Clear、遷移完了、風向き東) へ戻す。 */
    void Reset() noexcept;

public:
    /**
     * 天候ごとの描画修飾パラメータ。
     *
     * @details LUT として .cpp にテーブルで持つ。各値は対応する getter のコメント参照。
     */
    struct FKindParams {
        /** ambient 輝度倍率 (Storm/Sandstorm で暗化)。 */
        f32  ambient_mult;

        /** 粒子発生率倍率 (Clear=0、HeavyRain/Storm で最大)。 */
        f32  particle_density;

        /** sky color に乗算する補正 (1,1,1 = 補正なし)。 */
        FVec3 sky_tint;

        /** 風の強さ [0,1] (Storm/Sandstorm = 1)。 */
        f32  wind_strength;

        /** 霧密度倍率 (Fog で大、Sandstorm でも視界不良)。 */
        f32  fog_density;
    };

private:
    /**
     * 天候種別に対応する修飾パラメータを LUT から引く。
     *
     * @param k 引く天候種別。
 * @return k に対応する FKindParams への const 参照 (範囲外は Clear にフォールバック)。
     */
    static const FKindParams& Params(EWeatherKind k) noexcept;

    /** 現在 (遷移元) の天候。 */
    EWeatherKind m_Current = EWeatherKind::Clear;

    /** 目標 (遷移先) の天候。 */
    EWeatherKind m_Target  = EWeatherKind::Clear;

    /** 遷移にかける総時間 [s] (Tick で進行)。 */
    f32 m_TransitionDuration = 0.0f;

    /** 遷移開始からの経過時間 [s]。 */
    f32 m_TransitionElapsed  = 0.0f;

    /** 遷移の進行度 [0, 1] (1 = 完了。current == target かつ duration <= 0 のとき常に 1)。 */
    f32 m_TransitionT = 1.0f;

    /** 風向きベクトル (天候とは独立、既定は東向き)。 */
    FVec2 m_WindDir{1.0f, 0.0f};
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FWeatherSystem = CWeatherSystem;

} // namespace acs::game
