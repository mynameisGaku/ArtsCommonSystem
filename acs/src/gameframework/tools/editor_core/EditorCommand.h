// SPDX-License-Identifier: Apache-2.0
// GameFramework Tools — editor_core / EditorCommand (Phase 21a)
//
// 役割:
//   ACS の全エディタ (HierarchyPanel / InspectorPanel / ParticleEditor /
//   SceneInspector ほか将来追加される LevelEditor / TimelineEditor 等) が共有
//   する **undo/redo の原子単位**。Command パターンの GoF 古典に沿い、
//   1 操作 = 1 EditorCommand 派生インスタンスとして表現する。
//
// 使い方:
//   class MoveNodeCommand : public EditorCommand { ... };  // header 下に inline 例
//
//   // editor 側:
//   acs::TUniquePtr<MoveNodeCommand> cmd = acs::MakeUnique<MoveNodeCommand>(
//       &node, old_pos, new_pos);
//   undo_stack.Push(cmd.Release());   // 所有権を渡す + Execute 実行
//
// 設計選択 (Phase 21a):
//   ・**純粋抽象 + virtual dtor**: ベース型を `TUniquePtr<EditorCommand>` で
//     UndoStack に持たせるため、polymorphic delete が必要。全 noexcept は
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
//   ・transaction (BeginGroup / EndGroup): UndoStack 側で複数 EditorCommand
//     を 1 件として束ねる `CommandGroup : EditorCommand` を派生で実装する。
//   ・branched history: undo 後に new edit が来た時に redo stack を破棄するの
//     ではなく分岐として保存する。EditorCommand 側に変更は不要。
//   ・serialization: `virtual void Serialize(Writer&) const` を後付け可能。
//     既存派生は default = no-op で問題ない。
#pragma once

#include "foundation/Types.h"
#include "gameframework/Node2D.h"
#include "math/Vec.h"

namespace acs::game::editor_core {

// =============================================================================
// EditorCommand — undo/redo の原子単位 (純粋抽象)
// -----------------------------------------------------------------------------
// 派生クラスは Execute / Undo / Description を必ず override する。
// CanMerge / MergeWith は default で merge 拒否、必要な派生のみ override する。
// =============================================================================
class EditorCommand {
public:
    EditorCommand() noexcept          = default;
    virtual ~EditorCommand() noexcept = default;

    // 非コピー / 非ムーブ: 実行済み command の複製は意味的に危険なので禁止。
    EditorCommand(const EditorCommand&)            = delete;
    EditorCommand& operator=(const EditorCommand&) = delete;
    EditorCommand(EditorCommand&&)                 = delete;
    EditorCommand& operator=(EditorCommand&&)      = delete;

    // RTTI を使わずに command の "種類" を識別するための tag。
    // 派生クラスは自前の静的アドレス (static const char k; ... return &k;) を
    // 返す。default 実装は nullptr (= 「kind 不明、merge 対象外」)。
    // CanMerge / MergeWith は「Kind() ポインタが一致するか」を最低条件として
    // 派生で確認することで、不正な cross-type cast を防ぐ。
    virtual const void* Kind() const noexcept { return nullptr; }

    // 「Do」を実行する。UndoStack::Push 直後と、Redo 経路の両方から呼ばれる。
    // 既に Execute 済みの命令を 2 連続で呼ぶケースは UndoStack が排除する
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
    virtual bool CanMerge(const EditorCommand& /*next*/) const noexcept {
        return false;
    }

