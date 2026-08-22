// SPDX-License-Identifier: Apache-2.0
// 天候プリセット、連続遷移、風向きを管理し、補間済みの描画係数を提供する。
//
// 8種の天候を1つの表で表現する。遷移中に目標が変わった場合は、その時点の補間値を
// 次の開始値として保存し、環境光、粒子、空、風、霧を同じ進行度で連続させる。
#include "gameframework/WeatherSystem.h"

#include "math/Math.h"

namespace acs::game {

namespace {

/** 非数と正負の無限大を標準ライブラリなしで拒否する。 */
bool IsFiniteWeatherValue_Internal(f32 value) noexcept
{
    constexpr f32 kMaximumFiniteFloat = 3.402823466e+38f;
    return value == value && value >= -kMaximumFiniteFloat && value <= kMaximumFiniteFloat;
}

/**
 * 天候別の描画係数表。
 *
 * @details
 * 値はゲーム向けの表現を目的とし、写実的な気象モデルではない。環境光倍率、粒子発生率、
 * 空の色補正、0～1の風強さ、霧密度倍率の順で保持する。配列の並びはEWeatherKindの
 * Clear=0からSandstorm=7までの数値順と一致させる。
 */
static const CWeatherSystem::FKindParams kParamsTable[8] = {
    // Clear      晴天: 何も足さず、何も引かない基準値
    { 1.00f, 0.00f, FVec3{1.00f, 1.00f, 1.00f}, 0.10f, 1.00f },
    // Cloudy     曇り: 全体の輝度を少し落とし、わずかに灰青寄りにする
    { 0.85f, 0.00f, FVec3{0.85f, 0.88f, 0.92f}, 0.25f, 1.20f },
    // Rain       雨: 暗化 + 雨滴粒子 + やや強い風 + 霧少々
    { 0.70f, 1.00f, FVec3{0.70f, 0.75f, 0.85f}, 0.50f, 1.40f },
    // HeavyRain  豪雨: さらに暗く粒子密度 2x
    { 0.55f, 2.00f, FVec3{0.55f, 0.60f, 0.70f}, 0.70f, 1.80f },
    // Snow       雪: 雨より明るくし、雪の反射を示す青白い色と弱風にする
    { 0.90f, 1.20f, FVec3{0.95f, 0.97f, 1.05f}, 0.30f, 1.30f },
    // Storm      嵐: 一番暗く、最大粒子 / 最大風 / 強い霧
    { 0.50f, 2.50f, FVec3{0.45f, 0.50f, 0.60f}, 1.00f, 2.00f },
    // Fog        霧: 明度ほぼ通常、霧密度のみ突出、粒子は出さない
    { 0.80f, 0.00f, FVec3{0.85f, 0.85f, 0.85f}, 0.15f, 4.00f },
    // Sandstorm  砂嵐: 黄褐色へ補正し、視界不良と最大風にする
    { 0.50f, 1.50f, FVec3{1.10f, 0.85f, 0.55f}, 1.00f, 3.00f },
};

/** 定義済み天候種別と描画係数表の要素数。 */
constexpr u32 kWeatherKindCount = static_cast<u32>(sizeof(kParamsTable) / sizeof(kParamsTable[0]));

} // namespace

/** 範囲外の天候種別をClearへ直す。 */
EWeatherKind CWeatherSystem::NormalizeKind_Internal(EWeatherKind kind) noexcept
{
    return static_cast<u32>(kind) < kWeatherKindCount ? kind : EWeatherKind::Clear;
}

/** 天候種別に対応する描画係数を表から引く。 */
const CWeatherSystem::FKindParams& CWeatherSystem::Params_Internal(EWeatherKind kind) noexcept
{
    return kParamsTable[static_cast<u32>(NormalizeKind_Internal(kind))];
}

/** 現在表示している全描画係数を同じ進行度で補間する。 */
CWeatherSystem::FKindParams CWeatherSystem::CurrentParams_Internal() const noexcept
{
    const FKindParams& target = Params_Internal(m_Target);
    FKindParams result{};
    result.ambient_mult = Lerp(m_TransitionStartParams.ambient_mult, target.ambient_mult, m_TransitionT);
    result.particle_density = Lerp(m_TransitionStartParams.particle_density, target.particle_density, m_TransitionT);
    result.sky_tint = Lerp(m_TransitionStartParams.sky_tint, target.sky_tint, m_TransitionT);
    result.wind_strength = Lerp(m_TransitionStartParams.wind_strength, target.wind_strength, m_TransitionT);
    result.fog_density = Lerp(m_TransitionStartParams.fog_density, target.fog_density, m_TransitionT);
    return result;
}

/** 指定天候へ即時切替し、全遷移値を完了状態へ揃える。 */
void CWeatherSystem::CompleteTransition_Internal(EWeatherKind kind) noexcept
{
    const EWeatherKind normalizedKind = NormalizeKind_Internal(kind);
    m_Current = normalizedKind;
    m_Target = normalizedKind;
    m_TransitionStartParams = Params_Internal(normalizedKind);
    m_TransitionDuration = 0.0f;
    m_TransitionElapsed = 0.0f;
    m_TransitionT = 1.0f;
}

/** 目標天候を設定し、現在の描画値から連続する遷移を開始する。 */
void CWeatherSystem::SetWeather(EWeatherKind kind, f32 transition_duration) noexcept
{
    const EWeatherKind normalizedKind = NormalizeKind_Internal(kind);
    if (!(transition_duration > 0.0f) || !IsFiniteWeatherValue_Internal(transition_duration)) {
        CompleteTransition_Internal(normalizedKind);
        return;
    }

    // 毎フレーム同じ目標が通知されても、経過時間を0へ戻さない。
    if (m_TransitionT < 1.0f && normalizedKind == m_Target) return;
    if (m_TransitionT >= 1.0f && normalizedKind == m_Current) {
        CompleteTransition_Internal(normalizedKind);
        return;
    }

    // 目標を書き換える前に現在の補間値を保存し、視覚上の不連続を防ぐ。
    m_TransitionStartParams = CurrentParams_Internal();
    m_Target = normalizedKind;
    m_TransitionDuration = transition_duration;
    m_TransitionElapsed = 0.0f;
    m_TransitionT = 0.0f;
}

/** 遷移を1フレーム分進め、残り時間へ達したら目標へ正確に完了する。 */
void CWeatherSystem::Tick(f32 dt) noexcept
{
    if (m_TransitionT >= 1.0f || !(dt > 0.0f)) return;
    if (!(m_TransitionDuration > 0.0f) || !IsFiniteWeatherValue_Internal(m_TransitionDuration)) {
        CompleteTransition_Internal(m_Target);
        return;
    }

    const f32 remaining = m_TransitionDuration - m_TransitionElapsed;
    if (!(remaining > 0.0f) || dt >= remaining) {
        CompleteTransition_Internal(m_Target);
        return;
    }

    m_TransitionElapsed += dt;
    m_TransitionT = m_TransitionElapsed / m_TransitionDuration;
}

/** 初期化直後の状態 (Clear、遷移完了、風向き東) へ戻す。 */
void CWeatherSystem::Reset() noexcept
{
    CompleteTransition_Internal(EWeatherKind::Clear);
    m_WindDir = FVec2{1.0f, 0.0f};
}

/** 環境光倍率を遷移開始値から目標値まで補間して返す。 */
f32 CWeatherSystem::AmbientLightMultiplier() const noexcept
{
    return Lerp(m_TransitionStartParams.ambient_mult, Params_Internal(m_Target).ambient_mult, m_TransitionT);
}

/** 粒子密度を遷移開始値から目標値まで補間して返す。 */
f32 CWeatherSystem::ParticleDensity() const noexcept
{
    return Lerp(m_TransitionStartParams.particle_density, Params_Internal(m_Target).particle_density, m_TransitionT);
}

/** 空の色補正を遷移開始値から目標値まで補間して返す。 */
FVec3 CWeatherSystem::SkyTintMultiplier() const noexcept
{
    return Lerp(m_TransitionStartParams.sky_tint, Params_Internal(m_Target).sky_tint, m_TransitionT);
}

/** 風強さを遷移開始値から目標値まで補間して返す。 */
f32 CWeatherSystem::WindStrength() const noexcept
{
    return Lerp(m_TransitionStartParams.wind_strength, Params_Internal(m_Target).wind_strength, m_TransitionT);
}

/** 霧密度を遷移開始値から目標値まで補間して返す。 */
f32 CWeatherSystem::FogDensityMultiplier() const noexcept
{
    return Lerp(m_TransitionStartParams.fog_density, Params_Internal(m_Target).fog_density, m_TransitionT);
}

} // namespace acs::game
