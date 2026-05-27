// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar L — FBehaviorTree 実装
//
// Selector / FSequence のループはどちらも「子を順に Tick し、ある条件で
// 早期 return、最後まで通り抜けたら反対の結論を返す」という対称形をしている。
// 表 (ヘッダ参照) のとおり、停止条件と最終 fallthrough が真逆なだけ。
#include "gameframework/BehaviorTree.h"
#include "foundation/Move.h"

namespace acs::game {

// ---- FBtSelector --------------------------------------------------------------

void FBtSelector::AddChild(TUniquePtr<FBtNode> child) noexcept {
    // nullptr 子はそもそも tick できないので追加しない (静かに無視)。
    if (!child) return;
    m_Children.PushBack(Move(child));
}

EBtStatus FBtSelector::Tick(void* blackboard, f32 dt) noexcept {
    // OR セマンティクス: Running か Success を見つけたらその時点で抜ける。
    //   ・Running → 子がまだ進行中、Selector も Running を伝播
    //   ・Success → この子で目的達成、Selector も Success
    //   ・Failure → 次の子へ
    // すべて Failure を返したら、Selector 全体としても Failure。
    const usize n = m_Children.Size();
    for (usize i = 0; i < n; ++i) {
        FBtNode* c = m_Children[i].Get();
        if (c == nullptr) continue;            // 万一の null 安全 (本来 AddChild で弾く)
        const EBtStatus s = c->Tick(blackboard, dt);
        if (s == EBtStatus::Running) return EBtStatus::Running;
        if (s == EBtStatus::Success) return EBtStatus::Success;
        // Failure: 次の子へ
    }
    return EBtStatus::Failure;
}

// ---- FBtSequence --------------------------------------------------------------

void FBtSequence::AddChild(TUniquePtr<FBtNode> child) noexcept {
    if (!child) return;
    m_Children.PushBack(Move(child));
}

EBtStatus FBtSequence::Tick(void* blackboard, f32 dt) noexcept {
    // AND セマンティクス: Running か Failure を見つけたらその時点で抜ける。
    //   ・Running → 子がまだ進行中、FSequence も Running
    //   ・Failure → この子で失敗、FSequence も Failure
    //   ・Success → 次の子へ
    // すべて Success を返したら、FSequence 全体としても Success。
    const usize n = m_Children.Size();
    for (usize i = 0; i < n; ++i) {
        FBtNode* c = m_Children[i].Get();
        if (c == nullptr) continue;
        const EBtStatus s = c->Tick(blackboard, dt);
        if (s == EBtStatus::Running) return EBtStatus::Running;
        if (s == EBtStatus::Failure) return EBtStatus::Failure;
        // Success: 次の子へ
    }
    return EBtStatus::Success;
}

// ---- FBtAction ----------------------------------------------------------------

EBtStatus FBtAction::Tick(void* blackboard, f32 dt) noexcept {
    // 関数ポインタ未設定はソフトフェイル: Failure を返して composite が前進する。
    if (m_Fn == nullptr) return EBtStatus::Failure;
    return m_Fn(blackboard, dt);
}

// ---- FBehaviorTree ------------------------------------------------------------

void FBehaviorTree::SetRoot(TUniquePtr<FBtNode> root) noexcept {
    // 旧 root はここで TUniquePtr デストラクタにより自動破棄される。
    m_Root = Move(root);
}

EBtStatus FBehaviorTree::Tick(void* blackboard, f32 dt) noexcept {
    // root 未設定の tree は「常に失敗」として扱う (composite と同じ規約)。
    if (!m_Root) return EBtStatus::Failure;
    return m_Root->Tick(blackboard, dt);
}

} // namespace acs::game
