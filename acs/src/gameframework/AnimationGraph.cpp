// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar L — CAnimationGraph 実装
//
// アルゴリズム概要:
//   Tick(dt):
//     1. pending trigger があれば最優先で DoTransition (input event 駆動の
//        即時遷移を保証)
//     2. blend_timer を dt 分減らす (blend 中の alpha 進行)
//     3. local_time を current clip の duration / is_looping に従い前進
//        ・loop: duration を超えたら wrap
//        ・once: duration で固定 + ClipEndCallback (1 度だけ)
//     4. transitions を走査 (from==current_state のみ評価):
//        ・exit_immediately=true && param >= threshold → 即遷移
//        ・exit_immediately=false → 「現 clip が終端に達した」直後だけ評価
//          (= looping clip では事実上発火しない)
//
//   DoTransition(target):
//     ・previous = current、current = target
//     ・blend_duration = (target node の enter_blend_sec)
//     ・blend_timer = blend_duration (0 なら即時)
//     ・local_time = 0、clip_ended_fired = false
//     ・StateEnterCallback 発火
//
// 設計メモ:
//   ・param 比較は pointer 一致 fast path → strcmp で literal 共有 / 別バッファ
//     両方を高速化する (= ACS の他箇所と整合)。
//   ・loop wrap は while で複数周回 (= 巨大 dt が来ても周期内に収める。f32 精度
//     ロス対策)。
//   ・blend は **「新状態が 0→1 で前面に来る」一方向 cross-fade**。前状態の
//     local_time を保持していないため、呼出側 renderer が「current clip のみを
//     alpha で fade-in」する想定。前状態を含めた true cross-fade は
//     previous_local_time を別途保持する形に拡張できる。
//   ・state node 0 個での Tick / Reset は no-op。各 API は debug ビルドでも
//     crash させない方針 (= CSpriteAnimator / CCooldownTimer と同方針)。
#include "gameframework/AnimationGraph.h"

#include <cstring>   // strcmp

