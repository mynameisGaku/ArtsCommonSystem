// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — btedit / bake 用ガードノード
//
// エディタのグラフ (メタミラー) を「実行可能な FBehaviorTree」に焼く (bake) とき、
// composite / transform-decorator / action は core ランタイムノード
// (FBtSelector / FBtSequence / FBtDecorator / FBtAction) にそのまま対応するが、
// Condition / Compare デコレーターには core 側に対応物が無い。そこで btedit 側に
// FBtNode のサブクラスとして「条件ガードノード」を定義する。これらは core FBtNode を
// 継承するので、core の composite と混在した 1 本のツリーを構成できる。
//
//   ABtConditionNode : 条件 bool 関数が true のときだけ子を実行 (= Condition デコレーター)
//   ABtCompareNode   : 動的ブラックボード変数の比較が true のときだけ子を実行 (= Compare デコレーター)
//
// blackboard は FBtBlackboard* 前提 (bake は動的ブラックボードモデル)。core 層に
// FBtBlackboard 依存を持ち込まないため、これらは btedit 層に置く。
#pragma once

#include "gameframework/BehaviorTree.h"
#include "gameframework/tools/btedit/BtCatalog.h"   // CBtConditionRegistry::Fn / FBtBlackboard
#include "memory/UniquePtr.h"

#include <cstdio>    // std::snprintf

namespace acs::game::btedit {

class ABtConditionNode;
using FBtConditionNode = ABtConditionNode;

/**
 * 条件 bool 関数で子をガードする FBtNode (bake された Condition デコレーター)。
 *
 * @details fn(bb) が true のときだけ子を Tick し、その結果を返す。false (または fn 未設定)
 *          なら子を実行せず Failure。
 */
class ABtConditionNode : public FBtNode {
public:
    ACS_RTTI(FBtConditionNode, FBtNode)

    /** 条件関数の型 (CBtConditionRegistry と同型: bool(*)(void*) noexcept)。 */
    using Fn = CBtConditionRegistry::Fn;

    /**
     * 条件関数を指定して構築する。
     *
     * @param fn 評価する条件関数 (nullptr なら常に Failure)。
     */
    explicit ABtConditionNode(Fn fn) noexcept : m_Fn(fn) {}

    /** 破棄する (子は TUniquePtr が解放)。 */
    ~ABtConditionNode() noexcept override = default;

    /** ガードする子を設定する。 */
    void SetChild(TUniquePtr<FBtNode> child) noexcept { m_Child = Move(child); }

    /** 条件 true のときだけ子を Tick して返す (false / 子なし / fn 未設定は Failure)。 */
    EBtStatus Tick(void* blackboard, f32 dt) noexcept override {
        if (m_Fn == nullptr || !m_Fn(blackboard)) return EBtStatus::Failure;
        return m_Child ? m_Child->Tick(blackboard, dt) : EBtStatus::Failure;
    }

private:
    /** 条件関数。 */
    Fn                  m_Fn;

    /** ガードされる子ノード。 */
    TUniquePtr<FBtNode> m_Child;
};

class ABtCompareNode;
using FBtCompareNode = ABtCompareNode;

/**
 * 変数と定数の比較で子をガードする FBtNode (bake された Compare デコレーター)。
 *
 * @details
 * editor インタプリタと同じ 2 つの変数解決モデルを bake 時に固定する:
 *   ・dynamic モード … blackboard を FBtBlackboard* とみなし、変数「名」で値を引く
 *     (FBtBlackboard モデル)。変数が無ければ Failure。
 *   ・schema モード  … blackboard を raw 構造体とみなし、bake 時に解決した
 *     「オフセット+型」で BtCompareVar により読む (FBtBlackboardSchema モデル)。
 * どちらのモードを使うかは bake 時 (BuildRuntimeNode) に変数の解決先で決める。これにより
 * 「schema 変数を使った Compare が bake すると常に Failure / 不正キャスト」になる乖離を防ぐ。
 * 注意: 1 本の baked ツリーは単一の blackboard モデルで tick すること
 * (dynamic なら FBtBlackboard、schema なら対応する raw 構造体)。
 */
class ABtCompareNode : public FBtNode {
public:
    ACS_RTTI(FBtCompareNode, FBtNode)

    /**
     * dynamic モードで構築する (FBtBlackboard を変数名で引く)。
     *
     * @param var 比較する変数名 (FBtBlackboard のキー)。
     * @param op 比較演算子。
     * @param rhs 比較定数 (右辺)。
     */
    ABtCompareNode(const char* var, EBtCompareOp op, f32 rhs) noexcept
        : m_Op(op), m_Rhs(rhs), m_UseSchema(false), m_Offset(0u), m_Type(EBtVarType::F32) {
        std::snprintf(m_Var, sizeof(m_Var), "%s", (var != nullptr) ? var : "");
    }

    /**
     * schema モードで構築する (raw 構造体を offset+type で読む)。
     *
     * @param offset blackboard 先頭からのバイトオフセット。
     * @param type 読み取る型。
     * @param op 比較演算子。
     * @param rhs 比較定数 (右辺)。
     */
    ABtCompareNode(u32 offset, EBtVarType type, EBtCompareOp op, f32 rhs) noexcept
        : m_Op(op), m_Rhs(rhs), m_UseSchema(true), m_Offset(offset), m_Type(type) {
        m_Var[0] = '\0';
    }

    /** 破棄する (子は TUniquePtr が解放)。 */
    ~ABtCompareNode() noexcept override = default;

    /** ガードする子を設定する。 */
    void SetChild(TUniquePtr<FBtNode> child) noexcept { m_Child = Move(child); }

    /** 比較 true のときだけ子を Tick して返す (false / 子なしは Failure)。 */
    EBtStatus Tick(void* blackboard, f32 dt) noexcept override {
        bool pass;
        if (m_UseSchema) {
            // raw 構造体 + offset 読み (FBtBlackboard へのキャストはしない)。
            pass = BtCompareVar(blackboard, m_Offset, m_Type, m_Op, m_Rhs);
        } else {
            auto* board = static_cast<FBtBlackboard*>(blackboard);
            pass = (board != nullptr) && board->Has(m_Var)
                   && BtCompareF32(board->GetAsF32(m_Var), m_Op, m_Rhs);
        }
        if (!pass) return EBtStatus::Failure;
        return m_Child ? m_Child->Tick(blackboard, dt) : EBtStatus::Failure;
    }

private:
    /** 比較する変数名 (dynamic モードで使用)。 */
    char                m_Var[48] = {};

    /** 比較演算子。 */
    EBtCompareOp        m_Op;

    /** 比較定数 (右辺)。 */
    f32                 m_Rhs;

    /** true=schema モード (offset+type 読み) / false=dynamic モード (名前読み)。 */
    bool                m_UseSchema;

    /** schema モードのバイトオフセット。 */
    u32                 m_Offset;

    /** schema モードの読み取り型。 */
    EBtVarType          m_Type;

    /** ガードされる子ノード。 */
    TUniquePtr<FBtNode> m_Child;
};

} // namespace acs::game::btedit