    // CanMerge が true を返した直後に UndoStack が呼ぶ。`next` の「new 値」を
    // 自分に取り込み (= 自分の new 値を next の new 値で上書きし)、自分の
    // old 値はそのまま保つ。これで Undo 1 回で連続 drag 全体を巻き戻せる。
    // default は no-op (CanMerge が false なので呼ばれない想定)。
    virtual void MergeWith(const EditorCommand& /*next*/) noexcept {}
};

// =============================================================================
// MoveNodeCommand — Node2D の position を変更する EditorCommand 派生サンプル
// -----------------------------------------------------------------------------
// 教科書的な使用例。連続 drag を 1 件にまとめる CanMerge 実装も持つ。
//
// 使い方 (UndoStack 経由):
//   const acs::FVec2 old_pos = node.Local().position;
//   const acs::FVec2 new_pos = old_pos + delta;
//   acs::TUniquePtr<MoveNodeCommand> c = acs::MakeUnique<MoveNodeCommand>(
//       &node, old_pos, new_pos);
//   undo_stack.Push(c.Release());
//
// マージ規約:
//   ・同じ `_target` (raw pointer 一致) かつ同じ kind (= MoveNodeCommand)
//     なら merge OK。
//   ・MergeWith で `_new_pos` を next の `_new_pos` に置き換える。
//     `_old_pos` は最初の cmd のものを保つ (= Undo 1 回で初期位置に戻る)。
//   ・対象ノードが既に Destroy 済みの場合 (= raw pointer dangling) でも
//     比較自体は安全 (pointer 比較のみ)。実 Execute / Undo 時に対象が dangling
//     だと UB になるので、editor 側で Destroy 時に undo stack を Clear する等の
//     ハイレベルポリシーで防ぐ (Phase 21a 範囲外)。
// =============================================================================
class MoveNodeCommand : public EditorCommand {
public:
    MoveNodeCommand(Node2D* target, FVec2 old_pos, FVec2 new_pos) noexcept
        : _target(target), _old_pos(old_pos), _new_pos(new_pos) {}

    void Execute() noexcept override {
        if (_target != nullptr) {
            _target->Local().position = _new_pos;
        }
    }

    void Undo() noexcept override {
        if (_target != nullptr) {
            _target->Local().position = _old_pos;
        }
    }

    const char* Description() const noexcept override {
        return "Move Node";
    }

    // Kind tag は派生固有の静的アドレス。next 側も同じアドレスを返せば
    // 同一派生型と確定できる (= RTTI 不要、純粋にポインタ比較で型判定)。
    const void* Kind() const noexcept override { return KindTag(); }

    // 同一 target に対する連続 MoveNodeCommand のみ merge を許す。
    bool CanMerge(const EditorCommand& next) const noexcept override {
        // 型 (Kind tag) と対象 (target ポインタ) が両方一致するときだけ merge。
        if (next.Kind() != KindTag()) {
            return false;
        }
        // ここまでくれば next は MoveNodeCommand と確定 (Kind tag は static アドレス
        // で派生クラスを一意に識別している)。安全に static_cast 可能。
        const MoveNodeCommand& nxt = static_cast<const MoveNodeCommand&>(next);
        return nxt._target == _target;
    }

    void MergeWith(const EditorCommand& next) noexcept override {
        // CanMerge を経ているのが UndoStack 側の前提だが、防御的に再確認する。
        if (next.Kind() != KindTag()) {
            return;
        }
        const MoveNodeCommand& nxt = static_cast<const MoveNodeCommand&>(next);
        // new 値だけ更新し、old 値 (= 連続 drag の始点) は保持する。
        _new_pos = nxt._new_pos;
    }

    // ----- アクセサ (テスト / inspector 表示用) -----
    const Node2D* Target() const noexcept { return _target; }
    FVec2          OldPosition() const noexcept { return _old_pos; }
    FVec2          NewPosition() const noexcept { return _new_pos; }

private:
    // 派生固有の kind 識別用の静的アドレス。内容は使わない、アドレスだけが ID。
    // function-local static にすることで、ヘッダ多重 include + 複数 TU 跨ぎで
    // 「同一アドレス」を保証する (C++11 以降の保証)。
    static const void* KindTag() noexcept {
        static const char kTag = 0;
        return &kTag;
    }

    Node2D* _target  = nullptr;
    FVec2    _old_pos {};
    FVec2    _new_pos {};
};

} // namespace acs::game::editor_core
