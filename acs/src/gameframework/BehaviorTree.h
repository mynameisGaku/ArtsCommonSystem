// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar L — CBehaviorTree (selector / sequence / action)
//
// AI / 敵挙動 / cutscene 分岐 などのために最小構成の Behavior Tree を提供する。
// root が 1 つの ABtNode を持ち、毎フレーム Tick(blackboard, dt) で評価する。
//
// 戻り値セマンティクス:
//
//   ┌─────────────┬──────────────────────────┬──────────────────────────┐
//   │ Composite   │ 子の結果                 │ 自分の結果               │
//   ├─────────────┼──────────────────────────┼──────────────────────────┤
//   │ ABtSelector │ Success が出るまで進む   │ いずれかが Success       │
//   │  (OR)       │   どれか Running         │   → Success              │
//   │             │   全て Failure           │   Running が出た時点で   │
//   │             │                          │   → Running              │
//   │             │                          │   全て Failure           │
//   │             │                          │   → Failure              │
//   ├─────────────┼──────────────────────────┼──────────────────────────┤
//   │ ABtSequence │ Failure が出るまで進む   │ いずれかが Failure       │
//   │  (AND)      │   どれか Running         │   → Failure              │
//   │             │   全て Success           │   Running が出た時点で   │
//   │             │                          │   → Running              │
//   │             │                          │   全て Success           │
//   │             │                          │   → Success              │
//   └─────────────┴──────────────────────────┴──────────────────────────┘
//
//   ※ ABtSelector / ABtSequence は **stateless tick** (毎呼び出しで先頭から再評価)。
//     "1 フレームに 1 ステップだけ進める" 等の中断保持が必要になった段階で
//     PartialTick 派生を追加する想定だが、本最小実装ではスコープ外。
//
// 設計選択:
//   ・leaf アクションは **関数ポインタ** `EBtStatus(*)(void* bb, f32 dt) noexcept`。
//     `std::function` 不使用 (ACS 規約)。型消去のヒープ確保 / 例外を避けるため。
//     必要なキャプチャは blackboard 経由か `bb` を `Self*` にキャストして取り回す。
//   ・blackboard は `void*` (user 定義の任意構造体)。BT 側で型を持たないことで
//     どのモジュール (Pillar L AI / cutscene / UI 等) からも汎用に使える。
//   ・子ノードは `acs::TUniquePtr<ABtNode>` で所有 (= move-only)。
//     子の所有権は composite が握り、tree の寿命と一体化する。
//   ・CBehaviorTree / 各 ABtNode は **非コピー・非ムーブ**。tree は普通フィールドとして
//     抱えられて Tick されるだけなので、所有権を動かす運用は想定しない。
//     構築は `ABtSelector` を `MakeUnique` で作って `AddChild` で組み立てる。
//
// 使い方:
//   struct FEnemyBb { FVec3 pos; bool sees_player; };
//
//   static EBtStatus MoveToPlayer(void* bb, f32 dt) noexcept {
//       auto* e = static_cast<FEnemyBb*>(bb);
//       // ... move toward player ...
//       return reached ? EBtStatus::Success : EBtStatus::Running;
//   }
//   static EBtStatus Patrol(void* bb, f32 dt) noexcept { ... }
//
//   class FEnemy {
//       acs::game::CBehaviorTree m_Bt;
//       FEnemyBb                m_Bb;
//
//       FEnemy() noexcept {
//           // Selector: 「敵が見えたら追跡、見えなければパトロール」
//           auto root  = acs::MakeUnique<acs::game::ABtSelector>();
//           auto chase = acs::MakeUnique<acs::game::ABtSequence>();
//           chase->AddChild(acs::MakeUnique<acs::game::ABtAction>(&SeesPlayer));
//           chase->AddChild(acs::MakeUnique<acs::game::ABtAction>(&MoveToPlayer));
//           root->AddChild(acs::Move(chase));
//           root->AddChild(acs::MakeUnique<acs::game::ABtAction>(&Patrol));
//           m_Bt.SetRoot(acs::Move(root));
//       }
//       void Tick(f32 dt) noexcept { m_Bt.Tick(&m_Bb, dt); }
//   };
#pragma once

#include "foundation/Types.h"
#include "foundation/Move.h"
#include "foundation/Cast.h"      // ACS_RTTI_* / Cast<T> (RTTI 不使用のノード型判定)
#include "memory/UniquePtr.h"
#include "container/Array.h"

namespace acs::game {

/**
 * ノード Tick の戻り値。
 */
enum class EBtStatus : u8 {
    /** まだ実行中 (次フレームに継続)。 */
    Running = 0,