namespace acs::game {

namespace {

/**
 * param 名を比較する (literal 共有想定だが別バッファでも機能するよう strcmp 経由)。
 *
 * @param a 比較する文字列 1。
 * @param b 比較する文字列 2。
 * @return 内容が等しければ true (両方 nullptr 以外で同ポインタなら fast path)。
 */
inline bool NamesEqual(const char* a, const char* b) noexcept {
    if (a == b)                       return true;  // pointer 一致 fast path
    if (a == nullptr || b == nullptr) return false;
    return ::strcmp(a, b) == 0;
}

/** state node が見つからなかったことを表す番兵 index。 */
constexpr u32 kInvalidIndex = 0xFFFFFFFFu;

} // namespace

void CAnimationGraph::Init() noexcept {
    m_Clips.Reset();
    _state_nodes.Reset();
    m_Transitions.Reset();
    m_Params.Reset();

    m_CurrentState    = EAnimationGraphState::Idle;
    m_PreviousState   = EAnimationGraphState::Idle;
    m_LocalTime       = 0.0f;
    m_BlendTimer      = 0.0f;
    m_BlendDuration   = 0.0f;
    m_HasCurrent      = false;
    m_ClipEndedFired = false;
    m_bTriggerPending  = false;
    m_PendingTrigger  = EAnimationGraphState::Idle;

    _state_enter_cb   = nullptr;
    _state_enter_user = nullptr;
    m_ClipEndCb      = nullptr;
    m_ClipEndUser    = nullptr;
}

void CAnimationGraph::Shutdown() noexcept {
    m_Clips.Reset();
    _state_nodes.Reset();
    m_Transitions.Reset();
    m_Params.Reset();

    m_HasCurrent      = false;
    m_bTriggerPending  = false;
    m_BlendTimer      = 0.0f;
    m_BlendDuration   = 0.0f;
    m_LocalTime       = 0.0f;
    m_ClipEndedFired = false;
}

u32 CAnimationGraph::AddClip(const FAnimationClipBinding& clip) noexcept {
    m_Clips.Add(clip);
    return static_cast<u32>(m_Clips.Num()) - 1u;
}

u32 CAnimationGraph::ClipCount() const noexcept {
    return static_cast<u32>(m_Clips.Num());
}

const FAnimationClipBinding* CAnimationGraph::GetClip(u32 i) const noexcept {
    if (i >= m_Clips.Num()) return nullptr;
    return &m_Clips[i];
}

void CAnimationGraph::AddStateNode(const FAnimationStateNode& node) noexcept {
    // 既存 ID と同じ node なら後勝ち上書き (重複 ID を許容しない)
    const usize n = _state_nodes.Num();
    for (usize i = 0; i < n; ++i) {
        if (_state_nodes[i].id == node.id) {
            _state_nodes[i] = node;
            return;
        }
    }
    _state_nodes.Add(node);

    // 初回追加で m_HasCurrent が立っていなければ初期状態としてセット
    if (!m_HasCurrent) {
        m_CurrentState    = node.id;
        m_PreviousState   = node.id;
        m_LocalTime       = 0.0f;
        m_BlendTimer      = 0.0f;
        m_BlendDuration   = 0.0f;
        m_ClipEndedFired = false;
        m_HasCurrent      = true;
    }
}

u32 CAnimationGraph::StateNodeCount() const noexcept {
    return static_cast<u32>(_state_nodes.Num());
}

const FAnimationStateNode* CAnimationGraph::GetStateNode(u32 i) const noexcept {
    if (i >= _state_nodes.Num()) return nullptr;
    return &_state_nodes[i];
}

void CAnimationGraph::AddTransition(const FAnimationTransition& trans) noexcept {
    m_Transitions.Add(trans);
}

u32 CAnimationGraph::TransitionCount() const noexcept {
    return static_cast<u32>(m_Transitions.Num());
}

const FAnimationTransition* CAnimationGraph::GetTransition(u32 i) const noexcept {
    if (i >= m_Transitions.Num()) return nullptr;
    return &m_Transitions[i];
}

void CAnimationGraph::SetParam(const char* name, f32 value) noexcept {
    if (name == nullptr) return;

    const usize n = m_Params.Num();
    for (usize i = 0; i < n; ++i) {
        if (NamesEqual(m_Params[i].name, name)) {
            m_Params[i].value = value;
            return;
        }
    }
    FParam p;
    p.name  = name;
    p.value = value;
    m_Params.Add(p);
}

f32 CAnimationGraph::GetParam(const char* name) const noexcept {
    if (name == nullptr) return 0.0f;
    const usize n = m_Params.Num();
    for (usize i = 0; i < n; ++i) {
        if (NamesEqual(m_Params[i].name, name)) {
            return m_Params[i].value;
        }
    }
    return 0.0f;
}

void CAnimationGraph::TriggerTransition(EAnimationGraphState target_state) noexcept {
    // 同 state への明示的遷移は no-op (= 意図せぬ self-loop による再 enter を
    // 防ぐ; どうしても再 enter したい場合は一旦別 state を経由する設計に)
    if (m_HasCurrent && target_state == m_CurrentState) return;
    m_PendingTrigger = target_state;
    m_bTriggerPending = true;
}

f32 CAnimationGraph::CurrentBlendAlpha() const noexcept {
    if (m_BlendDuration <= 0.0f) return 1.0f;       // 即時切替の場合
    if (m_BlendTimer    <= 0.0f) return 1.0f;       // blend 完了
    // blend_timer は「残時間」: alpha = 1 - (残 / 全)
    const f32 ratio = m_BlendTimer / m_BlendDuration;
    const f32 a     = 1.0f - ratio;
    if (a < 0.0f) return 0.0f;
    if (a > 1.0f) return 1.0f;
    return a;
}

u32 CAnimationGraph::CurrentClipIndex() const noexcept {
    if (!m_HasCurrent) return 0u;
    const u32 idx = FindStateNodeIndex(m_CurrentState);
    if (idx == kInvalidIndex) return 0u;
    return _state_nodes[idx].clip_index;
}

u32 CAnimationGraph::FindStateNodeIndex(EAnimationGraphState id) const noexcept {
    const usize n = _state_nodes.Num();
    for (usize i = 0; i < n; ++i) {
        if (_state_nodes[i].id == id) return static_cast<u32>(i);
    }
    return kInvalidIndex;
}

void CAnimationGraph::DoTransition(EAnimationGraphState target) noexcept {
    const u32 target_idx = FindStateNodeIndex(target);
    if (target_idx == kInvalidIndex) return;   // 未登録 state への遷移は無視

    const EAnimationGraphState from = m_CurrentState;
    m_PreviousState   = from;
    m_CurrentState    = target;
    m_LocalTime       = 0.0f;
    m_ClipEndedFired = false;
    m_BlendDuration   = _state_nodes[target_idx].enter_blend_sec;
    if (m_BlendDuration < 0.0f) m_BlendDuration = 0.0f;
    m_BlendTimer      = m_BlendDuration;  // duration==0 なら即時 alpha=1
    m_HasCurrent      = true;

    if (_state_enter_cb != nullptr) {
        _state_enter_cb(_state_enter_user, from, target);
    }
}

bool CAnimationGraph::AdvanceLocalTime(f32 dt) noexcept {
    const u32 idx = FindStateNodeIndex(m_CurrentState);
    if (idx == kInvalidIndex) return false;

    const FAnimationStateNode& node = _state_nodes[idx];
    if (node.clip_index >= m_Clips.Num()) return false;

    const FAnimationClipBinding& clip = m_Clips[node.clip_index];

    const f32 speed = clip.default_speed;
    m_LocalTime += dt * speed;

    // 0 以下の duration は進行させない (= ガード)
    if (clip.duration_sec <= 0.0f) {
        m_LocalTime = 0.0f;
        return false;
    }

    bool ended = false;
    if (clip.is_looping) {
        // 巨大 dt 対策で while wrap (f32 精度を保つため)
        while (m_LocalTime >= clip.duration_sec) {
            m_LocalTime -= clip.duration_sec;
        }
        if (m_LocalTime < 0.0f) m_LocalTime = 0.0f;
    } else {
        if (m_LocalTime >= clip.duration_sec) {
            m_LocalTime = clip.duration_sec;
            ended       = true;
        }
        if (m_LocalTime < 0.0f) m_LocalTime = 0.0f;
    }
    return ended;
}

bool CAnimationGraph::EvaluateTransitions() noexcept {
    const usize n = m_Transitions.Num();
    for (usize i = 0; i < n; ++i) {
        const FAnimationTransition& t = m_Transitions[i];
        if (t.from != m_CurrentState) continue;

        // param 条件: name が nullptr なら param 評価をスキップ (exit_immediately
        // のみで成立判定)。
        bool param_ok = true;
        if (t.condition_param_name != nullptr) {
            const f32 v = GetParam(t.condition_param_name);
            param_ok = (v >= t.condition_param_threshold);
        }
        if (!param_ok) continue;

        // exit_immediately=false の場合は Once clip 終端到達時のみ発火
        if (!t.exit_immediately && !m_ClipEndedFired) continue;

        DoTransition(t.to);
        return true;
    }
    return false;
}

void CAnimationGraph::Tick(f32 dt) noexcept {
    if (dt <= 0.0f)            return;
    if (_state_nodes.IsEmpty()) return;
    if (!m_HasCurrent)         return;

    // 1. pending trigger を最優先で処理 (input event 駆動の即時遷移を保証)
    if (m_bTriggerPending) {
        m_bTriggerPending = false;
        DoTransition(m_PendingTrigger);
        // 遷移後 1 Tick の clip 進行は次フレームから (= enter 直後の安定化)
        // ただし blend timer は本 Tick で進めておく必要が無いので return
        return;
    }

    // 2. blend timer を進める (CurrentBlendAlpha() は lazy 計算)
    if (m_BlendTimer > 0.0f) {
        m_BlendTimer -= dt;
        if (m_BlendTimer < 0.0f) m_BlendTimer = 0.0f;
    }

    // 3. local_time を進める。Once clip 終端到達なら ClipEndCallback 発火 (1 度のみ)
    const bool ended = AdvanceLocalTime(dt);
    if (ended && !m_ClipEndedFired) {
        m_ClipEndedFired = true;
        if (m_ClipEndCb != nullptr) {
            const u32 ci = CurrentClipIndex();
            m_ClipEndCb(m_ClipEndUser, m_CurrentState, ci);
        }
    }

    // 4. transitions 評価 (from==current のものを 1 つだけ発火)
    EvaluateTransitions();
}

void CAnimationGraph::Reset() noexcept {
    if (_state_nodes.IsEmpty()) {
        m_HasCurrent      = false;
        m_LocalTime       = 0.0f;
        m_BlendTimer      = 0.0f;
        m_BlendDuration   = 0.0f;
        m_ClipEndedFired = false;
        m_bTriggerPending  = false;
        return;
    }

    // 先頭 state を初期状態として採用 (AddStateNode 1 つ目で m_HasCurrent が
    // 立つときと同じ規約)。
    const FAnimationStateNode& first = _state_nodes[0];
    m_CurrentState    = first.id;
    m_PreviousState   = first.id;
    m_LocalTime       = 0.0f;
    m_BlendTimer      = 0.0f;
    m_BlendDuration   = 0.0f;
    m_ClipEndedFired = false;
    m_bTriggerPending  = false;
    m_HasCurrent      = true;
}

void CAnimationGraph::SetOnStateEnterCallback(StateEnterCallback cb, void* user) noexcept {
    _state_enter_cb   = cb;
    _state_enter_user = user;
}

void CAnimationGraph::SetOnClipEndCallback(ClipEndCallback cb, void* user) noexcept {
    m_ClipEndCb   = cb;
    m_ClipEndUser = user;
}

} // namespace acs::game
