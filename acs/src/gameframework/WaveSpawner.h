// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar L/R — WaveSpawner (敵 wave スポーン管理)
//
// 連続する複数の wave をキュー管理し、各 wave に紐づく複数の SpawnRule を
// 時間軸上で順次発火させる。残数管理 → wave clear → intermission → 次 wave →
// 全 wave 完了の state machine を内包する。
//
// 使い方:
//   class GameplayScene : public Scene {
//       acs::game::WaveSpawner _waves;
//
//       static void OnSpawn(void* self, const char* enemy_id, acs::Vec2 pos) noexcept {
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
//           _waves.Init();
//           _waves.SetOnSpawnCallback(&OnSpawn, this);
//           _waves.SetOnWaveStateChangeCallback(&OnWaveState, this);
//           _waves.AddWave(wave0_def);
//           _waves.AddWave(wave1_def);
//           _waves.StartWaves();
//       }
//       void OnUpdate(f32 dt) noexcept override { _waves.Tick(dt); }
//       void OnEnemyDefeated(const char* id) noexcept { _waves.NotifyEnemyKilled(id); }
//   };
//
// 設計選択:
//   ・**state machine は線形**: Idle → Spawning → WaitingClear → Cleared →
//     (intermission) → Spawning (次 wave) → … → AllComplete。逆遷移は Reset でのみ。
//   ・**SpawnRule は POD + ポインタ参照**: WaveDef の rules は caller 所有の
//     SpawnRule 配列を指す軽量ハンドル。caller は wave が終わるまで配列の
//     寿命を保証する責任を持つ (= literal / 静的定義を想定)。
//   ・**spawned_per_rule は wave ごとに別 Array**: 各 wave が任意数の rule を
//     持つので Array<Array<u32>> で 2 段ネスト。これは Move 可能で Array 同士の
//     コピー禁止に抵触しない (PushBack(Move(inner)) で挿入)。
//   ・**enemy_id 比較は strcmp**: const char* 一致判定は ModRegistry / DevConsole
//     と同じく `<cstring>` strcmp 0 で行う。リテラル前提なら同一ポインタも一致するが、
//     アセット読み込み由来の別実体にも対応する。
//   ・**初期遅延 + spawn interval**: 各 rule は initial_delay_sec 経過後に count 個を
//     spawn_interval_sec 間隔で発火。複数 rule は並列に評価する (= 同 wave 内で
//     異なる enemy 種が同時湧きできる)。
//   ・**alive_count の正値性保証**: NotifyEnemyKilled は登録 enemy_id がその wave に
//     存在しても alive_count を 0 未満にしない (= 過剰 kill 通知は no-op)。
//   ・**Pause / Resume は単純フラグ**: タイマー進行のみ止め、state は据置。Resume
//     後は中断時の進捗から再開する。
//   ・**callback は state 更新後**: CombatStateMachine と同じく内部状態を反映してから
//     呼ぶ (= callback 内で CurrentState() / EnemiesRemainingInWave() を問い合わせて
//     OK)。callback 内で StopWaves / Reset を呼んでも安全。
//
// 範囲外 (将来 Phase で):
//   ・SpawnRule の確率分布 / バースト形式 (rule あたり 1 種固定 + 等間隔のみ)
//   ・wave 同士の DAG (= 並列 wave / 条件分岐 wave) — 現状は線形シーケンス
//   ・敵 ID と spawn pos 以外の追加メタデータ (velocity / level / loot table 等)
//     は callback 側で enemy_id を key に lookup する想定
//   ・seed 化 (Random との連動) — 完全決定的、ランダムは caller 側で
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "math/Vec.h"

