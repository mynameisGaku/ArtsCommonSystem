// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R — FCombatStateMachine (戦闘フェーズ + 脅威レベル)
//
// シーン全体の "combat phase" を 6 状態の有限オートマトンで追跡し、敵検出 /
// 戦闘開始 / 終了 / ボス出現 / ボス撃破 / リトリート / 勝利の主要遷移を一元化
// する。FMusicDirector / FAmbientDirector / FDamageFeedback と同列の上位ディレクタ
// で、FGame または FSceneServices が 1 個保持して毎フレーム Tick する想定。
//
// FMusicDirector との関係:
//   ・FMusicDirector は「BGM 状態 (Silent/Calm/Tension/Combat/Victory/GameOver)」
//     を保持する。FCombatStateMachine の状態は厳密に 1:1 ではなく、ゲームロジック
//     視点 (Peaceful / Alert / Engaged / BossFight / Victory / Retreat) で
//     抽象化されている。
//   ・上位 (FGame / FScene) が「ECombatState → EMusicState」マッピングを定義し、
//     OnStateChange callback の中で FMusicDirector::SetState を呼ぶ運用を想定。
//     本クラスは CAudioEngine 等の下位リソースを直接知らない。
//
// 機能:
//   ・ECombatState (6 種): Peaceful / Alert / Engaged / BossFight / Victory / Retreat
//   ・敵検出 / 戦闘開始 / 戦闘終了 / ボス遭遇 / ボス撃破 を notify API として提供
//   ・ThreatLevel() [0,1] — engaged 中は時間と共に上昇、平和になると低下する
//     連続値。BGM intensity や ambient color へ供給するのに使える。
//   ・FEnemyAwareness を TArray で管理し、複数敵 (= maybe sneak / multi engage) を
//     並列に追跡。EngagedEnemyCount() で「実際に交戦中の敵数」を返す。
//   ・OnStateChange callback で外部ディレクタ (Music / Ambient / UI) と疎結合連動。
//
// 設計選択 (Pillar R):
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
//     脅威感が増す」サブ表現を入れる。
//   ・**FEnemyAwareness は SoA でなく AoS**: 並列敵数は通常 1..8 程度で、
//     awareness_level の write が支配的なので AoS の方がキャッシュ的に有利。
//     線形探索でも O(N) は問題にならない。
//   ・**重複検出は no-op**: NotifyEnemyDetected を同 enemy_id で複数回呼んでも
//     awareness を 1.0 に再 clamp するだけ。NotifyCombatStarted も同様に
//     is_engaged の上書きで idempotent。
//   ・**Retreat への自動遷移は持たない**: Engaged → Retreat は明示的に
//     NotifyCombatEnded(victory=false) でのみ起きる。タイマーや距離ベースの
//     自動撤退判定は AI や FScene 側のロジックに委譲。
//   ・**callback は呼び出し中の Notify は受け付ける**: 再入安全 (state 更新を
//     先に行ってから callback を呼ぶ実装)。callback 内で別 Notify* を呼ぶと
//     state が連鎖更新される — これは仕様上許容 (デバウンスは caller 側で)。
//
// 範囲外 (本クラスでは持たない):
//   ・地点ごとの脅威マップ (= 2D heat map) は別モジュールで管理
//   ・敵 AI groupings (squad / formation)
//   ・boss 段階の階層 — caller が BossFight 内で intensity scalar を別途渡す想定
//     (本クラスは on/off の二値のみ)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

/**
 * 戦闘フェーズを表す 6 状態の列挙。
 *
 * @details
 * 値は安定なので save / replay に書ける (将来 enum 追加時は末尾追加のみ、既存値の
 * 意味は不変とする規約)。
 */
enum class ECombatState : u8 {
    /** 敵未検出 / 平穏 (探索 / 街)。 */
    Peaceful  = 0,

    /** 敵検出済 / 未交戦 (見張られている、ステルス可能)。 */
    Alert     = 1,

    /** 一般敵と交戦中。 */
    Engaged   = 2,

    /** ボス戦 (Engaged を割り込んで遷移、撃破後 Engaged へ復帰)。 */
    BossFight = 3,

    /** 戦闘勝利直後 (Peaceful へは caller が Reset で戻す)。 */
    Victory   = 4,

    /** 戦闘から撤退 / 敗北。 */
    Retreat   = 5,
};

/**
 * 1 敵分の認識情報 (FCombatStateMachine が内部 TArray で保持)。
 */
struct FEnemyAwareness {
    /** ゲーム側で割り振る一意 ID (FNodeId などをそのまま渡せる)。 */
    u32 enemy_id        = 0;

