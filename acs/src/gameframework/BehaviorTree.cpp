// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar L — CBehaviorTree 実装
//
// Selector / FSequence のループはどちらも「子を順に Tick し、ある条件で
// 早期 return、最後まで通り抜けたら反対の結論を返す」という対称形をしている。
// 表 (ヘッダ参照) のとおり、停止条件と最終 fallthrough が真逆なだけ。
#include "gameframework/BehaviorTree.h"
#include "foundation/Move.h"

namespace acs::game {

/** 子の所有権を奪って末尾に追加する (nullptr は静かに無視)。 */
void ABtSelector::AddChild(TUniquePtr<ABtNode> child) noexcept {
    // nullptr 子はそもそも tick できないので追加しない (静かに無視)。
    if (!child) return;
    m_Children.PushBack(Move(child));
}

/** 子を順に Tick し、最初の Running/Success で抜ける (全 Failure なら Failure)。 */
EBtStatus ABtSelector::Tick(void* blackboard, f32 dt) noexcept {
    // OR セマンティクス: Running か Success を見つけたらその時点で抜ける。
    //   ・Running → 子がまだ進行中、Selector も Running を伝播
    //   ・Success → この子で目的達成、Selector も Success
    //   ・Failure → 次の子へ
    // すべて Failure を返したら、Selector 全体としても Failure。
    const usize n = m_Children.Size();
    for (usize i = 0; i < n; ++i) {
        ABtNode* c = m_Children[i].Get();
        if (c == nullptr) continue;            // 万一の null 安全 (本来 AddChild で弾く)
        const EBtStatus s = c->Tick(blackboard, dt);
        if (s == EBtStatus::Running) return EBtStatus::Running;
        if (s == EBtStatus::Success) return EBtStatus::Success;
        // Failure: 次の子へ
    }
    return EBtStatus::Failure;
}

/** 子の所有権を奪って末尾に追加する (nullptr は静かに無視)。 */
void ABtSequence::AddChild(TUniquePtr<ABtNode> child) noexcept {
    if (!child) return;
    m_Children.PushBack(Move(child));
}

/** 子を順に Tick し、最初の Running/Failure で抜ける (全 Success なら Success)。 */
EBtStatus ABtSequence::Tick(void* blackboard, f32 dt) noexcept {
    // AND セマンティクス: Running か Failure を見つけたらその時点で抜ける。
    //   ・Running → 子がまだ進行中、FSequence も Running
    //   ・Failure → この子で失敗、FSequence も Failure
    //   ・Success → 次の子へ
    // すべて Success を返したら、FSequence 全体としても Success。
    const usize n = m_Children.Size();
    for (usize i = 0; i < n; ++i) {
        ABtNode* c = m_Children[i].Get();
        if (c == nullptr) continue;
        const EBtStatus s = c->Tick(blackboard, dt);
        if (s == EBtStatus::Running) return EBtStatus::Running;
        if (s == EBtStatus::Failure) return EBtStatus::Failure;
        // Success: 次の子へ
    }
    return EBtStatus::Success;
}

/** 保持する関数ポインタを呼ぶ (未設定ならソフトフェイルで Failure)。 */
EBtStatus ABtAction::Tick(void* blackboard, f32 dt) noexcept {
    // 関数ポインタ未設定はソフトフェイル: Failure を返して composite が前進する。
    if (m_Fn == nullptr) return EBtStatus::Failure;
    return m_Fn(blackboard, dt);
}

/** 子の status に decorator op を適用する (Running は全 op 素通し)。 */
EBtStatus ApplyDecorator(EBtDecoratorOp op, EBtStatus child) noexcept {
    // Running はどの op でも素通し: 子がまだ進行中なら decorator も進行中。
    if (child == EBtStatus::Running) return EBtStatus::Running;
    switch (op) {
        case EBtDecoratorOp::Inverter:
            return (child == EBtStatus::Success) ? EBtStatus::Failure : EBtStatus::Success;
        case EBtDecoratorOp::ForceSuccess:
            return EBtStatus::Success;
        case EBtDecoratorOp::ForceFailure:
            return EBtStatus::Failure;
        case EBtDecoratorOp::Repeat:
            // 子 Success → Running に変えて次フレームも回す (Failure はそのまま停止)。
            return (child == EBtStatus::Success) ? EBtStatus::Running : EBtStatus::Failure;
    }
    return child; // 到達不能
}

/** f32 同士を比較演算子で比べる。 */
bool BtCompareF32(f32 lhs, EBtCompareOp op, f32 rhs) noexcept {
    switch (op) {
        case EBtCompareOp::Less:      return lhs <  rhs;
        case EBtCompareOp::LessEq:    return lhs <= rhs;
        case EBtCompareOp::Equal:     return lhs == rhs;
        case EBtCompareOp::NotEqual:  return lhs != rhs;
        case EBtCompareOp::GreaterEq: return lhs >= rhs;
        case EBtCompareOp::Greater:   return lhs >  rhs;
    }
    return false; // 到達不能
}

/** blackboard の指定フィールドを定数と比較する (bb null は false)。 */
bool BtCompareVar(const void* bb, u32 offset, EBtVarType type, EBtCompareOp op, f32 rhs) noexcept {
    if (bb == nullptr) return false;
    const char* base = static_cast<const char*>(bb) + offset;

    // フィールドを f32 に正規化して読み出す (bool/i32/f32 の 3 型)。
    f32 lhs = 0.0f;
    switch (type) {
        case EBtVarType::Bool: lhs = (*reinterpret_cast<const bool*>(base)) ? 1.0f : 0.0f; break;
        case EBtVarType::I32:  lhs = static_cast<f32>(*reinterpret_cast<const i32*>(base)); break;
        case EBtVarType::F32:  lhs = *reinterpret_cast<const f32*>(base); break;
    }
    return BtCompareF32(lhs, op, rhs);
}

/** 装飾する子を差し替える (既存の子は move 代入で破棄)。 */
void ABtDecorator::SetChild(TUniquePtr<ABtNode> child) noexcept {
    m_Child = Move(child);
}

/** 子を Tick し、op の変換を施して返す (子未設定なら Failure)。 */
EBtStatus ABtDecorator::Tick(void* blackboard, f32 dt) noexcept {
    // 装飾対象が無ければソフトフェイル (composite が前進できる)。
    if (!m_Child) return EBtStatus::Failure;
    return ApplyDecorator(m_Op, m_Child->Tick(blackboard, dt));
}

/** root を差し替える (旧 root は TUniquePtr デストラクタで自動破棄)。 */
void CBehaviorTree::SetRoot(TUniquePtr<ABtNode> root) noexcept {
    // 旧 root はここで TUniquePtr デストラクタにより自動破棄される。
    m_Root = Move(root);
}

/** root を 1 フレーム評価する (root 未設定なら Failure)。 */
EBtStatus CBehaviorTree::Tick(void* blackboard, f32 dt) noexcept {
    // root 未設定の tree は「常に失敗」として扱う (composite と同じ規約)。
    if (!m_Root) return EBtStatus::Failure;
    return m_Root->Tick(blackboard, dt);
}

} // namespace acs::game