namespace acs::game {

// 全 wave 共通の進行 state。値は安定 (save / replay に書ける、enum 追加は末尾のみ)。
enum class EWaveState : u8 {
    Idle         = 0,  // StartWaves 前 / Reset 後 (= 何もしていない)
    Spawning     = 1,  // 現 wave の rule を実行中 (= まだ全部湧かせていない)
    WaitingClear = 2,  // 全 rule 発火済、残敵が 0 になるのを待っている
    Cleared      = 3,  // 現 wave clear、intermission タイマで次 wave へ
    AllComplete  = 4,  // 全 wave 完了 (queue 末尾 wave の Cleared を経て確定)
};

// 1 spawn rule。1 wave が任意数 (= rule_count) 持てる。
//   enemy_id            : 敵種別の識別子 (literal 推奨、strcmp で同種判定)
//   count               : この rule で湧かせる総数
//   spawn_interval_sec  : 2 個目以降の発火間隔 (sec)。0 以下なら同フレ即時連射
//                         扱い (= initial_delay 後にまとめて count 個発火)
//   initial_delay_sec   : wave 開始からこの rule の 1 個目発火までの遅延
//   spawn_position      : この rule の出現位置 (world coord)。出現位置を分散
//                         したい場合は同 enemy_id で別 rule を複数並べる
struct SpawnRule {
    const char* enemy_id           = nullptr;
    u32         count              = 0u;
    f32         spawn_interval_sec = 0.0f;
    f32         initial_delay_sec  = 0.0f;
    Vec2        spawn_position     = Vec2::Zero();
};

// 1 wave 定義。rules は caller 所有のスパン (寿命は caller 保証)。
//   wave_id               : デバッグ / 表示用識別子 (nullable)
//   rule_count            : rules 配列の要素数
//   rules                 : SpawnRule 配列 (rule_count 個)。rule_count==0 は
//                           「無敵 wave」: 即 Cleared に遷移する (intermission のみ)
//   wave_intermission_sec : Cleared → 次 Spawning までの待機 sec。最後の wave で
//                           は AllComplete までの猶予になる (= 演出時間)
struct WaveDef {
    const char*      wave_id               = nullptr;
    u32              rule_count            = 0u;
    const SpawnRule* rules                 = nullptr;
    f32              wave_intermission_sec = 0.0f;
};

// 敵 1 体出現時の callback。caller (= Scene 側) が実 entity を生成して NodeGraph
// に放り込む想定。enemy_id は SpawnRule の literal そのものをそのまま渡す。
using SpawnCallback = void(*)(void* user, const char* enemy_id, Vec2 spawn_pos) noexcept;

// wave state 遷移時の callback。wave_index は遷移時点の current wave index
// (= AllComplete への遷移時は最後の wave の index)。
using WaveStateChangeCallback = void(*)(void* user, u32 wave_index, EWaveState from, EWaveState to) noexcept;

class WaveSpawner {
public:
    // 想定 wave 数 (= reserve hint)。多めに見ても 16 で十分、それ超えは自動拡張。
    static constexpr u32 kWaveReserveHint = 16u;

    WaveSpawner() noexcept;
    ~WaveSpawner() noexcept = default;

    WaveSpawner(const WaveSpawner&)            = delete;
    WaveSpawner& operator=(const WaveSpawner&) = delete;
    WaveSpawner(WaveSpawner&&)                 = delete;
    WaveSpawner& operator=(WaveSpawner&&)      = delete;

    // ----- 初期化 / 破棄 -----
    // Init: state を Idle に、現 wave index / timer をリセット。AddWave で
    //   登録済みの wave queue は保持する (= scene 再 enter での再利用用)。
    //   callback も保持する。
    void Init() noexcept;

    // Reset: Init と同等だが追加で「state / 進行のみ」リセット。queue / callback
    //   は保持。Stop 後の完全リスタート用。AllComplete からも Idle に戻せる。
    void Reset() noexcept;

    // ClearAll: queue / state / callback すべてクリア (= Init 直後と同じ)。
    void ClearAll() noexcept;

    // ----- wave 構築 -----
    // wave をキュー末尾に追加。state==Idle 以外で呼んでも追加自体は通る
    // (= 進行中の wave 列に追記して continuous wave モードに使える)。
    void AddWave(const WaveDef& def) noexcept;

    // ----- 駆動 -----
    // 最初の wave からスポーン開始。Idle / Cleared / AllComplete から呼ぶ想定で、
    // Spawning / WaitingClear 中の呼び出しは no-op (誤用検出)。
    // wave queue が空のときは AllComplete に直行する (= 何もしないで終了通知)。
    void StartWaves() noexcept;

    // 一時停止。state は据置で、Tick による timer 進行のみ止める。
    // 既に停止 / Idle / AllComplete でも安全に呼べる (idempotent)。
    void StopWaves() noexcept;

