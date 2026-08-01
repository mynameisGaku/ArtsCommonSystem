// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R — CCombatStateMachine 実装
//
// state machine + threat level smoothing + enemy awareness tracking を完全実装。
// 外部ディレクタ (Music / Ambient / UI) との結線は OnStateChange callback で
// caller が定義する想定。本クラスは CAudioEngine 等の下位リソースを直接知らない。
#include "gameframework/CombatStateMachine.h"
#include "foundation/Log.h"

namespace acs::game {

/** 値を [0,1] にクランプする。 */
f32 CCombatStateMachine::Clamp01(f32 v) noexcept {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/**
 * 各 state の既定脅威レベルを返す (Tick 内で m_ThreatTarget に流し込む)。
 *
 * @details
 * 値は BGM intensity / camera shake の自然な強度に合わせた経験値:
 * Peaceful=0.00 (平穏) / Alert=0.30 (緊張) / Engaged=0.70 (戦闘) /
 * BossFight=1.00 (ボス戦) / Victory=0.40 (勝利直後) / Retreat=0.20 (撤退)。
 */
f32 CCombatStateMachine::DefaultThreatTarget(ECombatState state) noexcept {
    switch (state) {
        case ECombatState::Peaceful:  return 0.0f;
        case ECombatState::Alert:     return 0.3f;
        case ECombatState::Engaged:   return 0.7f;
        case ECombatState::BossFight: return 1.0f;
        case ECombatState::Victory:   return 0.4f;
        case ECombatState::Retreat:   return 0.2f;
    }
    return 0.0f;  // unreachable, defensive default
}

/** Peaceful 状態で構築し、敵配列を reserve する。 */
CCombatStateMachine::CCombatStateMachine() noexcept {
    m_Enemies.Reserve(kEnemyReserveHint);
}

/** state を Peaceful に、awareness を全クリアして再初期化する (callback は保持)。 */
void CCombatStateMachine::Init() noexcept {
    _state             = ECombatState::Peaceful;
    m_ThreatTarget     = DefaultThreatTarget(ECombatState::Peaceful);
    m_ThreatCurrent    = 0.0f;
    m_EngagedElapsed   = 0.0f;
    m_PreBossState    = ECombatState::Peaceful;
    m_Enemies.Clear();
    // m_Callback / m_CallbackUser は保持 (Init は scene 再 enter 用と位置付け)。
}

/** callback も含めて完全な初期状態に戻す。 */
void CCombatStateMachine::Reset() noexcept {
    Init();
    m_Callback      = nullptr;
    m_CallbackUser = nullptr;
}

/** EnemyAwareness を id で線形探索する (無ければ m_Enemies.Size() を返す)。 */
usize CCombatStateMachine::FindEnemy(u32 enemy_id) const noexcept {
    const usize n = m_Enemies.Size();
    for (usize i = 0; i < n; ++i) {
        if (m_Enemies[i].enemy_id == enemy_id) return i;
    }
    return n;
}

/** is_engaged=true な敵の数を返す。 */
u32 CCombatStateMachine::EngagedEnemyCount() const noexcept {
    u32 count = 0;
    const usize n = m_Enemies.Size();
    for (usize i = 0; i < n; ++i) {
        if (m_Enemies[i].is_engaged) ++count;
    }
    return count;
}

/** state が Engaged または BossFight なら true。 */
bool CCombatStateMachine::IsInCombat() const noexcept {
    return _state == ECombatState::Engaged || _state == ECombatState::BossFight;
}

/** 内部 state を遷移させ、必要なら callback を発火する。 */
void CCombatStateMachine::TransitionTo(ECombatState next) noexcept {
    if (next == _state) return;  // no-op (callback 不発火)
    const ECombatState prev = _state;
    _state = next;

    // 新 state の既定 threat target をセット (Tick 内で smooth に追従)。
    m_ThreatTarget = DefaultThreatTarget(next);

    // Engaged ドリフトタイマは Engaged へ "入った瞬間" にリセット。
    if (next == ECombatState::Engaged) {
        m_EngagedElapsed = 0.0f;
    } else {
        m_EngagedElapsed = 0.0f;
    }

    // callback は state 更新後に呼び、再入安全性を担保する。
    if (m_Callback != nullptr) {
        m_Callback(m_CallbackUser, prev, next);
    }
}

/** 敵検出を通知する (Peaceful なら Alert へ遷移)。 */
void CCombatStateMachine::NotifyEnemyDetected(u32 enemy_id) noexcept {
    // awareness レコードを upsert (新規 or 1.0 へ上書き)。is_engaged は据置 —
    // 既に交戦中の敵を再検出しても engaged 状態を解除しない。
    const usize idx = FindEnemy(enemy_id);
    if (idx >= m_Enemies.Size()) {
        FEnemyAwareness aw;
        aw.enemy_id        = enemy_id;
        aw.awareness_level = 1.0f;
        aw.is_engaged      = false;
        m_Enemies.PushBack(aw);
    } else {
        m_Enemies[idx].awareness_level = 1.0f;
    }

    // state 遷移: Peaceful のみ Alert に上げる。Alert / Engaged / BossFight /
    // Victory / Retreat の場合は state 据置 (新規敵検出だけでは BossFight が
    // 解除されたりはしない、という意味)。
    if (_state == ECombatState::Peaceful) {
        TransitionTo(ECombatState::Alert);
    }
}

/** 交戦開始を通知する (Engaged へ遷移、BossFight 中は据置)。 */
void CCombatStateMachine::NotifyCombatStarted(u32 enemy_id) noexcept {
    // 該当敵を is_engaged=true に。未登録なら新規追加 (awareness=1.0)。
    const usize idx = FindEnemy(enemy_id);
    if (idx >= m_Enemies.Size()) {
        FEnemyAwareness aw;
        aw.enemy_id        = enemy_id;
        aw.awareness_level = 1.0f;
        aw.is_engaged      = true;
        m_Enemies.PushBack(aw);
    } else {
        m_Enemies[idx].awareness_level = 1.0f;
        m_Enemies[idx].is_engaged      = true;
    }

    // BossFight 中は state 据置 (boss 優先)。それ以外なら Engaged へ。
    if (_state == ECombatState::BossFight) return;
    if (_state != ECombatState::Engaged) {
        TransitionTo(ECombatState::Engaged);
    }
}

/** 1 敵分の交戦解消を通知する (残敵 0 で Victory / Retreat へ遷移)。 */
void CCombatStateMachine::NotifyCombatEnded(u32 enemy_id, bool victory) noexcept {
    // 該当敵を engaged から外す。awareness は 0 に落として「忘れた」扱いに。
    // (= 次フレーム以降の NotifyEnemyDetected で再 alert 可能)
    const usize idx = FindEnemy(enemy_id);
    if (idx < m_Enemies.Size()) {
        m_Enemies[idx].is_engaged      = false;
        m_Enemies[idx].awareness_level = 0.0f;
    } else {
        // 未登録 enemy_id で end が来た: 警告のみで no-op。
        ACS_LOG_WARN("FCombatStateMachine::NotifyCombatEnded: unknown enemy_id=%u", enemy_id);
    }

    // BossFight 中は boss 撃破が別 API なので、ここでは state を動かさない。
    if (_state == ECombatState::BossFight) return;

    // 残りの engaged 数を見て、0 なら Victory / Retreat に遷移、>0 なら継続。
    if (EngagedEnemyCount() == 0) {
        TransitionTo(victory ? ECombatState::Victory : ECombatState::Retreat);
    }
}

/** ボス遭遇を通知する (どの state からでも BossFight へ割り込み遷移)。 */
void CCombatStateMachine::NotifyBossEncountered(u32 boss_id) noexcept {
    // ボスを engaged 一覧に upsert (awareness=1, is_engaged=true)。
    const usize idx = FindEnemy(boss_id);
    if (idx >= m_Enemies.Size()) {
        FEnemyAwareness aw;
        aw.enemy_id        = boss_id;
        aw.awareness_level = 1.0f;
        aw.is_engaged      = true;
        m_Enemies.PushBack(aw);
    } else {
        m_Enemies[idx].awareness_level = 1.0f;
        m_Enemies[idx].is_engaged      = true;
    }

    // 復帰先を覚えておく: Engaged 中なら Engaged に、それ以外なら Peaceful に
    // 戻す (= 一般敵が残っていない状況での孤独なボス撃破は Victory に行く)。
    m_PreBossState = (_state == ECombatState::Engaged) ? ECombatState::Engaged
                                                       : ECombatState::Peaceful;

    // どの state からでも BossFight に割り込み遷移。
    if (_state != ECombatState::BossFight) {
        TransitionTo(ECombatState::BossFight);
    }
}

/** ボス撃破を通知する (残 engaged 敵で Engaged / Victory へ復帰)。 */
void CCombatStateMachine::NotifyBossDefeated() noexcept {
    // BossFight 以外で呼ばれたら警告 + no-op (誤用検出)。
    if (_state != ECombatState::BossFight) {
        ACS_LOG_WARN("FCombatStateMachine::NotifyBossDefeated: not in BossFight (state=%u)",
                     static_cast<u32>(_state));
        return;
    }

    // is_engaged=true な最初の敵を boss とみなして外す (= 単一ボス想定)。
    // 複数ボス対応が必要になったら ID を引数に取る別 API を追加する。
    const usize n = m_Enemies.Size();
    for (usize i = 0; i < n; ++i) {
        if (m_Enemies[i].is_engaged) {
            m_Enemies[i].is_engaged      = false;
            m_Enemies[i].awareness_level = 0.0f;
            break;
        }
    }

    // 残りの engaged 数で復帰先を決定:
    //   >0 → Engaged (一般敵がまだ残っている)
    //    0 → Victory (= boss 撃破で全戦闘終了)
    if (EngagedEnemyCount() > 0) {
        TransitionTo(ECombatState::Engaged);
    } else {
        TransitionTo(ECombatState::Victory);
    }
}

/** 毎フレーム呼んで脅威レベルを target へ指数減衰で追従させる driver。 */
void CCombatStateMachine::Tick(f32 dt) noexcept {
    if (dt < 0.0f) dt = 0.0f;

    // Engaged 中はドリフトを target に加算: 戦闘が長引くほど脅威感が上がる。
    // 60 秒で +0.3 (上限) に到達する曲線 (= 0.005/sec を m_EngagedElapsed と
    // 連動させた cap 加算)。Engaged 以外では m_EngagedElapsed=0 にリセット
    // 済みなので drift=0。
    f32 effective_target = m_ThreatTarget;
    if (_state == ECombatState::Engaged) {
        m_EngagedElapsed += dt;
        const f32 drift = m_EngagedElapsed * 0.005f;  // 60s で +0.3
        const f32 drift_capped = drift > 0.3f ? 0.3f : drift;
        effective_target = Clamp01(m_ThreatTarget + drift_capped);
    }

    // 指数減衰追従: tau=0.5s で current → effective_target。
    // alpha = 1 - exp(-dt/tau)。tau=0.5s なら dt=16ms で alpha≈0.0317。
    // BossFight だけは tau=0.25s に短縮し、立ち上がりを鋭くする。
    const f32 tau = (_state == ECombatState::BossFight) ? 0.25f : 0.5f;
    // 近似式 (Taylor 展開): alpha ≈ dt / (tau + dt)。STL 不使用方針で expf を
    // 避け、十分滑らかな一次遅れに収まる近似を採用。
    const f32 alpha = dt / (tau + dt);
    m_ThreatCurrent += (effective_target - m_ThreatCurrent) * alpha;
    m_ThreatCurrent  = Clamp01(m_ThreatCurrent);
}

/** state 遷移 callback を登録する (cb==nullptr で解除)。 */
void CCombatStateMachine::SetOnStateChangeCallback(StateChangeCallback cb, void* user) noexcept {
    m_Callback      = cb;
    m_CallbackUser = user;
}

} // namespace acs::game
