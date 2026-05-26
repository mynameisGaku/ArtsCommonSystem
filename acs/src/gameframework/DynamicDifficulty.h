// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar U — FDynamicDifficulty (DDA: Dynamic Difficulty Adjustment)
//
// プレイヤーの実プレイスタッツ (死亡 / 撃破 / リトライ / クリア時間 / パワー
// アップ取得) を追跡し、それを元に「Easy / Normal / Hard / VeryHard / Adaptive」
// の 5 モードで難易度を切り替える。Adaptive モードでは skill 推定値から
// 連続値 [0, 1] の難易度を smooth lerp で目標へ寄せ、各種乗数 (敵 HP / ダメージ
// / 速度 / プレイヤー HP) を返す。
//
// 想定する位置付け (Pillar U Phase 2):
//   ・Pillar U Phase 1 (MlRuntime / Upscaler) は「外部 ML SDK seam」だった。
//     Phase 2 (本クラス) は ML を使わない **純内製の決定論的 DDA**。
//   ・**決定論ゾーンの外**: Adaptive の skill 推定 / smooth lerp は浮動小数の
//     非可換性を含むため、固定タイムステップでの再現性は保証しない。replay /
//     netcode 同期に乗せるなら `Adaptive` 以外 (Easy/Normal/Hard/VeryHard) を
//     使うか、難易度値そのものを replay に記録する運用を取ること。
//   ・FDamageFeedback / FProgression と独立: DDA は「乗数を提供する純粋 state
//     holder」で、ゲーム側の戦闘ロジックが乗数を pull して使う構造。
//
// 使い方:
//   class GameplayScene : public FScene {
//       acs::game::FDynamicDifficulty _dda;
//       void OnEnter() noexcept override {
//           _dda.Init(acs::game::EDifficultyLevel::Adaptive);
//       }
//       void OnPlayerDeath() noexcept {
//           _dda.RecordDeath();
//           _dda.RecordRetry();
//       }
//       void OnEnemyKilled() noexcept { _dda.RecordKill(); }
//       void OnLevelCleared(f32 time) noexcept { _dda.RecordLevelComplete(time); }
//       void OnUpdate(f32 dt) noexcept override {
//           _dda.Tick(dt);
//           // 戦闘ロジックは乗数を pull して使う
//           f32 enemy_hp_mul = _dda.EnemyHealthMultiplier();
//           f32 enemy_dmg    = _dda.EnemyDamageMultiplier();
//           // ...
//       }
//   };
//
// 設計選択 (Pillar U Phase 2):
//   ・**5 モード固定 + Adaptive**: ユーザーが UI で選ぶ離散モードと、自動調整の
//     Adaptive モードを enum 1 つに混ぜる。離散モード時は乗数も離散値を即返し、
//     `Tick()` は no-op に近い (Adaptive 時だけ smooth lerp を進める)。
//   ・**Adaptive の skill 推定**:
//      skill = saturate( kill_rate * w_k + clear_speed * w_c
//                      - retry_density * w_r - death_density * w_d )
//      target_difficulty = 1.0 - skill
//      → skill が高いほど高難易度に寄せる。
//      重み (w_k / w_c / w_r / w_d) は内部で固定 (Phase 2)。Phase 3 で
//      `SetAdaptiveWeights(...)` API で外部化検討。
//   ・**smooth lerp は FCamera2D と同じ framerate-independent**:
//      `t = 1 - exp(-rate * dt)` で dt 不変。rate = 0.5 で約 1.4 秒で 50% 詰める
//      ゆっくり追従。プレイヤーが「急にゲームが楽になった/難しくなった」と
//      感じない速度に意図的に抑える。
//   ・**乗数テーブル**:
//      離散モード: { Easy, Normal, Hard, VeryHard } 4 段階を表で持つ。
//      Adaptive: [0,1] の連続値を Easy(0.0) ↔ VeryHard(1.0) の 4 段表で
//         3 区間 (Easy-Normal / Normal-Hard / Hard-VeryHard) 線形補間。
//      これにより離散モードと Adaptive で乗数体系を揃え、ゲームコード側は
//      モード差を意識せずに `EnemyHealthMultiplier()` を pull すれば良い。
//   ・**PlayerHealthMultiplier は逆相関**: 難易度が高いほど player HP 倍率を
//     **下げる** (Easy で 1.5x / VeryHard で 0.75x) のが業界慣習。攻撃側の
//     乗数 3 種 (敵 HP / 敵ダメ / 敵速度) と組合せて全体の感触を作る。
//   ・**統計は単純加算 + average の incremental 計算**:
//     deaths / kills / retries / powerups は u32 加算。average_completion_time
//     は exponential moving average (EMA) で「直近のクリア時間」を反映。
//     全レベル全平均ではなく直近に重み付け、で skill 変動に追従しやすく。
//   ・**全 noexcept、非コピー・非ムーブ**: 他 Manager 系と統一。インスタンス
//     1 個前提 (FScene のメンバ持ち回り)。
//   ・**STL 不使用 / `<string>` 不使用**: ACS 規約。
//
// 範囲外 (Phase 3+):
//   ・skill 重みの外部設定 API
//   ・per-encounter difficulty (戦闘単位での個別調整)
//   ・persistence (FSaveSlot 経由で stats を残す → Phase 3 で連携検討)
//   ・ML 推論ベースの skill 推定 (Phase U-3 で MlRuntime と連携検討)
#pragma once

#include "foundation/Types.h"
#include "math/Math.h"