    /** このノードは目的達成 (composite が次へ進める判断材料)。 */
    Success = 1,

    /** このノードは失敗 (composite が次へ進める判断材料)。 */
    Failure = 2,
};

/**
 * ABtDecorator が子の結果をどう変換するか。
 *
 * @details
 * decorator は子を 1 つだけ持ち、その Tick 結果に下記の変換を施す。Running は
 * いずれの op でも素通し (子がまだ進行中なら decorator も進行中) する規約で統一する。
 * editor 側 (btedit) のメタミラーも本 enum を共有する (= 単一の真実点)。
 */
enum class EBtDecoratorOp : u8 {
    /** 子の Success↔Failure を反転する (Running は素通し)。条件の否定に使う。 */
    Inverter     = 0,

    /** 子が Failure でも Success に倒す (Running は素通し)。任意分岐を必ず成功扱い。 */
    ForceSuccess = 1,

    /** 子が Success でも Failure に倒す (Running は素通し)。常に失敗させる。 */
    ForceFailure = 2,

    /** 子が Success を返したら Running に変えて繰り返させる (Failure/Running は素通し)。 */
    Repeat       = 3,
};

/**
 * 子の status に decorator op を適用する純関数 (Running は全 op 素通し)。
 *
 * @details
 * ABtDecorator::Tick と editor (btedit) のグラフインタプリタが同じ意味論を共有する
 * ための単一実装。runtime とエディタの decorator 挙動が乖離しないことを保証する。
 * @param op 適用する変換種別。
 * @param child 子ノードの Tick 結果。
 * @return 変換後の status。
 */
EBtStatus ApplyDecorator(EBtDecoratorOp op, EBtStatus child) noexcept;

/**
 * blackboard 変数の型 (no-code 比較条件 / スキーマで使う基本スカラ型)。
 *
 * @details
 * ブラックボード上のフィールドをエディタから「名前 + 型 + オフセット」で参照し、
 * 値を f32 に正規化して定数と比較するための最小型集合。pointer/struct は対象外。
 */
enum class EBtVarType : u8 {
    /** bool (1 byte)。0=false / 非0=true。 */
    Bool = 0,
    /** 32bit 符号付き整数。 */
    I32  = 1,
    /** 32bit 浮動小数。 */
    F32  = 2,
};

/**
 * 比較条件の演算子 (variable <op> constant)。
 */
enum class EBtCompareOp : u8 {
    Less      = 0, /**< <  */
    LessEq    = 1, /**< <= */
    Equal     = 2, /**< == */
    NotEqual  = 3, /**< != */
    GreaterEq = 4, /**< >= */
    Greater   = 5, /**< >  */
};

/**
 * blackboard の指定フィールドを定数と比較する純関数 (no-code 比較条件の評価本体)。
 *
 * @details
 * `bb` を char* とみなして `offset` バイト先を `type` で読み、f32 に正規化して
 * `rhs` と `op` で比較する。bb が null の場合は false。Equal/NotEqual は f32 では
 * 厳密一致になりがちな点に注意 (整数/bool 比較で使う想定)。editor インタプリタと
 * (将来の) runtime 条件ノードが同じ意味論を共有するための単一実装。
 * @param bb     ブラックボード先頭ポインタ (user 構造体)。
 * @param offset bb 先頭からのバイトオフセット。
 * @param type   読み取る型。
 * @param op     比較演算子。
 * @param rhs    比較対象の定数 (f32 に正規化済み)。
 * @return 比較結果 (bb が null なら false)。
 */
bool BtCompareVar(const void* bb, u32 offset, EBtVarType type, EBtCompareOp op, f32 rhs) noexcept;

/**
 * f32 同士を比較演算子で比べる純関数 (比較条件のスカラ比較本体)。
 *
 * @details BtCompareVar と editor の動的ブラックボード比較が共有する単一実装。
 * @param lhs 左辺 (変数値を f32 に正規化したもの)。
 * @param op  比較演算子。
 * @param rhs 右辺の定数。
 * @return 比較結果。
 */
bool BtCompareF32(f32 lhs, EBtCompareOp op, f32 rhs) noexcept;

class ABtNode;

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FBtNode = ABtNode;

/**
 * 全 BT ノードの抽象基底。
 *
 * @details tick は noexcept、blackboard は void* で型を持たない。
 */
class ABtNode {
public:
    /** 空のノードを構築する。 */
    ABtNode() noexcept = default;

    /** 派生ノードを正しく破棄するための仮想デストラクタ。 */
    virtual ~ABtNode() noexcept = default;

