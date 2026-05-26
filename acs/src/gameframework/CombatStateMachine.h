// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R — CombatStateMachine (戦闘フェーズ + 脅威レベル)
//
// シーン全体の "combat phase" を 6 状態の有限オートマトンで追跡し、敵検出 /
// 戦闘開始 / 終了 / ボス出現 / ボス撃破 / リトリート / 勝利の主要遷移を一元化
// する。MusicDirector / AmbientDirector / DamageFeedback と同列の上位ディレクタ
// で、Game または SceneServices が 1 個保持して毎フレーム Tick する想定。
//
// MusicDirector との関係:
//   ・MusicDirector は「BGM 状態 (Silent/Calm/Tension/Combat/Victory/GameOver)」
//     を保持する。CombatStateMachine の状態は厳密に 1:1 ではなく、ゲームロジック
//     視点 (Peaceful / Alert / Engaged / BossFight / Victory / Retreat) で
//     抽象化されている。
//   ・上位 (Game / Scene) が「ECombatState → EMusicState」マッピングを定義し、
//     OnStateChange callback の中で MusicDirector::SetState を呼ぶ運用を想定。
//     本クラスは AudioEngine 等の下位リソースを直接知らない。
//
// 機能:
//   ・ECombatState (6 種): Peaceful / Alert / Engaged / BossFight / Victory / Retreat
//   ・敵検出 / 戦闘開始 / 戦闘終了 / ボス遭遇 / ボス撃破 を notify API として提供
//   ・ThreatLevel() [0,1] — engaged 中は時間と共に上昇、平和になると低下する
//     連続値。BGM intensity や ambient color へ供給するのに使える。
//   ・EnemyAwareness を TArray で管理し、複数敵 (= maybe sneak / multi engage) を
//     並列に追跡。EngagedEnemyCount() で「実際に交戦中の敵数」を返す。
//   ・OnStateChange callback で外部ディレクタ (Music / Ambient / UI) と疎結合連動。
//
// 設計選択 (Pillar R Phase 3):
//   ・**state 遷移はメソッド経由のみ**: caller が直接 _state を書き換えられないよう
//     全 mutation を Notify*/Reset() に閉じる。これにより遷移時の callback が
//     必ず発火し、ThreatLevel の起点 / 終点もここに集約される。
//   ・**BossFight は割り込み state**: どの state からでも遷移可能で、Engaged で
//     bossEncountered すると即 BossFight にスナップする。NotifyBossDefeated()
//     後は Engaged にフォールバックさせる (= 一般敵が残っていれば継続戦闘)。
//   ・**ThreatLevel は smooth interpolation**: 内部で目標値を持ち、Tick で
//     現在値→目標値へ指数減衰で追従させる (鋭い切替は BGM/演出が破綻するため)。
//     在状態ごとに target_threat が固定 (Peaceful=0 / Alert=0.3 / Engaged=0.7
//     / BossFight=1.0 / Victory=Engaged の値を保持 / Retreat=0.2 へ徐減衰)。
//     さらに Engaged 中は時間で +0..0.3 のドリフトを加算し「長引く戦闘ほど
//     脅威感が増す」サブ表現を入れる (UE5 系の流れに合わせた音響設計と整合)。
//   ・**EnemyAwareness は SoA でなく AoS**: 並列敵数は通常 1..8 程度で、
//     awareness_level の write が支配的なので AoS の方がキャッシュ的に有利。
//     線形探索でも O(N) は問題にならない。
//   ・**重複検出は no-op**: NotifyEnemyDetected を同 enemy_id で複数回呼んでも
//     awareness を 1.0 に再 clamp するだけ。NotifyCombatStarted も同様に
//     is_engaged の上書きで idempotent。
//   ・**Retreat への自動遷移は持たない**: Engaged → Retreat は明示的に
//     NotifyCombatEnded(victory=false) でのみ起きる。タイマーや距離ベースの
//     自動撤退判定は Pillar L (AI) や Scene 側のロジックに委譲。
//   ・**callback は呼び出し中の Notify は受け付ける**: 再入安全 (state 更新を
//     先に行ってから callback を呼ぶ実装)。callback 内で別 Notify* を呼ぶと
//     state が連鎖更新される — これは仕様上許容 (デバウンスは caller 側で)。
//
// 範囲外 (Phase 4+ で):
//   ・地点ごとの脅威マップ (= 2D heat map) は別モジュールで管理
//   ・敵 AI groupings (squad / formation) — Pillar L で扱う
//   ・boss 段階 (phase 1 / phase 2 / phase 3) の階層 — caller が BossFight 内で
//     intensity scalar を別途渡す想定 (本クラスは on/off の二値のみ)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

