// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar U — CDynamicDifficulty (DDA: Dynamic Difficulty Adjustment)
//
// プレイヤーの実プレイスタッツ (死亡 / 撃破 / リトライ / クリア時間 / パワー
// アップ取得) を追跡し、それを元に「Easy / Normal / Hard / VeryHard / Adaptive」
// の 5 モードで難易度を切り替える。Adaptive モードでは skill 推定値から
// 連続値 [0, 1] の難易度を smooth lerp で目標へ寄せ、各種乗数 (敵 HP / ダメージ
// / 速度 / プレイヤー HP) を返す。
//
// 想定する位置付け (Pillar U):
//   ・本クラスは ML を使わない **純内製の決定論的 DDA**。
//   ・**決定論ゾーンの外**: Adaptive の skill 推定 / smooth lerp は浮動小数の
//     非可換性を含むため、固定タイムステップでの再現性は保証しない。replay /
//     netcode 同期に乗せるなら `Adaptive` 以外 (Easy/Normal/Hard/VeryHard) を
//     使うか、難易度値そのものを replay に記録する運用を取ること。
//   ・CDamageFeedback / CProgression と独立: DDA は「乗数を提供する純粋 state
//     holder」で、ゲーム側の戦闘ロジックが乗数を pull して使う構造。
//
// 使い方:
//   class AGameplayScene : public AScene {
//       acs::game::CDynamicDifficulty m_Dda;
//       void OnEnter() noexcept override {
//           m_Dda.Init(acs::game::EDifficultyLevel::Adaptive);
//       }
//       void OnPlayerDeath() noexcept {
//           m_Dda.RecordDeath();
//           m_Dda.RecordRetry();
//       }
//       void OnEnemyKilled() noexcept { m_Dda.RecordKill(); }
//       void OnLevelCleared(f32 time) noexcept { m_Dda.RecordLevelComplete(time); }
//       void OnUpdate(f32 dt) noexcept override {
//           m_Dda.Tick(dt);
//           // 戦闘ロジックは乗数を pull して使う
//           f32 enemy_hp_mul = m_Dda.EnemyHealthMultiplier();
//           f32 enemy_dmg    = m_Dda.EnemyDamageMultiplier();
//           // ...
//       }
//   };
//
// 設計選択 (Pillar U):
//   ・**5 モード固定 + Adaptive**: ユーザーが UI で選ぶ離散モードと、自動調整の
//     Adaptive モードを enum 1 つに混ぜる。離散モード時は乗数も離散値を即返し、
//     `Tick()` は no-op に近い (Adaptive 時だけ smooth lerp を進める)。
//   ・**Adaptive の skill 推定**:
//      skill = saturate( kill_rate * w_k + clear_speed * w_c
//                      - retry_density * w_r - death_density * w_d )
//      target_difficulty = 1.0 - skill
//      → skill が高いほど高難易度に寄せる。
//      重み (w_k / w_c / w_r / w_d) は内部で固定。
//   ・**smooth lerp は CCamera2D と同じ framerate-independent**:
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
//     1 個前提 (AScene のメンバ持ち回り)。
//   ・**STL 不使用 / `<string>` 不使用**: ACS 規約。
//
// 範囲外:
//   ・skill 重みの外部設定 API
//   ・per-encounter difficulty (戦闘単位での個別調整)
//   ・persistence (TSaveSlot 経由で stats を残す)
//   ・ML 推論ベースの skill 推定
#pragma once

#include "foundation/Types.h"
#include "math/Math.h"

namespace acs::game {

/**
 * 難易度モード。
 *
 * @details
 * `Adaptive` 以外は離散値直接マップで Tick がほぼ no-op。`Adaptive` 時のみ Tick で
 * skill 推定 → target → smooth lerp を進める。
 */
enum class EDifficultyLevel : u8 {
    /** 易しい (敵弱体・player HP 増)。 */
    Easy     = 0,

    /** 標準。 */
    Normal   = 1,

    /** 難しい。 */
    Hard     = 2,

    /** 非常に難しい。 */
    VeryHard = 3,