    /** コピー禁止 (ノードは TUniquePtr で単独所有するため)。 */
    ABtNode(const ABtNode&)            = delete;

    /** コピー代入も禁止。 */
    ABtNode& operator=(const ABtNode&) = delete;

    /** ムーブ禁止。 */
    ABtNode(ABtNode&&)                 = delete;

    /** ムーブ代入も禁止。 */
    ABtNode& operator=(ABtNode&&)      = delete;

    // RTTI 不使用の型判定 (Cast<ABtSelector>(node) 等を可能にする)。階層のルート。
    ACS_RTTI_ROOT(FBtNode)

    /**
     * 1 フレーム分の評価を行う。
     *
     * @param blackboard user 定義の状態 (typically Self* にキャスト)。
     * @param dt 前フレームからの経過秒。
     * @return このノードの評価結果。
     */
    virtual EBtStatus Tick(void* blackboard, f32 dt) noexcept = 0;
};

class ABtSelector;

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FBtSelector = ABtSelector;

/**
 * 子を順に Tick する "OR" 合成ノード。
 *
 * @details
 * 子を順に Tick し、最初に Running か Success を返した子で停止する。
 * どれかが Success/Running ならそのまま返し、全て Failure なら Failure を返す。
 */
class ABtSelector : public ABtNode {
public:
    ACS_RTTI(FBtSelector, FBtNode)

    /** 空の selector を構築する。 */
    ABtSelector() noexcept = default;

    /** 破棄する (子は TUniquePtr が解放)。 */
    ~ABtSelector() noexcept override = default;

    /**
     * 子の所有権を奪って末尾に追加する。
     *
     * @param child 追加する子ノード (nullptr 渡しは no-op、ソフトフェイル)。
     */
    void AddChild(TUniquePtr<ABtNode> child) noexcept;

    /**
     * 子を順に評価して OR 合成の結果を返す。
     *
     * @param blackboard user 定義の状態。
     * @param dt 前フレームからの経過秒。
     * @return いずれかが Success/Running ならそれ、全 Failure なら Failure。
     */
    EBtStatus Tick(void* blackboard, f32 dt) noexcept override;

    /**
     * 子の数を返す。
     *
     * @return 追加済みの子ノード数。
     */
    usize ChildCount() const noexcept { return m_Children.Size(); }

private:
    /** 子ノード (所有権を持つ)。 */
    TArray<TUniquePtr<ABtNode>> m_Children;
};

class ABtSequence;

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FBtSequence = ABtSequence;

/**
 * 子を順に Tick する "AND" 合成ノード。
 *
 * @details
 * 子を順に Tick し、最初に Running か Failure を返した子で停止する。
 * どれかが Failure/Running ならそのまま返し、全て Success なら Success を返す。
 */
class ABtSequence : public ABtNode {
public:
    ACS_RTTI(FBtSequence, FBtNode)

    /** 空の sequence を構築する。 */
    ABtSequence() noexcept = default;

    /** 破棄する (子は TUniquePtr が解放)。 */
    ~ABtSequence() noexcept override = default;

    /**
     * 子の所有権を奪って末尾に追加する。
     *
     * @param child 追加する子ノード (nullptr 渡しは no-op、ソフトフェイル)。
     */
    void AddChild(TUniquePtr<ABtNode> child) noexcept;

    /**
     * 子を順に評価して AND 合成の結果を返す。
     *
     * @param blackboard user 定義の状態。
     * @param dt 前フレームからの経過秒。
     * @return いずれかが Failure/Running ならそれ、全 Success なら Success。
     */
    EBtStatus Tick(void* blackboard, f32 dt) noexcept override;

    /**
     * 子の数を返す。
     *
     * @return 追加済みの子ノード数。
     */
    usize ChildCount() const noexcept { return m_Children.Size(); }

private:
    /** 子ノード (所有権を持つ)。 */
    TArray<TUniquePtr<ABtNode>> m_Children;
};

class ABtAction;

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FBtAction = ABtAction;

/**
 * 関数ポインタを呼ぶだけの末端 leaf ノード。
 *
 * @details
 * std::function 不使用 (ACS 規約)。状態を持ちたい場合は blackboard に置く。
 * fn が nullptr の場合は常に Failure を返す (ソフトフェイル)。
 */
class ABtAction : public ABtNode {
public:
    ACS_RTTI(FBtAction, FBtNode)

    /** leaf が呼ぶ評価関数の型。 */
    using Fn = EBtStatus(*)(void* blackboard, f32 dt) noexcept;

