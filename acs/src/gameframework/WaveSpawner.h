// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar L/R — FWaveSpawner (敵 wave スポーン管理)
//
// 連続する複数の wave をキュー管理し、各 wave に紐づく複数の SpawnRule を
// 時間軸上で順次発火させる。残数管理 → wave clear → intermission → 次 wave →
// 全 wave 完了の state machine を内包する。
//
// 使い方:
//   class GameplayScene : public Scene {
//       acs::game::FWaveSpawner m_Waves;
//
//       static void OnSpawn(void* self, const char* enemy_id, acs::FVec2 pos) noexcept {
//           auto* s = static_cast<GameplayScene*>(self);
//           s->SpawnEnemyAt(enemy_id, pos);
//       }
//       static void OnWaveState(void* self, u32 idx,
//                               acs::game::EWaveState from,
//                               acs::game::EWaveState to) noexcept {
//           // BGM / UI / カメラ演出をここで連動
//       }
//
//       void OnEnter() noexcept override {
//           m_Waves.Init();
//           m_Waves.SetOnSpawnCallback(&OnSpawn, this);
//           m_Waves.SetOnWaveStateChangeCallback(&OnWaveState, this);
//           m_Waves.AddWave(wave0_def);
//           m_Waves.AddWave(wave1_def);
//           m_Waves.StartWaves();
//       }
//       void OnUpdate(f32 dt) noexcept override { m_Waves.Tick(dt); }
//       void OnEnemyDefeated(const char* id) noexcept { m_Waves.NotifyEnemyKilled(id); }
//   };
//
// 設計選択:
//   ・state machine は線形: Idle → Spawning → WaitingClear → Cleared →
//     (intermission) → Spawning (次 wave) → … → AllComplete。逆遷移は Reset でのみ。
//   ・SpawnRule は POD + ポインタ参照: FWaveDef の rules は caller 所有の
//     SpawnRule 配列を指す軽量ハンドル。caller は wave が終わるまで配列の
//     寿命を保証する責任を持つ (= literal / 静的定義を想定)。
//   ・spawned_per_rule は wave ごとに別 TArray: 各 wave が任意数の rule を
//     持つので TArray<TArray<u32>> で 2 段ネスト。これは Move 可能で TArray 同士の
//     コピー禁止に抵触しない (PushBack(Move(inner)) で挿入)。
//   ・初期遅延 + spawn interval: 各 rule は initial_delay_sec 経過後に count 個を
//     spawn_interval_sec 間隔で発火。複数 rule は並列に評価する (= 同 wave 内で
//     異なる enemy 種が同時湧きできる)。
//   ・alive_count の正値性保証: NotifyEnemyKilled は alive_count を 0 未満にしない
//     (= 過剰 kill 通知は no-op)。
//   ・Pause / Resume は単純フラグ: タイマー進行のみ止め、state は据置。Resume
//     後は中断時の進捗から再開する。
//   ・callback は state 更新後: 内部状態を反映してから呼ぶ (= callback 内で
//     CurrentState() / EnemiesRemainingInWave() を問い合わせて OK)。callback 内で
//     StopWaves / Reset を呼んでも安全。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "math/Vec.h"

namespace acs::game {

/**
 * 全 wave 共通の進行 state。
 *
 * @details
 * 値は安定 (save / replay に書ける、enum 追加は末尾のみ)。線形に
 * Idle → Spawning → WaitingClear → Cleared → AllComplete と進む。
 */
enum class EWaveState : u8 {
    /** StartWaves 前 / Reset 後 (= 何もしていない)。 */
    Idle         = 0,

    /** 現 wave の rule を実行中 (= まだ全部湧かせていない)。 */
    Spawning     = 1,

    /** 全 rule 発火済、残敵が 0 になるのを待っている。 */
    WaitingClear = 2,

    /** 現 wave clear、intermission タイマで次 wave へ。 */
    Cleared      = 3,

