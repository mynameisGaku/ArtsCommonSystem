// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar L — FBehaviorTree (selector / sequence / action)
//
// AI / 敵挙動 / cutscene 分岐 などのために最小構成の Behavior Tree を提供する。
// root が 1 つの FBtNode を持ち、毎フレーム Tick(blackboard, dt) で評価する。
//
// 戻り値セマンティクス:
//
//   ┌─────────────┬──────────────────────────┬──────────────────────────┐
//   │ Composite   │ 子の結果                 │ 自分の結果               │
//   ├─────────────┼──────────────────────────┼──────────────────────────┤
//   │ Selector    │ Success が出るまで進む   │ いずれかが Success       │
//   │  (OR)       │   どれか Running         │   → Success              │
//   │             │   全て Failure           │   Running が出た時点で   │
//   │             │                          │   → Running              │
//   │             │                          │   全て Failure           │
//   │             │                          │   → Failure              │
//   ├─────────────┼──────────────────────────┼──────────────────────────┤
//   │ FSequence    │ Failure が出るまで進む   │ いずれかが Failure       │
//   │  (AND)      │   どれか Running         │   → Failure              │
//   │             │   全て Success           │   Running が出た時点で   │
//   │             │                          │   → Running              │
//   │             │                          │   全て Success           │
//   │             │                          │   → Success              │
//   └─────────────┴──────────────────────────┴──────────────────────────┘
//
//   ※ Selector / FSequence は **stateless tick** (毎呼び出しで先頭から再評価)。
//     "1 フレームに 1 ステップだけ進める" 等の中断保持が必要になった段階で
//     PartialTick 派生を追加する想定だが、本最小実装ではスコープ外。
//
// 設計選択:
//   ・leaf アクションは **関数ポインタ** `EBtStatus(*)(void* bb, f32 dt) noexcept`。
//     `std::function` 不使用 (ACS 規約)。型消去のヒープ確保 / 例外を避けるため。
//     必要なキャプチャは blackboard 経由か `bb` を `Self*` にキャストして取り回す。
//   ・blackboard は `void*` (user 定義の任意構造体)。BT 側で型を持たないことで
//     どのモジュール (Pillar L AI / cutscene / UI 等) からも汎用に使える。
//   ・子ノードは `acs::TUniquePtr<FBtNode>` で所有 (= move-only)。
//     子の所有権は composite が握り、tree の寿命と一体化する。
//   ・FBehaviorTree / 各 Node は **非コピー・非ムーブ**。tree は普通フィールドとして
//     抱えられて Tick されるだけなので、所有権を動かす運用は想定しない。
//     構築は `FBtSelector` を `MakeUnique` で作って `AddChild` で組み立てる。
//
// 使い方:
//   struct EnemyBb { FVec3 pos; bool sees_player; };
//
//   static EBtStatus MoveToPlayer(void* bb, f32 dt) noexcept {
//       auto* e = static_cast<EnemyBb*>(bb);
//       // ... move toward player ...
//       return reached ? EBtStatus::Success : EBtStatus::Running;
//   }
//   static EBtStatus Patrol(void* bb, f32 dt) noexcept { ... }
//
//   class Enemy {
//       acs::game::FBehaviorTree m_Bt;
//       EnemyBb                 m_Bb;
//
//       Enemy() noexcept {
//           // Selector: 「敵が見えたら追跡、見えなければパトロール」
//           auto root  = acs::MakeUnique<acs::game::FBtSelector>();
//           auto chase = acs::MakeUnique<acs::game::FBtSequence>();
//           chase->AddChild(acs::MakeUnique<acs::game::FBtAction>(&SeesPlayer));
//           chase->AddChild(acs::MakeUnique<acs::game::FBtAction>(&MoveToPlayer));
//           root->AddChild(acs::Move(chase));
//           root->AddChild(acs::MakeUnique<acs::game::FBtAction>(&Patrol));
//           m_Bt.SetRoot(acs::Move(root));
//       }
//       void Tick(f32 dt) noexcept { m_Bt.Tick(&m_Bb, dt); }
//   };
#pragma once

#include "foundation/Types.h"
#include "foundation/Move.h"
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
 * 全 BT ノードの抽象基底。
 *
 * @details tick は noexcept、blackboard は void* で型を持たない。
 */
class FBtNode {
public:
    /** 空のノードを構築する。 */
    FBtNode() noexcept = default;