    /**
     * 評価関数を持つ action を構築する。
     *
     * @param fn 毎 Tick で呼ぶ評価関数 (nullptr なら常に Failure)。
     */
    explicit ABtAction(Fn fn) noexcept : m_Fn(fn) {}

    /** 破棄する。 */
    ~ABtAction() noexcept override = default;

    /**
     * 保持する関数ポインタを呼ぶ。
     *
     * @param blackboard user 定義の状態。
     * @param dt 前フレームからの経過秒。
     * @return fn の戻り値 (fn が nullptr なら Failure)。
     */
    EBtStatus Tick(void* blackboard, f32 dt) noexcept override;

private:
    /** 評価に使う関数ポインタ (未設定なら nullptr)。 */
    Fn m_Fn = nullptr;
};

class ABtDecorator;

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FBtDecorator = ABtDecorator;

/**
 * 子を 1 つだけ持ち、その結果を EBtDecoratorOp で変換する装飾ノード。
 *
 * @details
 * Inverter / ForceSuccess / ForceFailure / Repeat の 4 種を提供する。Running は
 * いずれの op でも素通しする (子が進行中なら decorator も進行中)。子が未設定の
 * 場合は Failure を返す (= 装飾対象が無いのでソフトフェイル)。子の所有権は
 * decorator が握る (SetChild で move-in、既存の子は破棄して差し替え)。
 */
class ABtDecorator : public ABtNode {
public:
    ACS_RTTI(FBtDecorator, FBtNode)

    /**
     * 変換 op を指定して decorator を構築する。
     *
     * @param op 子の結果に施す変換種別。
     */
    explicit ABtDecorator(EBtDecoratorOp op) noexcept : m_Op(op) {}

    /** 破棄する (子は TUniquePtr が解放)。 */
    ~ABtDecorator() noexcept override = default;

    /**
     * 装飾する子を設定する (既存の子は破棄して差し替え)。
     *
     * @param child 装飾対象の子ノード (nullptr で子を外す)。
     */
    void SetChild(TUniquePtr<ABtNode> child) noexcept;

    /**
     * 現在の変換 op を差し替える。
     *
     * @param op 新しい変換種別。
     */
    void SetOp(EBtDecoratorOp op) noexcept { m_Op = op; }

    /**
     * 子を Tick し、結果に op の変換を施して返す。
     *
     * @param blackboard user 定義の状態。
     * @param dt 前フレームからの経過秒。
     * @return 変換後の status (子が未設定なら Failure)。
     */
    EBtStatus Tick(void* blackboard, f32 dt) noexcept override;

    /** 子が設定済みかを返す。 */
    bool HasChild() const noexcept { return static_cast<bool>(m_Child); }

private:
    /** 変換種別。 */
    EBtDecoratorOp      m_Op;

    /** 装飾する子ノード (所有権を持つ)。 */
    TUniquePtr<ABtNode> m_Child;
};

/**
 * root を抱えて Tick を駆動するだけのハーネス。
 *
 * @details
 * 非コピー・非ムーブ。AScene / Actor のメンバとして固定の場所に置く想定。
 */
class CBehaviorTree {
public:
    /** 空の tree を構築する (root は SetRoot で設定)。 */
    CBehaviorTree() noexcept = default;

    /** 破棄する (root は TUniquePtr が解放)。 */
    ~CBehaviorTree() noexcept = default;

    /** コピー禁止。 */
    CBehaviorTree(const CBehaviorTree&)            = delete;

    /** コピー代入も禁止。 */
    CBehaviorTree& operator=(const CBehaviorTree&) = delete;

    /** ムーブ禁止。 */
    CBehaviorTree(CBehaviorTree&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CBehaviorTree& operator=(CBehaviorTree&&)      = delete;

    /**
     * root を差し替える。
     *
     * @details 古い root はここで破棄される (TUniquePtr デストラクタ)。
     * @param root 新しい root ノード (nullptr 渡しで tree を空にできる)。
     */
    void SetRoot(TUniquePtr<ABtNode> root) noexcept;

    /**
     * root を 1 フレーム分評価する。
     *
     * @param blackboard user 定義の状態。
     * @param dt 前フレームからの経過秒。
     * @return root の評価結果 (root 未設定なら Failure)。
     */
    EBtStatus Tick(void* blackboard, f32 dt) noexcept;

    /**
     * root が設定済みかを返す。
     *
     * @return root が非 null なら true。
     */
    bool HasRoot() const noexcept { return static_cast<bool>(m_Root); }

private:
    /** tree の root ノード (所有権を持つ)。 */
    TUniquePtr<ABtNode> m_Root;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FBehaviorTree = CBehaviorTree;

} // namespace acs::game