    /** 全 wave 完了 (queue 末尾 wave の Cleared を経て確定)。 */
    AllComplete  = 4,
};

/**
 * 1 つの spawn rule。
 *
 * @details 1 wave が任意数 (= rule_count) 持てる。
 */
struct SpawnRule {
    /** 敵種別の識別子 (literal 推奨、callback / ログの hint に使う)。 */
    const char* enemy_id           = nullptr;

    /** この rule で湧かせる総数。 */
    u32         count              = 0u;

    /** 2 個目以降の発火間隔 (sec)。0 以下なら initial_delay 後にまとめて count 個発火。 */
    f32         spawn_interval_sec = 0.0f;

    /** wave 開始からこの rule の 1 個目発火までの遅延 (sec)。 */
    f32         initial_delay_sec  = 0.0f;

    /** この rule の出現位置 (world coord)。分散させたい場合は同 enemy_id の別 rule を並べる。 */
    FVec2        spawn_position     = FVec2::Zero();
};

/**
 * 1 つの wave 定義。
 *
 * @details rules は caller 所有のスパン (寿命は caller 保証)。
 */
struct FWaveDef {
    /** デバッグ / 表示用識別子 (nullable)。 */
    const char*      wave_id               = nullptr;

    /** rules 配列の要素数。 */
    u32              rule_count            = 0u;

    /** SpawnRule 配列 (rule_count 個)。rule_count==0 は即 Cleared に遷移する「無敵 wave」。 */
    const SpawnRule* rules                 = nullptr;

    /** Cleared → 次 Spawning までの待機 sec。最後の wave では AllComplete までの猶予になる。 */
    f32              wave_intermission_sec = 0.0f;
};

/**
 * 敵 1 体出現時の callback 型。
 *
 * @details
 * caller (= Scene 側) が実 entity を生成して NodeGraph に放り込む想定。
 * enemy_id は SpawnRule の literal そのものをそのまま渡す。
 * @param user SetOnSpawnCallback で登録した user ポインタ。
 * @param enemy_id 出現させる敵の識別子。
 * @param spawn_pos 出現位置 (world coord)。
 */
using SpawnCallback = void(*)(void* user, const char* enemy_id, FVec2 spawn_pos) noexcept;

/**
 * wave state 遷移時の callback 型。
 *
 * @details wave_index は遷移時点の current wave index (AllComplete への遷移時は最後の wave の index)。
 * @param user SetOnWaveStateChangeCallback で登録した user ポインタ。
 * @param wave_index 遷移時点の current wave index。
 * @param from 遷移前の state。
 * @param to 遷移後の state。
 */
using WaveStateChangeCallback = void(*)(void* user, u32 wave_index, EWaveState from, EWaveState to) noexcept;

/**
 * 複数 wave をキュー管理し時間軸上で敵をスポーンさせる state machine。
 *
 * @details
 * 各 wave に紐づく複数の SpawnRule を並列評価し、残数管理 → wave clear →
 * intermission → 次 wave → 全 wave 完了を線形に進める。non-copy / non-move で
 * Scene 等に値メンバとして持たせる想定。Tick(dt) で駆動し、敵撃破は
 * NotifyEnemyKilled で通知する。
 */
class FWaveSpawner {
public:
    /** 想定 wave 数 (= reserve hint)。多めに見ても 16 で十分、それ超えは自動拡張。 */
    static constexpr u32 kWaveReserveHint = 16u;

    /** 空状態で構築する (wave queue は kWaveReserveHint で予約)。 */
    FWaveSpawner() noexcept;

    /** 破棄する (TArray が内部リソースを解放)。 */
    ~FWaveSpawner() noexcept = default;

    /** コピー禁止 (進行状態を単独所有するため)。 */
    FWaveSpawner(const FWaveSpawner&)            = delete;

    /** コピー代入も禁止。 */
    FWaveSpawner& operator=(const FWaveSpawner&) = delete;

