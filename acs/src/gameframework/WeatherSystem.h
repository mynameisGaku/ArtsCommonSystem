// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar Q — FWeatherSystem (天候モード)
//
// 役割:
//   FAmbientDirector (時刻補間) と直交する「天候」状態を保持し、現在天候 →
//   ターゲット天候への線形遷移と、各天候に対応する描画 / lighting 修飾係数
//   (ambient 倍率 / 粒子密度 / sky tint / 風 / 霧密度) を提供する。
//   レンダラ / FEffectSystem / ParticleSystem 側は毎フレーム本クラスから係数を
//   pull するだけで天候表情を反映できる。
//
// 使い方:
//   class WorldScene : public Scene {
//       acs::game::FWeatherSystem _weather;
//
//       void OnEnter() noexcept override {
//           _weather.SetWeather(acs::game::EWeatherKind::Clear);
//           _weather.SetWindDirection(acs::FVec2{1.0f, 0.0f});
//       }
//       void OnUpdate(f32 dt) noexcept override {
//           _weather.Tick(dt);
//           // 雨へ 8 秒掛けて遷移
//           if (player.EnteredRainZone()) _weather.SetWeather(
//               acs::game::EWeatherKind::Rain, 8.0f);
//
//           FRenderer().SetAmbientMultiplier(_weather.AmbientLightMultiplier());
//           FRenderer().SetSkyTint          (_weather.SkyTintMultiplier());
//           FRenderer().SetFogDensityScale  (_weather.FogDensityMultiplier());
//           FParticles().SetGlobalDensity   (_weather.ParticleDensity());
//           Wind().SetVector(_weather.WindDirection() * _weather.WindStrength());
//       }
//   };
//
// 設計選択 (Pillar Q Phase 2):
//   ・**8 種の固定 enum**: Clear / Cloudy / Rain / HeavyRain / Snow / Storm /
//     Fog / Sandstorm。表現の幅を確保しつつ、各描画モディファイアを 1 テーブルで
//     LUT 引きできる粒度 (`KindParams`)。FAmbientDirector と同様、利用者は
//     「現在 / 目標 / 遷移時間」だけ意識すれば良い。
//   ・**線形遷移**: SetWeather(target, duration) → transition_t を 0→1 で線形に
//     進める。current/target の全モディファイア値を t で Lerp。Hermite / 自然
//     な雲量カーブは Phase 3 で。
//   ・**遷移完了で snap**: transition_t == 1.0 で current = target に書き換え、
//     transition_t を 1.0f に固定。次に SetWeather を呼ぶまで「完了状態」に
//     とどまる。これにより current/target の lerp は常に t∈[0,1] の閉区間で済む。
//   ・**WindDirection は天候とは独立**: 「南風が雨を運ぶ」「無風の雪」など
//     表現が衝突するため、wind 方向はユーザーが任意に設定可能 (天候は強さのみ
//     決める)。デフォルトは (1, 0) = 東向き。
//   ・**ambient/sky 修飾は乗算**: FAmbientDirector の出力に「天候による調整」を
//     掛けるだけで時刻 × 天候の合成が完了する設計。Storm/Sandstorm 時は
//     ambient を 0.5 倍に暗くするなど。
//
// 範囲外 (Phase 3+ で):
//   ・天候プロファイルのカスタム差し替え API
//   ・季節遷移 / 確率的天候遷移マシン
//   ・雷フラッシュ / 雷音タイミングの emitter 統合
//   ・GPU 側 wetness map 連動 (濡れ表現)
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game {

// 天候種別。レンダラ / 粒子 / 音響側で参照する。値は安定なので save に書ける
// (将来 enum 追加時は末尾追加のみ、既存値の意味は不変とする規約)。
enum class EWeatherKind : u8 {
    Clear     = 0,  // 快晴 / 通常
    Cloudy    = 1,  // 曇り
    Rain      = 2,  // 雨
    HeavyRain = 3,  // 豪雨
    Snow      = 4,  // 雪
    Storm     = 5,  // 嵐 (強風 + 豪雨)
    Fog       = 6,  // 霧
    Sandstorm = 7,  // 砂嵐 (強風 + 視界不良)
};

class FWeatherSystem {
public:
    FWeatherSystem() noexcept = default;
    ~FWeatherSystem() noexcept = default;

    FWeatherSystem(const FWeatherSystem&)            = delete;
    FWeatherSystem& operator=(const FWeatherSystem&) = delete;
    FWeatherSystem(FWeatherSystem&&)                 = delete;
    FWeatherSystem& operator=(FWeatherSystem&&)      = delete;

    // ----- 天候設定 -----
    // 同一天候への設定は遷移をスキップして即完了状態にする。
    // transition_duration <= 0 は即時切替 (transition_t = 1)。
    void SetWeather(EWeatherKind kind, f32 transition_duration = 5.0f) noexcept;

    EWeatherKind CurrentWeather() const noexcept { return _current; }
    EWeatherKind TargetWeather()  const noexcept { return _target; }

    // [0, 1]。1 で遷移完了 (current == target に snap 済み)。
    f32 TransitionT() const noexcept { return _transition_t; }

    // ----- 進行 (FSceneServices などから毎フレーム呼ぶ。dt はリアル秒) -----
    void Tick(f32 dt) noexcept;

    // ----- 描画 / lighting 用 modifier (current → target を transition_t で Lerp) -----
    // 1.0 = 通常輝度。Storm / Sandstorm 中は 0.5 まで暗化。
    f32 AmbientLightMultiplier() const noexcept;
    // 雨 / 雪 / 嵐中で増える粒子密度倍率 (Clear = 0)。
    f32 ParticleDensity() const noexcept;
    // sky color に乗算する補正 (1,1,1 = 無補正)。Sandstorm 時は黄褐色など。
    FVec3 SkyTintMultiplier() const noexcept;
    // [0, 1]。Storm = 1.0、Clear = 0.0、Rain / Snow は中間値。
    f32 WindStrength() const noexcept;
    // Fog 中で増加 (1 = 通常)。Fog / Sandstorm で大きくなる。
    f32 FogDensityMultiplier() const noexcept;

    // ----- 風向き (天候とは独立) -----
    void SetWindDirection(FVec2 dir) noexcept { _wind_dir = dir; }
    FVec2 WindDirection() const noexcept { return _wind_dir; }

    // ----- リセット (初期化直後の状態へ戻す) -----
    void Reset() noexcept;

public:
    // 天候ごとの修飾パラメータ。LUT として .cpp に持つ。
    // 値の意味は各 getter のコメントを参照。
    struct KindParams {
        f32  ambient_mult;
        f32  particle_density;
        FVec3 sky_tint;
        f32  wind_strength;
        f32  fog_density;
    };

private:
    static const KindParams& Params(EWeatherKind k) noexcept;

    EWeatherKind _current = EWeatherKind::Clear;
    EWeatherKind _target  = EWeatherKind::Clear;

    // 遷移残り時間 [s]。> 0 の間 Tick で減算し、transition_t を更新する。
    f32 _transition_duration = 0.0f;
    f32 _transition_elapsed  = 0.0f;
    // [0, 1]。1 = 完了。current == target かつ duration <= 0 のとき常に 1。
    f32 _transition_t = 1.0f;

    FVec2 _wind_dir{1.0f, 0.0f};
};

} // namespace acs::game
