// SPDX-License-Identifier: Apache-2.0
// GameFramework Tools — editor_core / FEditorCommand (Phase 21a)
//
// 役割:
//   ACS の全エディタ (FHierarchyPanel / FInspectorPanel / ParticleEditor /
//   SceneInspector ほか将来追加される LevelEditor / TimelineEditor 等) が共有
//   する **undo/redo の原子単位**。Command パターンの GoF 古典に沿い、
//   1 操作 = 1 FEditorCommand 派生インスタンスとして表現する。
//
// 使い方:
//   class MoveNodeCommand : public FEditorCommand { ... };  // header 下に inline 例
//
//   // editor 側:
//   acs::TUniquePtr<MoveNodeCommand> cmd = acs::MakeUnique<MoveNodeCommand>(
//       &node, old_pos, new_pos);
//   undo_stack.Push(cmd.Release());   // 所有権を渡す + Execute 実行
//
// 設計選択 (Phase 21a):
//   ・**純粋抽象 + virtual dtor**: ベース型を `TUniquePtr<FEditorCommand>` で
//     FUndoStack に持たせるため、polymorphic delete が必要。全 noexcept は
//     ACS 規約。
//   ・**非コピー / 非ムーブ**: 「実行済み command を後から複製」は意味的に怪しい
//     (二重 Undo 等の事故源)。意図的な複製は派生クラス側で factory を用意する。
//   ・**Description は const char***: undo history UI (ImGui MenuItem の
//     "Undo Move Node" 表示) に直接渡す想定。動的文字列が要る派生クラスは
//     自分で `TArray<char>` 等を抱える。
//   ・**CanMerge / MergeWith**: 連続 slider / drag 操作 (例: position を
//     1 フレームに 60 回いじる) を 1 件にまとめる。default は merge 拒否。
//     merge する派生は **同じ対象 + 同じ "種類"** を確認したうえで、自身の
//     "new 値" を `next` の "new 値" に置き換える ("old 値" は最初の cmd の
//     ものを保つ)。これで Undo 1 回で連続 drag 全体を巻き戻せる。
//
// 将来拡張余地:
//   ・transaction (BeginGroup / EndGroup): FUndoStack 側で複数 FEditorCommand
//     を 1 件として束ねる `CommandGroup : FEditorCommand` を派生で実装する。
//   ・branched history: undo 後に new edit が来た時に redo stack を破棄するの
//     ではなく分岐として保存する。FEditorCommand 側に変更は不要。
//   ・serialization: `virtual void Serialize(Writer&) const` を後付け可能。
//     既存派生は default = no-op で問題ない。
#pragma once

#include "foundation/Types.h"
#include "gameframework/Node2D.h"
#include "math/Vec.h"

namespace acs::game::editor_core {

// =============================================================================
// FEditorCommand — undo/redo の原子単位 (純粋抽象)
// -----------------------------------------------------------------------------
// 派生クラスは Execute / Undo / Description を必ず override する。
// CanMerge / MergeWith は default で merge 拒否、必要な派生のみ override する。
// =============================================================================
class FEditorCommand {
public:
    FEditorCommand() noexcept          = default;
    virtual ~FEditorCommand() noexcept = default;

    // 非コピー / 非ムーブ: 実行済み command の複製は意味的に危険なので禁止。
    FEditorCommand(const FEditorCommand&)            = delete;
    FEditorCommand& operator=(const FEditorCommand&) = delete;
    FEditorCommand(FEditorCommand&&)                 = delete;
    FEditorCommand& operator=(FEditorCommand&&)      = delete;

    // RTTI を使わずに command の "種類" を識別するための tag。
    // 派生クラスは自前の静的アドレス (static const char k; ... return &k;) を
    // 返す。default 実装は nullptr (= 「kind 不明、merge 対象外」)。
    // CanMerge / MergeWith は「Kind() ポインタが一致するか」を最低条件として
    // 派生で確認することで、不正な cross-type cast を防ぐ。
    virtual const void* Kind() const noexcept { return nullptr; }

    // 「Do」を実行する。FUndoStack::Push 直後と、Redo 経路の両方から呼ばれる。
    // 既に Execute 済みの命令を 2 連続で呼ぶケースは FUndoStack が排除する
    // (Undo を挟まずに同じ command を 2 回 Execute することはない)。
    virtual void Execute() noexcept = 0;

    // Execute の逆操作を行う。Execute → Undo → Execute と往復可能な対称性が要件。
    virtual void Undo() noexcept = 0;

    // undo history UI 表示用の短い人間可読ラベル ("Move Node", "Add Component" 等)。
    // 戻り値は **永続文字列** であること (literal or 派生クラス内 char[N])。
    // ImGui MenuItem 等にそのまま渡される想定。
    virtual const char* Description() const noexcept = 0;