// 戦闘フェーズ。値は安定なので save / replay に書ける (将来 enum 追加時は末尾
// 追加のみ、既存値の意味は不変とする規約)。
enum class ECombatState : u8 {
    Peaceful  = 0,  // 敵未検出 / 平穏 (探索 / 街)
    Alert     = 1,  // 敵検出済 / 未交戦 (見張られている、ステルス可能)
    Engaged   = 2,  // 一般敵と交戦中
    BossFight = 3,  // ボス戦 (Engaged を割り込んで遷移、撃破後 Engaged へ復帰)
    Victory   = 4,  // 戦闘勝利直後 (Peaceful へは caller が Reset で戻す)
    Retreat   = 5,  // 戦闘から撤退 / 敗北
};

// 1 敵分の認識情報。CombatStateMachine が内部 TArray で保持する。
//   enemy_id        : ゲーム側で割り振る一意 ID (FNodeId などをそのまま渡せる)
//   awareness_level : [0, 1]。1.0 = 完全に検出、0.0 = 未検出。Notify*EnemyDetected
//                     で 1.0 に上書き、Retreat / Victory で 0.0 に減衰させる。
//   is_engaged      : NotifyCombatStarted で true、NotifyCombatEnded で false。
struct EnemyAwareness {
    u32 enemy_id        = 0;
    f32 awareness_level = 0.0f;
    bool is_engaged     = false;
};

// state 遷移時に発火する関数ポインタ。std::function 不使用ポリシーに従い、
// void* user を介してコンテキストを引き回す。
//   from / to は実際に遷移した state (同一値で呼ばれることはない、no-op 抑止)。
using StateChangeCallback = void(*)(void* user, ECombatState from, ECombatState to) noexcept;

class CombatStateMachine {
public:
    // ECombatState 総数 (debug / table sizing 用に公開)。
    static constexpr u32 kStateCount = 6;
    // 想定並列敵数 (= reserve hint)。多めに見ても 16 で十分、それ超えは自動拡張。
    static constexpr u32 kEnemyReserveHint = 16;

    CombatStateMachine() noexcept;
    ~CombatStateMachine() noexcept = default;

    CombatStateMachine(const CombatStateMachine&)            = delete;
    CombatStateMachine& operator=(const CombatStateMachine&) = delete;
    CombatStateMachine(CombatStateMachine&&)                 = delete;
    CombatStateMachine& operator=(CombatStateMachine&&)      = delete;

    // ----- 初期化 / リセット -----
    // Init: state を Peaceful に、awareness を全クリア。コンストラクタ後の
    //   再初期化用 (Scene 再 enter 時など)。callback は保持する。
    void Init() noexcept;

    // Reset: Init と同等だが追加で「callback も含めて再初期化したい」場合用。
    //   完全初期状態に戻す (state=Peaceful, threat=0, callback=nullptr)。
    void Reset() noexcept;

    // ----- 戦闘通知 (state 遷移トリガ) -----
    // Peaceful → Alert に遷移 (既に Alert/Engaged/BossFight の場合は state 据置で
    // awareness のみ 1.0 に更新)。既存 enemy_id の重複登録は awareness 上書き。
    void NotifyEnemyDetected(u32 enemy_id) noexcept;