    /** プレイ統計から自動調整する適応モード。 */
    Adaptive = 4,
};

/**
 * プレイヤーの腕前判定に使う実プレイ統計。
 */
struct FPlayerSkillStats {
    /** 現セッション中の死亡回数。 */
    u32 deaths_last_session   = 0;

    /** 現セッション中の撃破回数。 */
    u32 kills_last_session    = 0;

    /** 現レベルのリトライ回数 (RecordLevelComplete で 0 リセット)。 */
    u32 retries_current_level = 0;

    /** 直近クリア時間の指数移動平均 (秒)。0 = 未計測。 */
    f32 average_completion_time = 0.0f;

    /** 現セッション中のパワーアップ取得数。 */
    u32 powerups_collected    = 0;
};

/**
 * 内製の決定論的 DDA (Dynamic Difficulty Adjustment)。
 *
 * @details
 * プレイヤーの実プレイ統計 (死亡 / 撃破 / リトライ / クリア時間) を追跡し、5 モード
 * (Easy/Normal/Hard/VeryHard/Adaptive) で難易度を切り替える。Adaptive では skill 推定値
 * から連続難易度 [0,1] を framerate-independent な smooth lerp で目標へ寄せ、敵 HP /
 * 敵ダメージ / 敵速度 / player HP の各乗数を返す。インスタンス 1 個前提の非コピー・非ムーブ型。
 */
class CDynamicDifficulty {
public:
    /** Normal 相当の初期状態で構築する (本格的な初期化は Init)。 */
    CDynamicDifficulty()  noexcept = default;

    /** 破棄する。 */
    ~CDynamicDifficulty() noexcept = default;

    /** コピー禁止 (AScene が単独所有する想定)。 */
    CDynamicDifficulty(const CDynamicDifficulty&)            = delete;

    /** コピー代入も禁止。 */
    CDynamicDifficulty& operator=(const CDynamicDifficulty&) = delete;