    /** 認識度 [0,1] (1.0=完全検出、0.0=未検出)。検出で 1.0、Retreat/Victory で 0.0 に減衰。 */
    f32 awareness_level = 0.0f;

    /** 交戦中フラグ (NotifyCombatStarted で true、NotifyCombatEnded で false)。 */
    bool is_engaged     = false;
};

/**
 * state 遷移時に発火する callback の関数ポインタ型。
 *
 * @details
 * std::function 不使用ポリシーに従い、void* user でコンテキストを引き回す。
 * from / to は実際に遷移した state で、同一値では呼ばれない (no-op 抑止)。
 * @param user SetOnStateChangeCallback で渡した任意のコンテキスト。
 * @param from 遷移前の state。
 * @param to 遷移後の state。
 */
using StateChangeCallback = void(*)(void* user, ECombatState from, ECombatState to) noexcept;

/**
 * シーン全体の戦闘フェーズと連続脅威レベルを追跡する有限オートマトン。
 *
 * @details
 * ECombatState の 6 状態を全 mutation を Notify 系および Reset 経由に閉じて遷移させ、敵検出 /
 * 戦闘開始 / 終了 / ボス出現 / 撃破などを一元化する。ThreatLevel() は state 既定値へ
 * 指数減衰で追従する連続値 [0,1] で、BGM intensity や ambient color に供給できる。複数敵
 * を FEnemyAwareness の TArray で並列追跡し、OnStateChange callback で外部ディレクタ
 * (Music / Ambient / UI) と疎結合に連動する (本クラスは下位リソースを直接知らない)。
 */
class FCombatStateMachine {
public:
    /** ECombatState の総数 (debug / table sizing 用に公開)。 */
    static constexpr u32 kStateCount = 6;

    /** 想定並列敵数 (= reserve hint、超過分は自動拡張)。 */
    static constexpr u32 kEnemyReserveHint = 16;

    /** Peaceful 状態で構築し、敵配列を reserve する。 */
    FCombatStateMachine() noexcept;

    /** 破棄する。 */
    ~FCombatStateMachine() noexcept = default;

    /** コピー禁止。 */
    FCombatStateMachine(const FCombatStateMachine&)            = delete;

    /** コピー代入も禁止。 */
    FCombatStateMachine& operator=(const FCombatStateMachine&) = delete;

