// SPDX-License-Identifier: Apache-2.0
// HelloBehaviorTreeEditor — Task/Action Fn 群 + Condition Fn の実装。
//
// graph-run (registry 実行) では panel のグラフインタプリタが各ノードの last_status と
// 実行フローを自動更新するため、Fn 側は blackboard 変数を名前で読み書きして status を
// 返すことに集中する。本サンプルのタスクは状態を持たない最小スタブ:
//   ・Attack / Pickup … 即 Success
//   ・Move   / Wait   … 継続 (Running) する巡回 / 待機
// 流れの切り替えは Condition(CanSeePlayer) と Compare(see_phase / health) が担う。
#include "TreeActions.h"

using namespace acs;
using namespace acs::game;

namespace hellobt {

// "Pickup" — 即 Success (アイテム取得は即時完了)。
EBtStatus ActionPickup(void* /*blackboard*/, f32 /*dt*/) noexcept {
    return EBtStatus::Success;
}

// "Attack" — 即 Success (攻撃成立)。
EBtStatus ActionAttack(void* /*blackboard*/, f32 /*dt*/) noexcept {
    return EBtStatus::Success;
}

// "Move" — 巡回。継続するので常に Running を返す (Selector の末端フォールバック)。
EBtStatus ActionMove(void* /*blackboard*/, f32 /*dt*/) noexcept {
    return EBtStatus::Running;
}

// "Wait" — 待機。継続するので常に Running を返す。
EBtStatus ActionWait(void* /*blackboard*/, f32 /*dt*/) noexcept {
    return EBtStatus::Running;
}

// "CanSeePlayer" — Condition デコレーター用の条件関数。
//   blackboard 変数 "see_phase" を毎 tick 進めて 0..119 を周回させ、前半 (<60) で true。
//   約1秒周期で「見えている/いない」が切り替わり、子のガードがライブで切り替わる。
//   see_phase は動的ブラックボード上の変数なので、エディタの Compare デコレーター
//   (see_phase > N) からも同じ値が参照される。
bool CanSeePlayer(void* blackboard) noexcept {
    auto* bb = static_cast<Blackboard*>(blackboard);
    if (!bb) return false;
    const i32 p = (bb->GetI32("see_phase") + 1) % 120;
    bb->SetI32("see_phase", p);
    return p < 60;
}

} // namespace hellobt