    // Alert → Engaged に遷移 (Peaceful からの直接交戦も可: Alert を経由せずに
    // 直接 Engaged に遷移する)。enemy_id の is_engaged を true に。
    // 既に BossFight 中は state 据置 + awareness 更新のみ (boss 優先)。
    void NotifyCombatStarted(u32 enemy_id) noexcept;

    // Engaged の 1 敵分を解消。victory=true なら勝利扱い (残敵 0 → Victory)、
    // false なら撤退扱い (残敵 0 → Retreat)。残敵がいる場合は state 据置。
    void NotifyCombatEnded(u32 enemy_id, bool victory) noexcept;

    // どの state からでも BossFight に遷移 (= 割り込み)。boss_id は EnemyAwareness
    // として追加 (既存なら is_engaged=true に上書き)。
    void NotifyBossEncountered(u32 boss_id) noexcept;

    // BossFight 時のみ有効。boss を engaged 一覧から取り除き、残り engaged 敵が
    // いれば Engaged へ復帰、いなければ Victory へ遷移する。
    void NotifyBossDefeated() noexcept;

    // ----- 問い合わせ -----
    ECombatState CurrentState() const noexcept { return _state; }

    // 連続値 [0, 1]。BGM intensity / ambient saturation / camera shake に流す想定。
    // 内部の smooth interpolation 結果なので、state 遷移直後でも急変しない。
    f32 ThreatLevel() const noexcept { return _threat_current; }

    // is_engaged=true な EnemyAwareness の数 (= 実際に交戦中の敵数)。
    u32 EngagedEnemyCount() const noexcept;

    // 単純判定: Engaged or BossFight。UI / SaveSlot 抑制 / fast-travel 禁止判定等。
    bool IsInCombat() const noexcept;

    // ----- driver -----
    // SceneServices / Game から毎フレーム呼ぶ。dt はリアル秒。
    // 内部で ThreatLevel を _threat_target に向けて指数減衰で追従させる。
    // また Engaged 中は時間ドリフト (最大 +0.3) を _threat_target に加算する。
    void Tick(f32 dt) noexcept;

    // ----- callback -----
    // cb == nullptr で登録解除。重複登録は不可 (上書き)。
    // callback は state 遷移完了後 (内部状態更新後) に呼ばれる。
    void SetOnStateChangeCallback(StateChangeCallback cb, void* user) noexcept;

private:
    // 内部 state 遷移。同一 state への遷移は no-op (callback 不発火)。
    // 必要に応じて _threat_target を新 state 既定値に再設定する。
    void TransitionTo(ECombatState next) noexcept;

    // EnemyAwareness を id で線形探索。見つからなければ npos 相当 (= _enemies.Size())。
    usize FindEnemy(u32 enemy_id) const noexcept;

    // _state に紐づく既定 threat target を返す ([0, 1])。
    static f32 DefaultThreatTarget(ECombatState state) noexcept;

    static f32 Clamp01(f32 v) noexcept;

    // ----- state -----
    ECombatState _state = ECombatState::Peaceful;

    // ----- threat level smoothing -----
    // _threat_target: state によって決まる目標値 + Engaged 中のドリフト加算。
    // _threat_current: target に向けて指数減衰で追従する表示用の値。
    f32 _threat_target  = 0.0f;
    f32 _threat_current = 0.0f;

    // Engaged 中の経過時間 (ドリフト加算用)。state 遷移ごとにリセット。
    f32 _engaged_elapsed = 0.0f;

    // ----- enemy 追跡 -----
    TArray<EnemyAwareness> _enemies;

    // BossFight に入る前の state を覚えておき、NotifyBossDefeated 後に
    // engaged 敵が残っていれば Engaged、いなければ Victory に戻す。
    ECombatState _pre_boss_state = ECombatState::Peaceful;

    // ----- callback -----
    StateChangeCallback _callback     = nullptr;
    void*               _callback_user = nullptr;
};

} // namespace acs::game