    /** ムーブ禁止。 */
    CDynamicDifficulty(CDynamicDifficulty&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CDynamicDifficulty& operator=(CDynamicDifficulty&&)      = delete;

    /**
     * 統計を初期化して base_level に切り替える。
     *
     * @details
     * Adaptive 指定時は m_CurrentDifficulty を 0.5 (中庸) スタートにして Tick で target へ
     * 寄せていく。離散モード指定時は該当段の連続値にスナップする。
     * @param base_level 初期モード (既定 Normal)。
     */
    void Init(EDifficultyLevel base_level = EDifficultyLevel::Normal) noexcept;

    /**
     * モードを切り替える。
     *
     * @details
     * 離散モードへの変更は m_CurrentDifficulty を該当段の連続値 (Easy=0 / Normal=1/3 /
     * Hard=2/3 / VeryHard=1) に即スナップする。Adaptive への切替時は現在値を保持して target
     * に向かって lerp を続行する。
     * @param mode 切り替え先のモード。
     */
    void SetMode(EDifficultyLevel mode) noexcept;

    /**
     * 現在のモードを返す。
     *
     * @return 現在の EDifficultyLevel。
     */
    EDifficultyLevel CurrentMode() const noexcept { return m_Mode; }

    /** 死亡を 1 回記録する (u32 max でクランプ)。 */
    void RecordDeath()           noexcept;

    /** 撃破を 1 回記録する (u32 max でクランプ)。 */
    void RecordKill()            noexcept;

    /**
     * レベルクリアを記録する。
     *
     * @details
     * average_completion_time を EMA (係数 0.3) で更新し、retries_current_level を 0 リセットする。
     * @param time_taken そのレベルのクリアにかかった秒数。負値は 0 として扱う。
     */
    void RecordLevelComplete(f32 time_taken) noexcept;

    /** リトライを 1 回記録する (u32 max でクランプ)。 */
    void RecordRetry()           noexcept;

    /** パワーアップ取得を 1 回記録する (u32 max でクランプ)。 */
    void RecordPowerupCollected() noexcept;

    /**
     * 現在の連続値難易度 [0,1] を返す。
     *
     * @return 離散モードは該当段の固定値、Adaptive は smooth lerp 中の現在値。
     */
    f32 CurrentDifficulty() const noexcept { return m_CurrentDifficulty; }

    /**
     * 敵 HP 乗数を返す。
     *
     * @details Easy 0.5 / Normal 1.0 / Hard 1.5 / VeryHard 2.0。Adaptive 時は 4 段表を線形補間。
     * @return 敵 HP 乗数。
     */
    f32 EnemyHealthMultiplier() const noexcept;

    /**
     * 敵ダメージ乗数を返す。
     *
     * @details Easy 0.6 / Normal 1.0 / Hard 1.4 / VeryHard 1.8。Adaptive 時は 4 段表を線形補間。
     * @return 敵ダメージ乗数。
     */
    f32 EnemyDamageMultiplier() const noexcept;

    /**
     * 敵移動 / 攻撃速度乗数を返す。
     *
     * @details Easy 0.85 / Normal 1.0 / Hard 1.15 / VeryHard 1.3。Adaptive 時は 4 段表を線形補間。
     * @return 敵速度乗数。
     */
    f32 EnemySpeedMultiplier()  const noexcept;

    /**
     * プレイヤー HP 倍率を返す (難易度と逆相関)。
     *
     * @details Easy 1.5 / Normal 1.0 / Hard 0.85 / VeryHard 0.75。Adaptive 時は 4 段表を線形補間。
     * @return プレイヤー HP 倍率。
     */
    f32 PlayerHealthMultiplier() const noexcept;

    /**
     * 統計への const 参照を返す。
     *
     * @return FPlayerSkillStats への参照。
     */
    const FPlayerSkillStats& Stats() const noexcept { return _stats; }

    /** 統計とセッション時間のみ初期化する (モード / current_difficulty は維持。NewGame・シーン切替向け)。 */
    void ResetStats() noexcept;

    /**
     * 毎フレーム呼ぶ driver。
     *
     * @details
     * session_time を進め、Adaptive 時は skill 推定 → target → framerate-independent な
     * exponential lerp で current_difficulty を target へ寄せる。離散モード時は値が固定済みなので
     * session_time の加算のみ。
     * @param dt 経過秒。負値は 0 として扱う。
     */
    void Tick(f32 dt) noexcept;

private:
    /**
     * Adaptive 時の target 難易度を skill 推定から算出する。
     *
     * @return target 難易度 [0,1] (= 1 - skill)。
     */
    f32 ComputeAdaptiveTarget() const noexcept;

    /**
     * 離散モードを連続値 [0,1] に変換する。
     *
     * @param m 離散モード (Easy=0 / Normal=1/3 / Hard=2/3 / VeryHard=1)。
     * @return 対応する連続値 (Adaptive 渡しは fallback で 1/3)。
     */
    static f32 ContinuousFromDiscrete(EDifficultyLevel m) noexcept;

    /**
     * 連続値 [0,1] を 4 段表で線形補間する。
     *
     * @details 区間境界は 0.0 / 1/3 / 2/3 / 1.0、範囲外は両端値で頭打ち。
     * @param t 補間位置 [0,1]。
     * @param vals 4 段の値テーブル。
     * @return 補間結果。
     */
    static f32 SampleCurve(f32 t, const f32 vals[4]) noexcept;

    /** 現在のモード。 */
    EDifficultyLevel m_Mode               = EDifficultyLevel::Normal;

    /** 現在の連続値難易度 [0,1] (Normal start = 1/3)。 */
    f32             m_CurrentDifficulty = 0.333333f;

    /** プレイヤーの実プレイ統計。 */
    FPlayerSkillStats _stats {};

    /** Adaptive 用の累積セッション時間 (統計密度の分母)。 */
    f32 m_SessionTime = 0.0f;

    /** smooth lerp rate (1/s)。0.5 で約 1.4 秒で 50% 詰める指数追従。 */
    static constexpr f32 kAdaptiveLerpRate = 0.5f;

    /** average_completion_time の EMA 係数 (新値 30% / 旧値 70%)。 */
    static constexpr f32 kCompletionTimeEmaAlpha = 0.3f;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FDynamicDifficulty = CDynamicDifficulty;

} // namespace acs::game