    /** ムーブ禁止。 */
    FCombatStateMachine(FCombatStateMachine&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FCombatStateMachine& operator=(FCombatStateMachine&&)      = delete;

    /**
     * state を Peaceful に、awareness を全クリアして再初期化する (callback は保持)。
     *
     * @details コンストラクタ後の再初期化用 (FScene 再 enter 時など)。
     */
    void Init() noexcept;

    /**
     * callback も含めて完全な初期状態に戻す。
     *
     * @details Init を呼んだうえで callback / user も nullptr にクリアする。
     */
    void Reset() noexcept;

    /**
     * 敵検出を通知する (Peaceful なら Alert へ遷移)。
     *
     * @details
     * 既に Alert/Engaged/BossFight/Victory/Retreat の場合は state 据置で awareness のみ
     * 1.0 に更新する。既存 enemy_id の重複登録は awareness 上書きの no-op 相当。
     * @param enemy_id 検出した敵の一意 ID。
     */
    void NotifyEnemyDetected(u32 enemy_id) noexcept;

    /**
     * 交戦開始を通知する (Engaged へ遷移)。
     *
     * @details
     * Peaceful からも Alert を経由せず直接 Engaged になる。enemy_id の is_engaged を
     * true にする。既に BossFight 中は state 据置 + awareness 更新のみ (boss 優先)。
     * @param enemy_id 交戦を開始した敵の一意 ID。
     */
    void NotifyCombatStarted(u32 enemy_id) noexcept;

    /**
     * 1 敵分の交戦解消を通知する。
     *
     * @details
     * 残りの engaged 敵が 0 になったとき、victory=true なら Victory、false なら Retreat
     * に遷移する。残敵がいる場合は state 据置。BossFight 中は state を動かさない。
     * @param enemy_id 交戦を終えた敵の一意 ID。
     * @param victory 勝利扱いなら true、撤退扱いなら false。
     */
    void NotifyCombatEnded(u32 enemy_id, bool victory) noexcept;

    /**
     * ボス遭遇を通知する (どの state からでも BossFight へ割り込み遷移)。
     *
     * @details boss_id は FEnemyAwareness として追加 (既存なら is_engaged=true に上書き)。
     * @param boss_id 遭遇したボスの一意 ID。
     */
    void NotifyBossEncountered(u32 boss_id) noexcept;

    /**
     * ボス撃破を通知する (BossFight 時のみ有効)。
     *
     * @details
     * is_engaged な最初の敵を boss とみなして外し、残り engaged 敵がいれば Engaged へ
     * 復帰、いなければ Victory へ遷移する。BossFight 以外で呼ぶと警告 + no-op。
     */
    void NotifyBossDefeated() noexcept;

    /**
     * 現在の戦闘 state を返す。
     *
     * @return 現在の ECombatState。
     */
    ECombatState CurrentState() const noexcept { return _state; }

    /**
     * 平滑化された脅威レベルを返す。
     *
     * @details
     * 連続値 [0,1]。BGM intensity / ambient saturation / camera shake に流す想定で、
     * 内部の smooth interpolation 結果なので state 遷移直後でも急変しない。
     * @return 現在の脅威レベル [0,1]。
     */
    f32 ThreatLevel() const noexcept { return m_ThreatCurrent; }

    /**
     * 実際に交戦中の敵数を返す。
     *
     * @return is_engaged=true な FEnemyAwareness の数。
     */
    u32 EngagedEnemyCount() const noexcept;

    /**
     * 戦闘中かを単純判定する。
     *
     * @details UI / TSaveSlot 抑制 / fast-travel 禁止判定等に使う。
     * @return state が Engaged または BossFight なら true。
     */
    bool IsInCombat() const noexcept;

    /**
     * 毎フレーム呼んで脅威レベルを更新する driver。
     *
     * @details
     * ThreatLevel を m_ThreatTarget へ指数減衰で追従させる。Engaged 中は時間ドリフト
     * (最大 +0.3) を target に加算する。FSceneServices / FGame から毎フレーム呼ぶ。
     * @param dt このフレームの実経過秒 (負値は 0 にクランプ)。
     */
    void Tick(f32 dt) noexcept;

    /**
     * state 遷移 callback を登録する。
     *
     * @details cb==nullptr で登録解除。重複登録は不可 (上書き)。callback は state 遷移完了後に呼ばれる。
     * @param cb 遷移時に呼ぶ関数ポインタ。
     * @param user callback に引き回す任意のコンテキスト。
     */
    void SetOnStateChangeCallback(StateChangeCallback cb, void* user) noexcept;

private:
    /**
     * 内部 state を遷移させ、必要なら callback を発火する。
     *
     * @details
     * 同一 state への遷移は no-op (callback 不発火)。m_ThreatTarget を新 state 既定値に
     * 再設定し、Engaged ドリフトタイマをリセットしてから callback を呼ぶ。
     * @param next 遷移先の state。
     */
    void TransitionTo(ECombatState next) noexcept;

    /**
     * FEnemyAwareness を id で線形探索する。
     *
     * @param enemy_id 探す敵の一意 ID。
     * @return 見つかればインデックス、無ければ npos 相当 (= m_Enemies.Size())。
     */
    usize FindEnemy(u32 enemy_id) const noexcept;

    /**
     * state に紐づく既定の脅威 target を返す。
     *
     * @param state 対象の戦闘 state。
     * @return その state の既定脅威レベル [0,1]。
     */
    static f32 DefaultThreatTarget(ECombatState state) noexcept;

    /**
     * 値を [0,1] にクランプする。
     *
     * @param v クランプする値。
     * @return [0,1] に収めた値。
     */
    static f32 Clamp01(f32 v) noexcept;

    /** 現在の戦闘 state。 */
    ECombatState _state = ECombatState::Peaceful;

    /** 脅威レベルの目標値 (state 既定値 + Engaged 中のドリフト加算)。 */
    f32 m_ThreatTarget  = 0.0f;

    /** target へ指数減衰で追従する表示用の脅威レベル。 */
    f32 m_ThreatCurrent = 0.0f;

    /** Engaged 中の経過時間 (ドリフト加算用、state 遷移ごとにリセット)。 */
    f32 m_EngagedElapsed = 0.0f;

    /** 追跡中の敵認識情報 (AoS)。 */
    TArray<FEnemyAwareness> m_Enemies;

    /** BossFight に入る前の state (撃破後の復帰先決定に使う)。 */
    ECombatState m_PreBossState = ECombatState::Peaceful;

    /** state 遷移 callback (未登録なら nullptr)。 */
    StateChangeCallback m_Callback     = nullptr;

    /** callback に引き回すユーザコンテキスト。 */
    void*               m_CallbackUser = nullptr;
};

} // namespace acs::game
