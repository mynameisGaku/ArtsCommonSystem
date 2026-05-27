// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar Q — FWeatherSystem 実装
//
// 8 種の天候を 1 つの LUT (KindParams テーブル) で表現し、current/target を
// 線形補間する。Tick で transition_t を進行し、1.0 到達時に current = target に
// snap する。
#include "gameframework/WeatherSystem.h"

#include "math/Math.h"

namespace acs::game {

// ----- 天候別パラメータ LUT --------------------------------------------------
//
// 値はゲーム性重視 (写実気象モデルではない):
//   ambient_mult     : ambient 輝度倍率 (Storm/Sandstorm で暗化)
//   particle_density : 粒子発生率倍率 (Clear=0、HeavyRain/Storm で max)
//   sky_tint         : sky color に乗算 (1,1,1 = 補正なし)
//   wind_strength    : [0,1]、Storm/Sandstorm = 1
//   fog_density      : 霧密度倍率 (Fog で大、Sandstorm でも視界不良)
//
// 並びは EWeatherKind の数値順 (Clear=0 .. Sandstorm=7) と一致させる。
static const FWeatherSystem::KindParams kParamsTable[8] = {
    // Clear      晴天: 何も足さず、何も引かない基準値
    { 1.00f, 0.00f, FVec3{1.00f, 1.00f, 1.00f}, 0.10f, 1.00f },
    // Cloudy     曇り: 全体的に少しトーン落ち、わずかに灰青寄りに
    { 0.85f, 0.00f, FVec3{0.85f, 0.88f, 0.92f}, 0.25f, 1.20f },
    // Rain       雨: 暗化 + 雨滴粒子 + やや強い風 + 霧少々
    { 0.70f, 1.00f, FVec3{0.70f, 0.75f, 0.85f}, 0.50f, 1.40f },
    // HeavyRain  豪雨: さらに暗く粒子密度 2x
    { 0.55f, 2.00f, FVec3{0.55f, 0.60f, 0.70f}, 0.70f, 1.80f },
    // Snow       雪: 雨より明るく (雪の反射)、青白い tint、弱風
    { 0.90f, 1.20f, FVec3{0.95f, 0.97f, 1.05f}, 0.30f, 1.30f },
    // Storm      嵐: 一番暗く、最大粒子 / 最大風 / 強い霧
    { 0.50f, 2.50f, FVec3{0.45f, 0.50f, 0.60f}, 1.00f, 2.00f },
    // Fog        霧: 明度ほぼ通常、霧密度のみ突出、粒子は出さない
    { 0.80f, 0.00f, FVec3{0.85f, 0.85f, 0.85f}, 0.15f, 4.00f },
    // Sandstorm  砂嵐: 黄褐色 tint、視界不良、最大風
    { 0.50f, 1.50f, FVec3{1.10f, 0.85f, 0.55f}, 1.00f, 3.00f },
};

const FWeatherSystem::KindParams& FWeatherSystem::Params(EWeatherKind k) noexcept {
    // 範囲外は Clear にフォールバック (將来の enum 拡張で size > 8 となる場合の
    // 安全網)。現状の enum 定義では決して走らない。
    const u32 idx = static_cast<u32>(k);
    if (idx >= 8) return kParamsTable[0];
    return kParamsTable[idx];
}

// ----- 状態制御 -------------------------------------------------------------

void FWeatherSystem::SetWeather(EWeatherKind kind, f32 transition_duration) noexcept {
    // 同一天候: 既にその天候なので即完了状態に揃える。
    if (kind == m_Current && m_TransitionT >= 1.0f) {
        m_Target              = kind;
        m_TransitionDuration = 0.0f;
        m_TransitionElapsed  = 0.0f;
        m_TransitionT        = 1.0f;
        return;
    }

    // 遷移中に SetWeather を再呼出: 現在の混合値を「次の current」として固定
    // するのが理想だが、KindParams は離散なので簡略化 — 単純に current を
    // 「今までの target」に置き換え、新しい target に向け再スタートする。
    // (混合途中の急な切替は視覚的に気にならない範囲、Phase 3 で要検討。)
    if (m_TransitionT < 1.0f) {
        m_Current = m_Target;
    }

    m_Target = kind;

    if (transition_duration <= 0.0f) {
        // 即時切替: snap して完了状態に。
        m_Current             = kind;
        m_TransitionDuration = 0.0f;
        m_TransitionElapsed  = 0.0f;
        m_TransitionT        = 1.0f;
    } else {
        m_TransitionDuration = transition_duration;
        m_TransitionElapsed  = 0.0f;
        m_TransitionT        = 0.0f;
    }
}

void FWeatherSystem::Tick(f32 dt) noexcept {
    if (dt < 0.0f) dt = 0.0f;

    // 既に完了状態なら何もしない (浮動小数の累積ドリフトを避ける)。
    if (m_TransitionT >= 1.0f) return;
    if (m_TransitionDuration <= 0.0f) {
        // 念のため (SetWeather を経由しない代入で 0 になった場合のセーフネット)。
        m_Current      = m_Target;
        m_TransitionT = 1.0f;
        return;
    }

    m_TransitionElapsed += dt;
    if (m_TransitionElapsed >= m_TransitionDuration) {
        // 遷移完了: current を target にスナップし、t を 1 で固定。
        m_Current             = m_Target;
        m_TransitionElapsed  = m_TransitionDuration;
        m_TransitionT        = 1.0f;
    } else {
        m_TransitionT = m_TransitionElapsed / m_TransitionDuration;
    }
}

void FWeatherSystem::Reset() noexcept {
    m_Current             = EWeatherKind::Clear;
    m_Target              = EWeatherKind::Clear;
    m_TransitionDuration = 0.0f;
    m_TransitionElapsed  = 0.0f;
    m_TransitionT        = 1.0f;
    m_WindDir            = FVec2{1.0f, 0.0f};
}

// ----- 描画 / lighting 用 modifier (current → target を線形補間) ------------

f32 FWeatherSystem::AmbientLightMultiplier() const noexcept {
    const KindParams& a = Params(m_Current);
    const KindParams& b = Params(m_Target);
    return Lerp(a.ambient_mult, b.ambient_mult, m_TransitionT);
}

f32 FWeatherSystem::ParticleDensity() const noexcept {
    const KindParams& a = Params(m_Current);
    const KindParams& b = Params(m_Target);
    return Lerp(a.particle_density, b.particle_density, m_TransitionT);
}

FVec3 FWeatherSystem::SkyTintMultiplier() const noexcept {
    const KindParams& a = Params(m_Current);
    const KindParams& b = Params(m_Target);
    return Lerp(a.sky_tint, b.sky_tint, m_TransitionT);
}

f32 FWeatherSystem::WindStrength() const noexcept {
    const KindParams& a = Params(m_Current);
    const KindParams& b = Params(m_Target);
    return Lerp(a.wind_strength, b.wind_strength, m_TransitionT);
}

f32 FWeatherSystem::FogDensityMultiplier() const noexcept {
    const KindParams& a = Params(m_Current);
    const KindParams& b = Params(m_Target);
    return Lerp(a.fog_density, b.fog_density, m_TransitionT);
}

} // namespace acs::game