    /** ムーブ禁止 (callback の user ポインタ等の参照安定性を保つため)。 */
    FWaveSpawner(FWaveSpawner&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FWaveSpawner& operator=(FWaveSpawner&&)      = delete;

    /**
     * state を Idle に戻し、現 wave index / timer / 各 wave の発火カウンタをリセットする。
     *
     * @details
     * AddWave で登録済みの wave queue と callback は保持する (= scene 再 enter での
     * 再利用用)。
     */
    void Init() noexcept;

    /**
     * Init と同等のリセットを行う (state / 進行のみ初期化)。
     *
     * @details queue / callback は保持。Stop 後の完全リスタート用で、AllComplete からも Idle に戻せる。
     */
    void Reset() noexcept;

    /** queue / state / callback をすべてクリアする (= Init 直後と同じ完全初期状態)。 */
    void ClearAll() noexcept;

    /**
     * wave をキュー末尾に追加する。
     *
     * @details
     * state==Idle 以外で呼んでも追加自体は通る (= 進行中の wave 列に追記して
     * continuous wave モードに使える)。spawned_per_rule は rule_count 個の 0 で初期化される。
     * @param def 追加する wave 定義 (rules は caller 所有で寿命を保証すること)。
     */
    void AddWave(const FWaveDef& def) noexcept;

    /**
     * 最初の wave からスポーンを開始する。
     *
     * @details
     * Idle / Cleared / AllComplete から呼ぶ想定で、Spawning / WaitingClear 中の
     * 呼び出しは警告 + no-op (誤用検出)。wave queue が空のときは AllComplete に
     * 直行する (= 何もしないで終了通知)。
     */
    void StartWaves() noexcept;

    /**
     * 進行を一時停止する。
     *
     * @details state は据置で、Tick による timer 進行のみ止める。既に停止 / Idle /
     * AllComplete でも安全に呼べる (idempotent)。
     */
    void StopWaves() noexcept;

    /** StopWaves で停止していた進行を再開する (停止していなければ実質 no-op)。 */
    void ResumeWaves() noexcept;

    /**
     * 敵 1 体撃破を通知する。
     *
     * @details
     * alive_count を 1 減らし、WaitingClear かつ 0 になった瞬間に Cleared 遷移を起こす。
     * 識別子のマッチングは行わず単純に現 wave の生存者数を 1 減らすだけ (= どの種を
     * 倒したかは caller / 上位ロジックの責務)。Spawning/WaitingClear 以外、または
     * alive_count==0 のときは no-op (過剰 kill 防御)。
     * @param enemy_id 撃破した敵の識別子 (本実装ではマッチングに使わず、callback / ログ用の hint)。
     */
    void NotifyEnemyKilled(const char* enemy_id) noexcept;

    /**
     * 現在の進行 state を返す。
     *
     * @return 現在の EWaveState。
     */
    EWaveState CurrentState() const noexcept { return _state; }

    /**
     * 現在処理中の wave index を返す。
     *
     * @details Idle では 0、AllComplete では最後に処理した wave の index を返す。
     * @return 現 wave の 0-based インデックス。
     */
    u32 CurrentWaveIndex() const noexcept { return m_CurrentWave; }

    /**
     * AddWave で積まれた総 wave 数を返す。
     *
     * @return 登録済み wave の数。
     */
    u32 TotalWaves() const noexcept;

    /**
     * 現 wave で生きている (= 湧かせた - 倒した) 敵の数を返す。
     *
     * @return 現 wave の生存敵数 (Idle/AllComplete では 0)。
     */
    u32 EnemiesRemainingInWave() const noexcept { return m_AliveCount; }

    /**
     * 現 wave で既に湧かせた敵の総数を返す (全 rule 合計)。
     *
     * @return 現 wave の累計 spawn 数 (Idle/AllComplete では 0)。
     */
    u32 EnemiesSpawnedInWave() const noexcept;

    /**
     * 現 wave 開始からの経過 sec を返す。
     *
     * @details
     * Spawning に入った瞬間に 0 にリセットされ、WaitingClear / Cleared の間も加算され
     * 続ける (= 次 wave Spawning でリセット)。Idle / AllComplete では 0、Pause 中は加算停止。
     * @return 現 wave 開始からの経過秒。
     */
    f32 WaveTimerSec() const noexcept { return m_WaveTimer; }

    /**
     * 敵出現 callback を設定する。
     *
     * @details cb == nullptr で登録解除。重複登録は上書き。
     * @param cb 敵 1 体出現時に呼ぶ callback (nullptr で解除)。
     * @param user callback に渡す user ポインタ。
     */
    void SetOnSpawnCallback(SpawnCallback cb, void* user) noexcept;

    /**
     * wave state 遷移 callback を設定する。
     *
     * @details cb == nullptr で登録解除。重複登録は上書き。
     * @param cb state 遷移時に呼ぶ callback (nullptr で解除)。
     * @param user callback に渡す user ポインタ。
     */
    void SetOnWaveStateChangeCallback(WaveStateChangeCallback cb, void* user) noexcept;

    /**
     * 1 フレーム分だけ進行を進める。
     *
     * @details
     * dt < 0 は 0 にクランプ。Pause 中は何もしない。1 Tick 内で複数 spawn / 複数
     * state 遷移が連鎖し得る (大 dt 対策)。
     * @param dt 経過秒 (リアル秒)。
     */
    void Tick(f32 dt) noexcept;

private:
    /**
     * wave 定義 + 内部進捗を束ねるエントリ。
     *
     * @details spawned_per_rule[i] は rule i の発火済個数 (size == def.rule_count)。
     */
    struct FWaveEntry {
        /** この wave の定義 (rules は caller 所有)。 */
        FWaveDef     def;

        /** rule ごとの発火済個数 (size == def.rule_count)。 */
        TArray<u32>  spawned_per_rule;
    };

    /**
     * 内部 state を遷移し、必要なら state callback を発火する。
     *
     * @details same-state は no-op (callback 不発火)。Spawning で wave_timer を、
     * Cleared で intermission_timer を 0 にする。
     * @param next 遷移先の state。
     */
    void TransitionTo(EWaveState next) noexcept;

    /**
     * 現 wave の全 rule に対して 1 Tick 分の spawn 進行を行う。
     *
     * @details 発火条件を満たすたびに spawn callback を呼び、全 rule の count を
     * 満たしたら WaitingClear に自動遷移する。
     * @param dt 経過秒 (クランプ済み)。
     */
    void TickSpawning(f32 dt) noexcept;

    /** Cleared から次 wave の Spawning へ進める (最後の wave なら AllComplete)。 */
    void AdvanceToNextWave() noexcept;

    /** 現在の進行 state。 */
    EWaveState _state = EWaveState::Idle;

    /** 現 wave index (0-based)。Idle では 0、AllComplete では最後に処理した wave の index。 */
    u32 m_CurrentWave = 0u;

    /** Spawning に入ってから経過した sec (rule の delay / interval 判定の基準)。 */
    f32 m_WaveTimer = 0.0f;

    /** Cleared 中の intermission 経過 sec (完了で次 wave へ)。 */
    f32 m_IntermissionTimer = 0.0f;

    /** この wave 内で生きている敵数 (= spawned 合計 - killed)。 */
    u32 m_AliveCount = 0u;

    /** Pause フラグ (Tick 冒頭で見て、true なら timer 進行をスキップ)。 */
    bool m_Paused = false;

    /** wave queue (定義 + 進捗)。 */
    TArray<FWaveEntry> m_Waves;

    /** 敵出現 callback (未設定なら nullptr)。 */
    SpawnCallback           m_SpawnCb           = nullptr;

    /** 敵出現 callback に渡す user ポインタ。 */
    void*                   m_SpawnCbUser      = nullptr;

    /** wave state 遷移 callback (未設定なら nullptr)。 */
    WaveStateChangeCallback _state_cb           = nullptr;

    /** wave state 遷移 callback に渡す user ポインタ。 */
    void*                   _state_cb_user      = nullptr;
};

} // namespace acs::game
