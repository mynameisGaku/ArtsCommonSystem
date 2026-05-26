// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar L — FAnimationGraph 実装 (Phase 4)
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
//     local_time を保持していないため、本 Phase では呼出側 renderer が
//     「current clip のみを alpha で fade-in」する想定。前状態を含めた true
//     cross-fade は Phase 5+ で previous_local_time を別途保持する形に拡張。
//   ・state node 0 個での Tick / Reset は no-op。各 API は debug ビルドでも
//     crash させない方針 (= FSpriteAnimator / FCooldownTimer と同方針)。
#include "gameframework/AnimationGraph.h"

#include <cstring>   // strcmp

namespace acs::game {

namespace {

// param 名比較 (literal 共有想定だが別バッファでも機能するよう strcmp 経由)
inline bool NamesEqual(const char* a, const char* b) noexcept {
    if (a == b)                       return true;  // pointer 一致 fast path
    if (a == nullptr || b == nullptr) return false;
    return ::strcmp(a, b) == 0;
}

constexpr u32 kInvalidIndex = 0xFFFFFFFFu;

} // namespace

// =============================================================================
// 初期化 / 解放
// =============================================================================

void FAnimationGraph::Init() noexcept {
    _clips.Clear();
    _state_nodes.Clear();
    _transitions.Clear();
    _params.Clear();

    _current_state    = EAnimationGraphState::Idle;
    _previous_state   = EAnimationGraphState::Idle;
    _local_time       = 0.0f;
    _blend_timer      = 0.0f;
    _blend_duration   = 0.0f;
    _has_current      = false;
    _clip_ended_fired = false;
    _trigger_pending  = false;
    _pending_trigger  = EAnimationGraphState::Idle;

    _state_enter_cb   = nullptr;
    _state_enter_user = nullptr;
    _clip_end_cb      = nullptr;
    _clip_end_user    = nullptr;
}

void FAnimationGraph::Shutdown() noexcept {
    _clips.Clear();
    _state_nodes.Clear();
    _transitions.Clear();
    _params.Clear();

    _has_current      = false;
    _trigger_pending  = false;
    _blend_timer      = 0.0f;
    _blend_duration   = 0.0f;
    _local_time       = 0.0f;
    _clip_ended_fired = false;
}

// =============================================================================
// clip 管理
// =============================================================================

u32 FAnimationGraph::AddClip(const FAnimationClipBinding& clip) noexcept {
    _clips.PushBack(clip);
    return static_cast<u32>(_clips.Size()) - 1u;
}

u32 FAnimationGraph::ClipCount() const noexcept {
    return static_cast<u32>(_clips.Size());
}

const FAnimationClipBinding* FAnimationGraph::GetClip(u32 i) const noexcept {
    if (i >= _clips.Size()) return nullptr;
    return &_clips[i];
}

// =============================================================================
// state node 管理
// =============================================================================

void FAnimationGraph::AddStateNode(const FAnimationStateNode& node) noexcept {
    // 既存 ID と同じ node なら後勝ち上書き (重複 ID を許容しない)
    const usize n = _state_nodes.Size();
    for (usize i = 0; i < n; ++i) {
        if (_state_nodes[i].id == node.id) {
            _state_nodes[i] = node;
            return;
        }
    }
    _state_nodes.PushBack(node);

    // 初回追加で _has_current が立っていなければ初期状態としてセット
    if (!_has_current) {
        _current_state    = node.id;
        _previous_state   = node.id;
        _local_time       = 0.0f;
        _blend_timer      = 0.0f;
        _blend_duration   = 0.0f;
        _clip_ended_fired = false;
        _has_current      = true;
    }
}

u32 FAnimationGraph::StateNodeCount() const noexcept {
    return static_cast<u32>(_state_nodes.Size());
}

const FAnimationStateNode* FAnimationGraph::GetStateNode(u32 i) const noexcept {
    if (i >= _state_nodes.Size()) return nullptr;
    return &_state_nodes[i];
}

// =============================================================================
// transition 管理
// =============================================================================

void FAnimationGraph::AddTransition(const FAnimationTransition& trans) noexcept {
    _transitions.PushBack(trans);
}

u32 FAnimationGraph::TransitionCount() const noexcept {
    return static_cast<u32>(_transitions.Size());
}

const FAnimationTransition* FAnimationGraph::GetTransition(u32 i) const noexcept {
    if (i >= _transitions.Size()) return nullptr;
    return &_transitions[i];
}

// =============================================================================
// パラメータ
// =============================================================================

void FAnimationGraph::SetParam(const char* name, f32 value) noexcept {
    if (name == nullptr) return;

    const usize n = _params.Size();
    for (usize i = 0; i < n; ++i) {
        if (NamesEqual(_params[i].name, name)) {
            _params[i].value = value;
            return;
        }
    }
    FParam p;
    p.name  = name;
    p.value = value;
    _params.PushBack(p);
}

f32 FAnimationGraph::GetParam(const char* name) const noexcept {
    if (name == nullptr) return 0.0f;
    const usize n = _params.Size();
    for (usize i = 0; i < n; ++i) {
        if (NamesEqual(_params[i].name, name)) {
            return _params[i].value;
        }
    }
    return 0.0f;
}

// =============================================================================
// 遷移制御
// =============================================================================

void FAnimationGraph::TriggerTransition(EAnimationGraphState target_state) noexcept {
    // 同 state への明示的遷移は no-op (= 意図せぬ self-loop による再 enter を
    // 防ぐ; どうしても再 enter したい場合は一旦別 state を経由する設計に)
    if (_has_current && target_state == _current_state) return;
    _pending_trigger = target_state;
    _trigger_pending = true;
}

// =============================================================================
// 状態取得
// =============================================================================

f32 FAnimationGraph::CurrentBlendAlpha() const noexcept {
    if (_blend_duration <= 0.0f) return 1.0f;       // 即時切替の場合
    if (_blend_timer    <= 0.0f) return 1.0f;       // blend 完了
    // blend_timer は「残時間」: alpha = 1 - (残 / 全)
    const f32 ratio = _blend_timer / _blend_duration;
    const f32 a     = 1.0f - ratio;
    if (a < 0.0f) return 0.0f;
    if (a > 1.0f) return 1.0f;
    return a;
}

u32 FAnimationGraph::CurrentClipIndex() const noexcept {
    if (!_has_current) return 0u;
    const u32 idx = FindStateNodeIndex(_current_state);
    if (idx == kInvalidIndex) return 0u;
    return _state_nodes[idx].clip_index;
}

// =============================================================================
// 内部ヘルパ
// =============================================================================

u32 FAnimationGraph::FindStateNodeIndex(EAnimationGraphState id) const noexcept {
    const usize n = _state_nodes.Size();
    for (usize i = 0; i < n; ++i) {
        if (_state_nodes[i].id == id) return static_cast<u32>(i);
    }
    return kInvalidIndex;
}

void FAnimationGraph::DoTransition(EAnimationGraphState target) noexcept {
    const u32 target_idx = FindStateNodeIndex(target);
    if (target_idx == kInvalidIndex) return;   // 未登録 state への遷移は無視

    const EAnimationGraphState from = _current_state;
    _previous_state   = from;
    _current_state    = target;
    _local_time       = 0.0f;
    _clip_ended_fired = false;
    _blend_duration   = _state_nodes[target_idx].enter_blend_sec;
    if (_blend_duration < 0.0f) _blend_duration = 0.0f;
    _blend_timer      = _blend_duration;  // duration==0 なら即時 alpha=1
    _has_current      = true;

    if (_state_enter_cb != nullptr) {
        _state_enter_cb(_state_enter_user, from, target);
    }
}

bool FAnimationGraph::AdvanceLocalTime(f32 dt) noexcept {
    const u32 idx = FindStateNodeIndex(_current_state);
    if (idx == kInvalidIndex) return false;

    const FAnimationStateNode& node = _state_nodes[idx];
    if (node.clip_index >= _clips.Size()) return false;

    const FAnimationClipBinding& clip = _clips[node.clip_index];

    const f32 speed = clip.default_speed;
    _local_time += dt * speed;

    // 0 以下の duration は進行させない (= ガード)
    if (clip.duration_sec <= 0.0f) {
        _local_time = 0.0f;
        return false;
    }

    bool ended = false;
    if (clip.is_looping) {
        // 巨大 dt 対策で while wrap (f32 精度を保つため)
        while (_local_time >= clip.duration_sec) {
            _local_time -= clip.duration_sec;
        }
        if (_local_time < 0.0f) _local_time = 0.0f;
    } else {
        if (_local_time >= clip.duration_sec) {
            _local_time = clip.duration_sec;
            ended       = true;
        }
        if (_local_time < 0.0f) _local_time = 0.0f;
    }
    return ended;
}

bool FAnimationGraph::EvaluateTransitions() noexcept {
    const usize n = _transitions.Size();
    for (usize i = 0; i < n; ++i) {
        const FAnimationTransition& t = _transitions[i];
        if (t.from != _current_state) continue;

        // param 条件: name が nullptr なら param 評価をスキップ (exit_immediately
        // のみで成立判定)。
        bool param_ok = true;
        if (t.condition_param_name != nullptr) {
            const f32 v = GetParam(t.condition_param_name);
            param_ok = (v >= t.condition_param_threshold);
        }
        if (!param_ok) continue;

        // exit_immediately=false の場合は Once clip 終端到達時のみ発火
        if (!t.exit_immediately && !_clip_ended_fired) continue;

        DoTransition(t.to);
        return true;
    }
    return false;
}

// =============================================================================
// Tick / Reset
// =============================================================================

void FAnimationGraph::Tick(f32 dt) noexcept {
    if (dt <= 0.0f)            return;
    if (_state_nodes.IsEmpty()) return;
    if (!_has_current)         return;

    // 1. pending trigger を最優先で処理 (input event 駆動の即時遷移を保証)
    if (_trigger_pending) {
        _trigger_pending = false;
        DoTransition(_pending_trigger);
        // 遷移後 1 Tick の clip 進行は次フレームから (= enter 直後の安定化)
        // ただし blend timer は本 Tick で進めておく必要が無いので return
        return;
    }

    // 2. blend timer を進める (CurrentBlendAlpha() は lazy 計算)
    if (_blend_timer > 0.0f) {
        _blend_timer -= dt;
        if (_blend_timer < 0.0f) _blend_timer = 0.0f;
    }

    // 3. local_time を進める。Once clip 終端到達なら ClipEndCallback 発火 (1 度のみ)
    const bool ended = AdvanceLocalTime(dt);
    if (ended && !_clip_ended_fired) {
        _clip_ended_fired = true;
        if (_clip_end_cb != nullptr) {
            const u32 ci = CurrentClipIndex();
            _clip_end_cb(_clip_end_user, _current_state, ci);
        }
    }

    // 4. transitions 評価 (from==current のものを 1 つだけ発火)
    EvaluateTransitions();
}

void FAnimationGraph::Reset() noexcept {
    if (_state_nodes.IsEmpty()) {
        _has_current      = false;
        _local_time       = 0.0f;
        _blend_timer      = 0.0f;
        _blend_duration   = 0.0f;
        _clip_ended_fired = false;
        _trigger_pending  = false;
        return;
    }

    // 先頭 state を初期状態として採用 (AddStateNode 1 つ目で _has_current が
    // 立つときと同じ規約)。
    const FAnimationStateNode& first = _state_nodes[0];
    _current_state    = first.id;
    _previous_state   = first.id;
    _local_time       = 0.0f;
    _blend_timer      = 0.0f;
    _blend_duration   = 0.0f;
    _clip_ended_fired = false;
    _trigger_pending  = false;
    _has_current      = true;
}

// =============================================================================
// callback
// =============================================================================

void FAnimationGraph::SetOnStateEnterCallback(StateEnterCallback cb, void* user) noexcept {
    _state_enter_cb   = cb;
    _state_enter_user = user;
}

void FAnimationGraph::SetOnClipEndCallback(ClipEndCallback cb, void* user) noexcept {
    _clip_end_cb   = cb;
    _clip_end_user = user;
}

} // namespace acs::game
