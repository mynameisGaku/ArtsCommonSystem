// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R — FCombatStateMachine 実装
//
// state machine + threat level smoothing + enemy awareness tracking を完全実装。
// 外部ディレクタ (Music / Ambient / UI) との結線は OnStateChange callback で
// caller が定義する想定。本クラスは FAudioEngine 等の下位リソースを直接知らない。
#include "gameframework/CombatStateMachine.h"
#include "foundation/Log.h"

namespace acs::game {

// ----------------------------------------------------------------------------
// helpers
// ----------------------------------------------------------------------------

f32 FCombatStateMachine::Clamp01(f32 v) noexcept {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

// 各 state の既定脅威レベル。Tick 内で _threat_target に流し込む。
// 値は「BGM intensity / camera shake の自然な強度」に合わせた経験値。
//   Peaceful  = 0.00 — 平穏 (BGM Calm / 環境音 のみ)
//   Alert     = 0.30 — 緊張 (BGM Tension / 心拍 SE)
//   Engaged   = 0.70 — 戦闘 (BGM Combat / 連続 SFX)
//   BossFight = 1.00 — ボス戦 (BGM Combat 最大 / 振動最大)
//   Victory   = 0.40 — 勝利直後 (Engaged からの自然な減衰、Calm へは Reset で)
//   Retreat   = 0.20 — 撤退 (緊張感は残るが Combat ではない)
f32 FCombatStateMachine::DefaultThreatTarget(ECombatState state) noexcept {
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

// ----------------------------------------------------------------------------
// construction / lifecycle
// ----------------------------------------------------------------------------

FCombatStateMachine::FCombatStateMachine() noexcept {
    _enemies.Reserve(kEnemyReserveHint);
}

void FCombatStateMachine::Init() noexcept {
    _state             = ECombatState::Peaceful;
    _threat_target     = DefaultThreatTarget(ECombatState::Peaceful);
    _threat_current    = 0.0f;
    _engaged_elapsed   = 0.0f;
    _pre_boss_state    = ECombatState::Peaceful;
    _enemies.Clear();
    // _callback / _callback_user は保持 (Init は scene 再 enter 用と位置付け)。
}

void FCombatStateMachine::Reset() noexcept {
    Init();
    _callback      = nullptr;
    _callback_user = nullptr;
}

// ----------------------------------------------------------------------------
// enemy 検索 / 取得
// ----------------------------------------------------------------------------

usize FCombatStateMachine::FindEnemy(u32 enemy_id) const noexcept {
    const usize n = _enemies.Size();
    for (usize i = 0; i < n; ++i) {
        if (_enemies[i].enemy_id == enemy_id) return i;
    }
    return n;
}

u32 FCombatStateMachine::EngagedEnemyCount() const noexcept {
    u32 count = 0;
    const usize n = _enemies.Size();
    for (usize i = 0; i < n; ++i) {
        if (_enemies[i].is_engaged) ++count;
    }
    return count;
}

bool FCombatStateMachine::IsInCombat() const noexcept {
    return _state == ECombatState::Engaged || _state == ECombatState::BossFight;
}

// ----------------------------------------------------------------------------
// state 遷移コア
// ----------------------------------------------------------------------------

void FCombatStateMachine::TransitionTo(ECombatState next) noexcept {
    if (next == _state) return;  // no-op (callback 不発火)
    const ECombatState prev = _state;
    _state = next;

    // 新 state の既定 threat target をセット (Tick 内で smooth に追従)。
    _threat_target = DefaultThreatTarget(next);

    // Engaged ドリフトタイマは Engaged へ "入った瞬間" にリセット。
    if (next == ECombatState::Engaged) {
        _engaged_elapsed = 0.0f;
    } else {
        _engaged_elapsed = 0.0f;
    }

    // callback は state 更新後に呼び、再入安全性を担保する。
    if (_callback != nullptr) {
        _callback(_callback_user, prev, next);
    }
}

// ----------------------------------------------------------------------------
// 戦闘通知
// ----------------------------------------------------------------------------

void FCombatStateMachine::NotifyEnemyDetected(u32 enemy_id) noexcept {
    // awareness レコードを upsert (新規 or 1.0 へ上書き)。is_engaged は据置 —
    // 既に交戦中の敵を再検出しても engaged 状態を解除しない。
    const usize idx = FindEnemy(enemy_id);
    if (idx >= _enemies.Size()) {
        EnemyAwareness aw;
        aw.enemy_id        = enemy_id;
        aw.awareness_level = 1.0f;
        aw.is_engaged      = false;
        _enemies.PushBack(aw);
    } else {
        _enemies[idx].awareness_level = 1.0f;
    }

    // state 遷移: Peaceful のみ Alert に上げる。Alert / Engaged / BossFight /
    // Victory / Retreat の場合は state 据置 (新規敵検出だけでは BossFight が
    // 解除されたりはしない、という意味)。
    if (_state == ECombatState::Peaceful) {
        TransitionTo(ECombatState::Alert);
    }
}

void FCombatStateMachine::NotifyCombatStarted(u32 enemy_id) noexcept {
    // 該当敵を is_engaged=true に。未登録なら新規追加 (awareness=1.0)。
    const usize idx = FindEnemy(enemy_id);
    if (idx >= _enemies.Size()) {
        EnemyAwareness aw;
        aw.enemy_id        = enemy_id;
        aw.awareness_level = 1.0f;
        aw.is_engaged      = true;
        _enemies.PushBack(aw);
    } else {
        _enemies[idx].awareness_level = 1.0f;
        _enemies[idx].is_engaged      = true;
    }

    // BossFight 中は state 据置 (boss 優先)。それ以外なら Engaged へ。
    if (_state == ECombatState::BossFight) return;
    if (_state != ECombatState::Engaged) {
        TransitionTo(ECombatState::Engaged);
    }
}

void FCombatStateMachine::NotifyCombatEnded(u32 enemy_id, bool victory) noexcept {
    // 該当敵を engaged から外す。awareness は 0 に落として「忘れた」扱いに。
    // (= 次フレーム以降の NotifyEnemyDetected で再 alert 可能)
    const usize idx = FindEnemy(enemy_id);
    if (idx < _enemies.Size()) {
        _enemies[idx].is_engaged      = false;
        _enemies[idx].awareness_level = 0.0f;
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

void FCombatStateMachine::NotifyBossEncountered(u32 boss_id) noexcept {
    // ボスを engaged 一覧に upsert (awareness=1, is_engaged=true)。
    const usize idx = FindEnemy(boss_id);
    if (idx >= _enemies.Size()) {
        EnemyAwareness aw;
        aw.enemy_id        = boss_id;
        aw.awareness_level = 1.0f;
        aw.is_engaged      = true;
        _enemies.PushBack(aw);
    } else {
        _enemies[idx].awareness_level = 1.0f;
        _enemies[idx].is_engaged      = true;
    }

    // 復帰先を覚えておく: Engaged 中なら Engaged に、それ以外なら Peaceful に
    // 戻す (= 一般敵が残っていない状況での孤独なボス撃破は Victory に行く)。
    _pre_boss_state = (_state == ECombatState::Engaged) ? ECombatState::Engaged
                                                       : ECombatState::Peaceful;

    // どの state からでも BossFight に割り込み遷移。
    if (_state != ECombatState::BossFight) {
        TransitionTo(ECombatState::BossFight);
    }
}

void FCombatStateMachine::NotifyBossDefeated() noexcept {
    // BossFight 以外で呼ばれたら警告 + no-op (誤用検出)。
    if (_state != ECombatState::BossFight) {
        ACS_LOG_WARN("FCombatStateMachine::NotifyBossDefeated: not in BossFight (state=%u)",
                     static_cast<u32>(_state));
        return;
    }

    // is_engaged=true な最初の敵を boss とみなして外す (= 単一ボス想定)。
    // 複数ボス対応が必要になったら ID を引数に取る別 API を追加する。
    const usize n = _enemies.Size();
    for (usize i = 0; i < n; ++i) {
        if (_enemies[i].is_engaged) {
            _enemies[i].is_engaged      = false;
            _enemies[i].awareness_level = 0.0f;
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

// ----------------------------------------------------------------------------
// driver
// ----------------------------------------------------------------------------

void FCombatStateMachine::Tick(f32 dt) noexcept {
    if (dt < 0.0f) dt = 0.0f;

    // Engaged 中はドリフトを target に加算: 戦闘が長引くほど脅威感が上がる。
    // 60 秒で +0.3 (上限) に到達する曲線 (= 0.005/sec を _engaged_elapsed と
    // 連動させた cap 加算)。Engaged 以外では _engaged_elapsed=0 にリセット
    // 済みなので drift=0。
    f32 effective_target = _threat_target;
    if (_state == ECombatState::Engaged) {
        _engaged_elapsed += dt;
        const f32 drift = _engaged_elapsed * 0.005f;  // 60s で +0.3
        const f32 drift_capped = drift > 0.3f ? 0.3f : drift;
        effective_target = Clamp01(_threat_target + drift_capped);
    }

    // 指数減衰追従: tau=0.5s で current → effective_target。
    // alpha = 1 - exp(-dt/tau)。tau=0.5s なら dt=16ms で alpha≈0.0317。
    // BossFight だけは tau=0.25s に短縮し、立ち上がりを鋭くする。
    const f32 tau = (_state == ECombatState::BossFight) ? 0.25f : 0.5f;
    // 近似式 (Taylor 展開): alpha ≈ dt / (tau + dt)。STL 不使用方針で expf を
    // 避け、十分滑らかな一次遅れに収まる近似を採用。
    const f32 alpha = dt / (tau + dt);
    _threat_current += (effective_target - _threat_current) * alpha;
    _threat_current  = Clamp01(_threat_current);
}

// ----------------------------------------------------------------------------
// callback
// ----------------------------------------------------------------------------

void FCombatStateMachine::SetOnStateChangeCallback(StateChangeCallback cb, void* user) noexcept {
    _callback      = cb;
    _callback_user = user;
}

} // namespace acs::game
