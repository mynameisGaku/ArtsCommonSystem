// SPDX-License-Identifier: Apache-2.0
#ifndef ACS_GAMEFRAMEWORK_WEATHER_SYSTEM_H
#define ACS_GAMEFRAMEWORK_WEATHER_SYSTEM_H

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
 * 天候モードを保持し、現在の描画状態から目標天候まで連続遷移させるシステム。
 *
 * @details
 * CAmbientDirector の時刻補間とは独立し、天候ごとの環境光倍率、粒子密度、空の色補正、
 * 風強さ、霧密度を 1 つの表から引いて補間する。コピーと移動は行わず、AScene などに
 * 値メンバーとして持たせ、Tick(dt) で遷移を進める。
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
     * 遷移中に別の天候を指定した場合は、その時点の補間済み描画値を新しい開始値として
     * 保持する。同じ目標を正の時間で再指定した場合は、毎フレームの再指定で遷移が停止しないよう
     * 進行中の遷移を維持する。0以下または非有限の時間は即時切替として扱う。
     * @param kind 目標とする天候。
     * @param transition_duration 遷移に掛ける秒数。0以下または非有限なら即時切替する。
     */
    void SetWeather(EWeatherKind kind, f32 transition_duration = 5.0f) noexcept;

    /**
     * 最後に遷移を完了した天候を返す。
     *
     * @return 遷移中は直前に完了した天候、完了後は目標天候。
     */
    EWeatherKind CurrentWeather() const noexcept { return m_Current; }

    /**
     * 目標 (遷移先) 天候を返す。
     *
     * @return 目標の EWeatherKind。範囲外の入力は Clear に直される。
     */
    EWeatherKind TargetWeather()  const noexcept { return m_Target; }

    /**
     * 遷移の進行度を返す。
     *
     * @return 0～1 の進行度。1 で遷移完了済み。
     */
    f32 TransitionT() const noexcept { return m_TransitionT; }

    /**
     * 遷移を 1 フレーム分進める。
     *
     * @details
     * 0以下または非数の dt は進行させない。残り時間以上の dt は加算前に判定して完了させ、
     * 加算あふれによる非数を作らない。
     * @param dt 経過秒 (リアル秒)。
     */
    void Tick(f32 dt) noexcept;

    /**
     * 環境光の輝度倍率を返す。
     *
     * @return 補間済みの環境光倍率。1.0が通常輝度。
     */
    f32 AmbientLightMultiplier() const noexcept;

    /**
     * 粒子密度倍率を返す。
     *
     * @return 粒子密度倍率 (Clear = 0、雨 / 雪 / 嵐中で増加)。
     */
    f32 ParticleDensity() const noexcept;

    /**
     * 空の色へ乗算する補正を返す。
     *
     * @return 補間済みの色補正。1,1,1は無補正。
     */
    FVec3 SkyTintMultiplier() const noexcept;

    /**
     * 風の強さを返す。
     *
     * @return 風強さ [0, 1] (Storm = 1.0、Clear = 0.0、Rain / Snow は中間値)。
     */
    f32 WindStrength() const noexcept;

    /**
     * 霧密度倍率を返す。
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
     * @details 実装ファイルの固定表に保持する。各値は対応する取得関数の説明を参照する。
     */
    struct FKindParams {
        /** 環境光の輝度倍率。StormとSandstormでは暗くする。 */
        f32 ambient_mult = 1.0f;

        /** 粒子発生率の倍率。Clearは0、HeavyRainとStormでは最大になる。 */
        f32 particle_density = 0.0f;

        /** 空の色へ乗算する補正。1,1,1は無補正。 */
        FVec3 sky_tint{1.0f, 1.0f, 1.0f};

        /** 0～1の風強さ。StormとSandstormは1。 */
        f32 wind_strength = 0.1f;

        /** 霧密度倍率。Fogで最大になり、Sandstormでも視界を狭める。 */
        f32 fog_density = 1.0f;
    };

private:
    /**
     * 天候種別に対応する描画係数を固定表から引く。
     *
     * @param kind 引く天候種別。
     * @return kindに対応するFKindParams。範囲外はClearへ戻す。
     */
    static const FKindParams& Params_Internal(EWeatherKind kind) noexcept;

    /**
     * 保存値や外部入力の天候種別を定義済み範囲へ直す。
     *
     * @param kind 検査する天候種別。
     * @return 定義済みならkind、範囲外ならClear。
     */
    static EWeatherKind NormalizeKind_Internal(EWeatherKind kind) noexcept;

    /**
     * 現在表示している全描画係数を同じ進行度で補間する。
     *
     * @return 遷移開始値と目標値を現在の進行度で混ぜた値。
     */
    FKindParams CurrentParams_Internal() const noexcept;

    /**
     * 指定天候へ即時切替し、遷移状態と開始値を完了状態へ揃える。
     *
     * @param kind 正規化前でも受け付ける完了先天候。
     */
    void CompleteTransition_Internal(EWeatherKind kind) noexcept;

    /** 最後に遷移を完了した天候。 */
    EWeatherKind m_Current = EWeatherKind::Clear;

    /** 目標 (遷移先) の天候。 */
    EWeatherKind m_Target  = EWeatherKind::Clear;

    /** 現在の遷移を開始した時点の連続な描画係数。 */
    FKindParams m_TransitionStartParams{};

    /** 遷移にかける総時間 [s] (Tick で進行)。 */
    f32 m_TransitionDuration = 0.0f;

    /** 遷移開始からの経過時間 [s]。 */
    f32 m_TransitionElapsed  = 0.0f;

    /** 0～1の遷移進行度。1は完了状態。 */
    f32 m_TransitionT = 1.0f;

    /** 風向きベクトル (天候とは独立、既定は東向き)。 */
    FVec2 m_WindDir{1.0f, 0.0f};
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FWeatherSystem = CWeatherSystem;

} // namespace acs::game

#endif // ACS_GAMEFRAMEWORK_WEATHER_SYSTEM_H