    // StopWaves で停止していた進行を再開。停止していなければ no-op。
    void ResumeWaves() noexcept;

    // 敵 1 体撃破通知。alive_count を 1 減らし、0 + 全 rule 発火済なら
    // WaitingClear → Cleared 遷移を起こす。enemy_id は SpawnRule の literal
    // と strcmp で比較する想定だが、本実装では識別子のマッチングは行わず
    // 単純に「現 wave の生存者数を 1 減らす」だけにする (= 同 wave 内で
    // どの種を倒したかは caller / 上位ロジックの責務)。enemy_id は callback /
    // ログ / 集計の hint として保持される。
    void NotifyEnemyKilled(const char* enemy_id) noexcept;

    // ----- 問い合わせ -----
    EWaveState CurrentState() const noexcept { return _state; }

    // 現在処理中の wave index。Idle では 0、AllComplete では TotalWaves() を返す
    // (= 範囲外の sentinel として使える、TotalWaves() 自身は有効 wave index 数)。
    u32 CurrentWaveIndex() const noexcept { return _current_wave; }

    // AddWave で積まれた総 wave 数。
    u32 TotalWaves() const noexcept;

    // 現 wave で生きている (= 湧かせた - 倒した) 敵の数。Idle/AllComplete では 0。
    u32 EnemiesRemainingInWave() const noexcept { return _alive_count; }

    // 現 wave で既に湧かせた敵の総数 (全 rule 合計)。Idle/AllComplete では 0。
    u32 EnemiesSpawnedInWave() const noexcept;

    // 現 wave 開始からの経過 sec。Spawning に入った瞬間に 0 にリセットされ、
    // WaitingClear / Cleared の間も加算され続ける (= 次 wave Spawning でリセット)。
    // Idle / AllComplete では 0。Pause 中は加算停止。
    f32 WaveTimerSec() const noexcept { return _wave_timer; }

    // ----- callback -----
    // cb == nullptr で登録解除。重複登録は上書き。
    void SetOnSpawnCallback(SpawnCallback cb, void* user) noexcept;
    void SetOnWaveStateChangeCallback(WaveStateChangeCallback cb, void* user) noexcept;

    // ----- driver -----
    // dt はリアル秒。dt < 0 は 0 にクランプ。Pause 中は何もしない。
    // 1 Tick 内で複数 spawn / 複数 state 遷移が連鎖し得る (大 dt 対策)。
    void Tick(f32 dt) noexcept;

private:
    // wave + 内部進捗。spawned_per_rule[i] は rule i の発火済個数。
    struct WaveEntry {
        WaveDef     def;
        Array<u32>  spawned_per_rule;  // size == def.rule_count
    };

    // 内部 state 遷移。same-state は no-op (callback 不発火)。
    void TransitionTo(EWaveState next) noexcept;

    // 現 wave の全 rule に対して 1 Tick 分の進行を行い、満たしている発火を実行。
    // 全 rule の count を満たしたら WaitingClear に自動遷移する。
    void TickSpawning(f32 dt) noexcept;

    // Cleared から次 wave へ。最後の wave なら AllComplete。
    void AdvanceToNextWave() noexcept;

    // ----- state -----
    EWaveState _state = EWaveState::Idle;

    // 現 wave index (0-based)。Idle では 0、AllComplete では TotalWaves()。
    u32 _current_wave = 0u;

    // Spawning に入ってから経過した sec。rule の delay / interval 判定の基準。
    f32 _wave_timer = 0.0f;

    // Cleared 中の intermission 経過 sec。intermission 完了で次 wave へ。
    f32 _intermission_timer = 0.0f;

    // この wave 内で生きている敵数 (= spawned 合計 - killed)。
    u32 _alive_count = 0u;

    // Pause フラグ。Tick の冒頭で見て、true なら timer 進行をスキップ。
    bool _paused = false;

    // ----- queue -----
    Array<WaveEntry> _waves;

    // ----- callback -----
    SpawnCallback           _spawn_cb           = nullptr;
    void*                   _spawn_cb_user      = nullptr;
    WaveStateChangeCallback _state_cb           = nullptr;
    void*                   _state_cb_user      = nullptr;
};

} // namespace acs::game