    /** 派生ノードを正しく破棄するための仮想デストラクタ。 */
    virtual ~FBtNode() noexcept = default;

    /** コピー禁止 (ノードは TUniquePtr で単独所有するため)。 */
    FBtNode(const FBtNode&)            = delete;

    /** コピー代入も禁止。 */
    FBtNode& operator=(const FBtNode&) = delete;

    /** ムーブ禁止。 */
    FBtNode(FBtNode&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FBtNode& operator=(FBtNode&&)      = delete;

    /**
     * 1 フレーム分の評価を行う。
     *
     * @param blackboard user 定義の状態 (typically Self* にキャスト)。
     * @param dt 前フレームからの経過秒。
     * @return このノードの評価結果。
     */
    virtual EBtStatus Tick(void* blackboard, f32 dt) noexcept = 0;
};

/**
 * 子を順に Tick する "OR" 合成ノード。
 *
 * @details
 * 子を順に Tick し、最初に Running か Success を返した子で停止する。
 * どれかが Success/Running ならそのまま返し、全て Failure なら Failure を返す。
 */
class FBtSelector : public FBtNode {
public:
    /** 空の selector を構築する。 */
    FBtSelector() noexcept = default;

    /** 破棄する (子は TUniquePtr が解放)。 */
    ~FBtSelector() noexcept override = default;

    /**
     * 子の所有権を奪って末尾に追加する。
     *
     * @param child 追加する子ノード (nullptr 渡しは no-op、ソフトフェイル)。
     */
    void AddChild(TUniquePtr<FBtNode> child) noexcept;

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
    TArray<TUniquePtr<FBtNode>> m_Children;
};

/**
 * 子を順に Tick する "AND" 合成ノード。
 *
 * @details
 * 子を順に Tick し、最初に Running か Failure を返した子で停止する。
 * どれかが Failure/Running ならそのまま返し、全て Success なら Success を返す。
 */
class FBtSequence : public FBtNode {
public:
    /** 空の sequence を構築する。 */
    FBtSequence() noexcept = default;

    /** 破棄する (子は TUniquePtr が解放)。 */
    ~FBtSequence() noexcept override = default;

    /**
     * 子の所有権を奪って末尾に追加する。
     *
     * @param child 追加する子ノード (nullptr 渡しは no-op、ソフトフェイル)。
     */
    void AddChild(TUniquePtr<FBtNode> child) noexcept;

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
    TArray<TUniquePtr<FBtNode>> m_Children;
};

/**
 * 関数ポインタを呼ぶだけの末端 leaf ノード。
 *
 * @details
 * std::function 不使用 (ACS 規約)。状態を持ちたい場合は blackboard に置く。
 * fn が nullptr の場合は常に Failure を返す (ソフトフェイル)。
 */
class FBtAction : public FBtNode {
public:
    /** leaf が呼ぶ評価関数の型。 */
    using Fn = EBtStatus(*)(void* blackboard, f32 dt) noexcept;

    /**
     * 評価関数を持つ action を構築する。
     *
     * @param fn 毎 Tick で呼ぶ評価関数 (nullptr なら常に Failure)。
     */
    explicit FBtAction(Fn fn) noexcept : m_Fn(fn) {}

    /** 破棄する。 */
    ~FBtAction() noexcept override = default;

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

/**
 * root を抱えて Tick を駆動するだけのハーネス。
 *
 * @details
 * 非コピー・非ムーブ。Scene / Actor のメンバとして固定の場所に置く想定。
 */
class FBehaviorTree {
public:
    /** 空の tree を構築する (root は SetRoot で設定)。 */
    FBehaviorTree() noexcept = default;

    /** 破棄する (root は TUniquePtr が解放)。 */
    ~FBehaviorTree() noexcept = default;

    /** コピー禁止。 */
    FBehaviorTree(const FBehaviorTree&)            = delete;

    /** コピー代入も禁止。 */
    FBehaviorTree& operator=(const FBehaviorTree&) = delete;

    /** ムーブ禁止。 */
    FBehaviorTree(FBehaviorTree&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FBehaviorTree& operator=(FBehaviorTree&&)      = delete;

    /**
     * root を差し替える。
     *
     * @details 古い root はここで破棄される (TUniquePtr デストラクタ)。
     * @param root 新しい root ノード (nullptr 渡しで tree を空にできる)。
     */
    void SetRoot(TUniquePtr<FBtNode> root) noexcept;

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
    TUniquePtr<FBtNode> m_Root;
};

} // namespace acs::game