    // 直後に Push されようとしている `next` を、自分にマージ可能か返す。
    // default は false (= マージしない、毎回別エントリとして undo stack に積む)。
    //
    // マージは「同じ対象 (= same target pointer / same FNodeId)」かつ
    // 「同じ command kind (typeid 相当のフィールド比較)」のときに限定する。
    // 派生クラスが override する場合は dynamic_cast を使わず、自前の kind 比較
    // (例: const char* GetKind() を override) で行うこと (ACS は RTTI を避ける)。
    virtual bool CanMerge(const FEditorCommand& /*next*/) const noexcept {
        return false;
    }

    // CanMerge が true を返した直後に FUndoStack が呼ぶ。`next` の「new 値」を
    // 自分に取り込み (= 自分の new 値を next の new 値で上書きし)、自分の
    // old 値はそのまま保つ。これで Undo 1 回で連続 drag 全体を巻き戻せる。
    // default は no-op (CanMerge が false なので呼ばれない想定)。
    virtual void MergeWith(const FEditorCommand& /*next*/) noexcept {}
};

// =============================================================================
// MoveNodeCommand — FNode2D の position を変更する FEditorCommand 派生サンプル
// -----------------------------------------------------------------------------
// 教科書的な使用例。連続 drag を 1 件にまとめる CanMerge 実装も持つ。
//
// 使い方 (FUndoStack 経由):
//   const acs::FVec2 old_pos = node.Local().position;
//   const acs::FVec2 new_pos = old_pos + delta;
//   acs::TUniquePtr<MoveNodeCommand> c = acs::MakeUnique<MoveNodeCommand>(
//       &node, old_pos, new_pos);
//   undo_stack.Push(c.Release());
//
// マージ規約:
//   ・同じ `m_Target` (raw pointer 一致) かつ同じ kind (= MoveNodeCommand)
//     なら merge OK。
//   ・MergeWith で `m_NewPos` を next の `m_NewPos` に置き換える。
//     `m_OldPos` は最初の cmd のものを保つ (= Undo 1 回で初期位置に戻る)。
//   ・対象ノードが既に Destroy 済みの場合 (= raw pointer dangling) でも
//     比較自体は安全 (pointer 比較のみ)。実 Execute / Undo 時に対象が dangling
//     だと UB になるので、editor 側で Destroy 時に undo stack を Clear する等の
//     ハイレベルポリシーで防ぐ (Phase 21a 範囲外)。
// =============================================================================
class MoveNodeCommand : public FEditorCommand {
public:
    MoveNodeCommand(FNode2D* target, FVec2 old_pos, FVec2 new_pos) noexcept
        : m_Target(target), m_OldPos(old_pos), m_NewPos(new_pos) {}

    void Execute() noexcept override {
        if (m_Target != nullptr) {
            m_Target->Local().position = m_NewPos;
        }
    }

    void Undo() noexcept override {
        if (m_Target != nullptr) {
            m_Target->Local().position = m_OldPos;
        }
    }

    const char* Description() const noexcept override {
        return "Move Node";
    }

    // Kind tag は派生固有の静的アドレス。next 側も同じアドレスを返せば
    // 同一派生型と確定できる (= RTTI 不要、純粋にポインタ比較で型判定)。
    const void* Kind() const noexcept override { return KindTag(); }

    // 同一 target に対する連続 MoveNodeCommand のみ merge を許す。
    bool CanMerge(const FEditorCommand& next) const noexcept override {
        // 型 (Kind tag) と対象 (target ポインタ) が両方一致するときだけ merge。
        if (next.Kind() != KindTag()) {
            return false;
        }
        // ここまでくれば next は MoveNodeCommand と確定 (Kind tag は static アドレス
        // で派生クラスを一意に識別している)。安全に static_cast 可能。
        const MoveNodeCommand& nxt = static_cast<const MoveNodeCommand&>(next);
        return nxt.m_Target == m_Target;
    }

    void MergeWith(const FEditorCommand& next) noexcept override {
        // CanMerge を経ているのが FUndoStack 側の前提だが、防御的に再確認する。
        if (next.Kind() != KindTag()) {
            return;
        }
        const MoveNodeCommand& nxt = static_cast<const MoveNodeCommand&>(next);
        // new 値だけ更新し、old 値 (= 連続 drag の始点) は保持する。
        m_NewPos = nxt.m_NewPos;
    }

    // ----- アクセサ (テスト / inspector 表示用) -----
    const FNode2D* Target() const noexcept { return m_Target; }
    FVec2          OldPosition() const noexcept { return m_OldPos; }
    FVec2          NewPosition() const noexcept { return m_NewPos; }

private:
    // 派生固有の kind 識別用の静的アドレス。内容は使わない、アドレスだけが ID。
    // function-local static にすることで、ヘッダ多重 include + 複数 TU 跨ぎで
    // 「同一アドレス」を保証する (C++11 以降の保証)。
    static const void* KindTag() noexcept {
        static const char kTag = 0;
        return &kTag;
    }

    FNode2D* m_Target  = nullptr;
    FVec2    m_OldPos {};
    FVec2    m_NewPos {};
};

} // namespace acs::game::editor_core
