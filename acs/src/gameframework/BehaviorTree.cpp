// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar L — BehaviorTree 実装
//
// Selector / Sequence のループはどちらも「子を順に Tick し、ある条件で
// 早期 return、最後まで通り抜けたら反対の結論を返す」という対称形をしている。
// 表 (ヘッダ参照) のとおり、停止条件と最終 fallthrough が真逆なだけ。
#include "gameframework/BehaviorTree.h"
#include "foundation/Move.h"

namespace acs::game {

// ---- BtSelector --------------------------------------------------------------

void BtSelector::AddChild(UniquePtr<BtNode> child) noexcept {
    // nullptr 子はそもそも tick できないので追加しない (静かに無視)。
    if (!child) return;
    _children.PushBack(Move(child));
}

BtStatus BtSelector::Tick(void* blackboard, f32 dt) noexcept {
    // OR セマンティクス: Running か Success を見つけたらその時点で抜ける。
    //   ・Running → 子がまだ進行中、Selector も Running を伝播
    //   ・Success → この子で目的達成、Selector も Success
    //   ・Failure → 次の子へ
    // すべて Failure を返したら、Selector 全体としても Failure。
    const usize n = _children.Size();
    for (usize i = 0; i < n; ++i) {
        BtNode* c = _children[i].Get();
        if (c == nullptr) continue;            // 万一の null 安全 (本来 AddChild で弾く)
        const BtStatus s = c->Tick(blackboard, dt);
        if (s == BtStatus::Running) return BtStatus::Running;
        if (s == BtStatus::Success) return BtStatus::Success;
        // Failure: 次の子へ
    }
    return BtStatus::Failure;
}

// ---- BtSequence --------------------------------------------------------------

void BtSequence::AddChild(UniquePtr<BtNode> child) noexcept {
    if (!child) return;
    _children.PushBack(Move(child));
}

BtStatus BtSequence::Tick(void* blackboard, f32 dt) noexcept {
    // AND セマンティクス: Running か Failure を見つけたらその時点で抜ける。
    //   ・Running → 子がまだ進行中、Sequence も Running
    //   ・Failure → この子で失敗、Sequence も Failure
    //   ・Success → 次の子へ
    // すべて Success を返したら、Sequence 全体としても Success。
    const usize n = _children.Size();
    for (usize i = 0; i < n; ++i) {
        BtNode* c = _children[i].Get();
        if (c == nullptr) continue;
        const BtStatus s = c->Tick(blackboard, dt);
        if (s == BtStatus::Running) return BtStatus::Running;
        if (s == BtStatus::Failure) return BtStatus::Failure;
        // Success: 次の子へ
    }
    return BtStatus::Success;
}

// ---- BtAction ----------------------------------------------------------------

BtStatus BtAction::Tick(void* blackboard, f32 dt) noexcept {
    // 関数ポインタ未設定はソフトフェイル: Failure を返して composite が前進する。
    if (_fn == nullptr) return BtStatus::Failure;
    return _fn(blackboard, dt);
}

// ---- BehaviorTree ------------------------------------------------------------

void BehaviorTree::SetRoot(UniquePtr<BtNode> root) noexcept {
    // 旧 root はここで UniquePtr デストラクタにより自動破棄される。
    _root = Move(root);
}

BtStatus BehaviorTree::Tick(void* blackboard, f32 dt) noexcept {
    // root 未設定の tree は「常に失敗」として扱う (composite と同じ規約)。
    if (!_root) return BtStatus::Failure;
    return _root->Tick(blackboard, dt);
}

} // namespace acs::game