namespace acs::game {

// ---- EDifficultyLevel -------------------------------------------------------
// 5 モード。`Adaptive` 以外は離散値直接マップで Tick が no-op (近い動作)。
// `Adaptive` 時のみ Tick で skill→target→smooth lerp を進める。
enum class EDifficultyLevel : u8 {
    Easy     = 0,
    Normal   = 1,
    Hard     = 2,
    VeryHard = 3,
    Adaptive = 4,
};

// ---- FPlayerSkillStats ------------------------------------------------------
// プレイヤーの腕前判定に使う実プレイ統計。
// ・deaths_last_session  : 現セッション中の死亡回数
// ・kills_last_session   : 現セッション中の撃破回数
// ・retries_current_level: 現レベルのリトライ回数 (RecordLevelComplete で 0 リセット)
// ・average_completion_time : 直近クリア時間の指数移動平均 (s)。0 = 未計測
// ・powerups_collected   : 現セッション中のパワーアップ取得数
struct FPlayerSkillStats {
    u32 deaths_last_session   = 0;
    u32 kills_last_session    = 0;
    u32 retries_current_level = 0;
    f32 average_completion_time = 0.0f;
    u32 powerups_collected    = 0;
};

class FDynamicDifficulty {
public:
    FDynamicDifficulty()  noexcept = default;
    ~FDynamicDifficulty() noexcept = default;

    FDynamicDifficulty(const FDynamicDifficulty&)            = delete;
    FDynamicDifficulty& operator=(const FDynamicDifficulty&) = delete;
    FDynamicDifficulty(FDynamicDifficulty&&)                 = delete;
    FDynamicDifficulty& operator=(FDynamicDifficulty&&)      = delete;

    // ----- 初期化 / モード切替 -----
    // base_level: 初期モード。Adaptive 指定時は `_current_difficulty` を
    //             0.5 (= Normal 相当) スタートにして、Tick で target に寄せていく。
    void Init(EDifficultyLevel base_level = EDifficultyLevel::Normal) noexcept;

    // モード切替。離散モードへの変更は `_current_difficulty` を該当段の
    // 連続値 (Easy=0 / Normal=1/3 / Hard=2/3 / VeryHard=1) に即スナップ。
    // Adaptive へ切替時は現在値を保持して target に向かって lerp 続行。
    void SetMode(EDifficultyLevel mode) noexcept;

    EDifficultyLevel CurrentMode() const noexcept { return _mode; }

    // ----- 統計記録 (gameplay 側がイベント駆動で呼ぶ) -----
    void RecordDeath()           noexcept;
    void RecordKill()            noexcept;
    // time_taken: そのレベルのクリアにかかった秒数。負値は 0 として扱う。
    // average は EMA (係数 0.3) で更新、retries_current_level は 0 リセット。
    void RecordLevelComplete(f32 time_taken) noexcept;
    void RecordRetry()           noexcept;
    void RecordPowerupCollected() noexcept;

    // ----- 連続値難易度 [0, 1] -----
    // 離散モード: 該当段の固定値。Adaptive: smooth lerp 中の現在値。
    f32 CurrentDifficulty() const noexcept { return _current_difficulty; }

    // ----- 乗数 accessor (戦闘ロジックが pull) -----
    // 0.5 = Easy, 1.0 = Normal, 1.5 = Hard, 2.0 = VeryHard。
    // Adaptive 時は 4 段表を線形補間。
    f32 EnemyHealthMultiplier() const noexcept;
    // ダメージ乗数。Easy 0.6 / Normal 1.0 / Hard 1.4 / VeryHard 1.8。
    f32 EnemyDamageMultiplier() const noexcept;
    // 敵移動 / 攻撃速度乗数。Easy 0.85 / Normal 1.0 / Hard 1.15 / VeryHard 1.3。
    f32 EnemySpeedMultiplier()  const noexcept;
    // プレイヤー HP 倍率 (逆相関)。Easy 1.5 / Normal 1.0 / Hard 0.85 / VeryHard 0.75。
    f32 PlayerHealthMultiplier() const noexcept;

    // ----- 統計参照 / リセット -----
    const FPlayerSkillStats& FStats() const noexcept { return _stats; }
    // 統計のみ初期化。モード / current_difficulty は維持。NewGame/シーン切替向け。
    void ResetStats() noexcept;

    // ----- driver (Adaptive 時に skill 推定 → target → smooth lerp) -----
    // 離散モード時は no-op (current は離散値で固定済み)。
    void Tick(f32 dt) noexcept;

private:
    // Adaptive 時の target 難易度を skill 推定から算出 ([0,1])。
    f32 ComputeAdaptiveTarget() const noexcept;

    // 離散モード → 連続値 [0,1] 対応 (Easy=0, Normal=1/3, Hard=2/3, VeryHard=1)。
    static f32 ContinuousFromDiscrete(EDifficultyLevel m) noexcept;

    // 連続値 [0,1] を 4 段表 (vals[4]) で線形補間。区間 0.0..1/3..2/3..1.0。
    static f32 SampleCurve(f32 t, const f32 vals[4]) noexcept;

    EDifficultyLevel _mode               = EDifficultyLevel::Normal;
    f32             _current_difficulty = 0.333333f;  // Normal start (= 1/3)
    FPlayerSkillStats _stats {};

    // Adaptive 用の累積セッション時間 (= 統計密度の分母として使う)。
    f32 _session_time = 0.0f;

    // smooth lerp rate (1/s)。`1 - exp(-rate * dt)` で dt 不変な指数追従。
    // 0.5 で約 1.4 秒で 50% 詰める。意図的にゆっくり (急変回避)。
    static constexpr f32 kAdaptiveLerpRate = 0.5f;

    // EMA 係数: average_completion_time の更新で「新値 30% / 旧値 70%」。
    static constexpr f32 kCompletionTimeEmaAlpha = 0.3f;
};

} // namespace acs::game
