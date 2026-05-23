// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar L — BehaviorTree (selector / sequence / action)
//
// AI / 敵挙動 / cutscene 分岐 などのために最小構成の Behavior Tree を提供する。
// root が 1 つの BtNode を持ち、毎フレーム Tick(blackboard, dt) で評価する。
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
//   │ Sequence    │ Failure が出るまで進む   │ いずれかが Failure       │
//   │  (AND)      │   どれか Running         │   → Failure              │
//   │             │   全て Success           │   Running が出た時点で   │
//   │             │                          │   → Running              │
//   │             │                          │   全て Success           │
//   │             │                          │   → Success              │
//   └─────────────┴──────────────────────────┴──────────────────────────┘
//
//   ※ Selector / Sequence は **stateless tick** (毎呼び出しで先頭から再評価)。
//     "1 フレームに 1 ステップだけ進める" 等の中断保持が必要になった段階で
//     PartialTick 派生を追加する想定だが、本最小実装ではスコープ外。
//
// 設計選択:
//   ・leaf アクションは **関数ポインタ** `EBtStatus(*)(void* bb, f32 dt) noexcept`。
//     `std::function` 不使用 (ACS 規約)。型消去のヒープ確保 / 例外を避けるため。
//     必要なキャプチャは blackboard 経由か `bb` を `Self*` にキャストして取り回す。
//   ・blackboard は `void*` (user 定義の任意構造体)。BT 側で型を持たないことで
//     どのモジュール (Pillar L AI / cutscene / UI 等) からも汎用に使える。
//   ・子ノードは `acs::UniquePtr<BtNode>` で所有 (= move-only)。
//     子の所有権は composite が握り、tree の寿命と一体化する。
//   ・BehaviorTree / 各 Node は **非コピー・非ムーブ**。tree は普通フィールドとして
//     抱えられて Tick されるだけなので、所有権を動かす運用は想定しない。
//     構築は `BtSelector` を `MakeUnique` で作って `AddChild` で組み立てる。
//
// 使い方:
//   struct EnemyBb { Vec3 pos; bool sees_player; };
//
//   static EBtStatus MoveToPlayer(void* bb, f32 dt) noexcept {
//       auto* e = static_cast<EnemyBb*>(bb);
//       // ... move toward player ...
//       return reached ? EBtStatus::Success : EBtStatus::Running;
//   }
//   static EBtStatus Patrol(void* bb, f32 dt) noexcept { ... }
//
//   class Enemy {
//       acs::game::BehaviorTree _bt;
//       EnemyBb                 _bb;
//
//       Enemy() noexcept {
//           // Selector: 「敵が見えたら追跡、見えなければパトロール」
//           auto root  = acs::MakeUnique<acs::game::BtSelector>();
//           auto chase = acs::MakeUnique<acs::game::BtSequence>();
//           chase->AddChild(acs::MakeUnique<acs::game::BtAction>(&SeesPlayer));
//           chase->AddChild(acs::MakeUnique<acs::game::BtAction>(&MoveToPlayer));
//           root->AddChild(acs::Move(chase));
//           root->AddChild(acs::MakeUnique<acs::game::BtAction>(&Patrol));
//           _bt.SetRoot(acs::Move(root));
//       }
//       void Tick(f32 dt) noexcept { _bt.Tick(&_bb, dt); }
//   };
#pragma once

#include "foundation/Types.h"
#include "foundation/Move.h"
#include "memory/UniquePtr.h"
#include "container/Array.h"

namespace acs::game {

// ノード Tick の戻り値。
//   Running = まだ実行中 (次フレームに継続)
//   Success = このノードは目的達成 (composite が次へ進める判断材料)
//   Failure = このノードは失敗   (composite が次へ進める判断材料)
enum class EBtStatus : u8 {
    Running = 0,
    Success = 1,
    Failure = 2,
};

// 全 BT ノードの抽象基底。tick は noexcept、blackboard は void* で型を持たない。
class BtNode {
public:
    BtNode() noexcept = default;
    virtual ~BtNode() noexcept = default;

    BtNode(const BtNode&)            = delete;
    BtNode& operator=(const BtNode&) = delete;
    BtNode(BtNode&&)                 = delete;
    BtNode& operator=(BtNode&&)      = delete;

    // 1 フレーム分の評価。blackboard は user 定義 (typically `Self*` にキャスト)。
    virtual EBtStatus Tick(void* blackboard, f32 dt) noexcept = 0;
};

// Selector ("OR" 合成): 子を順に Tick し、最初に Running か Success を返した子で停止。
//   ・どれかが Success/Running → そのまま返す
//   ・全て Failure              → Failure を返す
class BtSelector : public BtNode {
public:
    BtSelector() noexcept = default;
    ~BtSelector() noexcept override = default;

    // 子の所有権を奪う。nullptr 渡しは no-op (ソフトフェイル)。
    void AddChild(UniquePtr<BtNode> child) noexcept;

    EBtStatus Tick(void* blackboard, f32 dt) noexcept override;

    usize ChildCount() const noexcept { return _children.Size(); }

private:
    Array<UniquePtr<BtNode>> _children;
};

// Sequence ("AND" 合成): 子を順に Tick し、最初に Running か Failure を返した子で停止。
//   ・どれかが Failure/Running → そのまま返す
//   ・全て Success              → Success を返す
class BtSequence : public BtNode {
public:
    BtSequence() noexcept = default;
    ~BtSequence() noexcept override = default;

    // 子の所有権を奪う。nullptr 渡しは no-op (ソフトフェイル)。
    void AddChild(UniquePtr<BtNode> child) noexcept;

    EBtStatus Tick(void* blackboard, f32 dt) noexcept override;

    usize ChildCount() const noexcept { return _children.Size(); }

private:
    Array<UniquePtr<BtNode>> _children;
};

// Action: 関数ポインタを呼ぶだけの末端 leaf。
//   ・`std::function` 不使用 (ACS 規約)。状態を持ちたい場合は blackboard に置く。
//   ・fn が nullptr の場合は常に Failure を返す (ソフトフェイル)。
class BtAction : public BtNode {
public:
    using Fn = EBtStatus(*)(void* blackboard, f32 dt) noexcept;

    explicit BtAction(Fn fn) noexcept : _fn(fn) {}
    ~BtAction() noexcept override = default;

    EBtStatus Tick(void* blackboard, f32 dt) noexcept override;

private:
    Fn _fn = nullptr;
};

// BehaviorTree: root を抱えて Tick を駆動するだけのハーネス。
// 非コピー・非ムーブ — Scene / Actor のメンバとして固定の場所に置く想定。
class BehaviorTree {
public:
    BehaviorTree() noexcept = default;
    ~BehaviorTree() noexcept = default;

    BehaviorTree(const BehaviorTree&)            = delete;
    BehaviorTree& operator=(const BehaviorTree&) = delete;
    BehaviorTree(BehaviorTree&&)                 = delete;
    BehaviorTree& operator=(BehaviorTree&&)      = delete;

    // root を差し替える。古い root はここで破棄される (UniquePtr デストラクタ)。
    // nullptr 渡しで tree を空にできる (以降の Tick は Failure を返す)。
    void SetRoot(UniquePtr<BtNode> root) noexcept;

    // 1 フレーム分の評価。root 未設定なら Failure を返す。
    EBtStatus Tick(void* blackboard, f32 dt) noexcept;

    bool HasRoot() const noexcept { return static_cast<bool>(_root); }

private:
    UniquePtr<BtNode> _root;
};

} // namespace acs::game
