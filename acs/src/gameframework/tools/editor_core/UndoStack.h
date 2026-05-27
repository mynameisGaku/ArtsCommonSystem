// SPDX-License-Identifier: Apache-2.0
// GameFramework Tools — editor_core / FUndoStack (Phase 21a)
//
// 役割:
//   全エディタが共有する undo / redo の中央ハブ。FEditorCommand 派生インスタンス
//   を所有して、Push で 1 操作分を実行・登録し、Undo / Redo でユーザ操作を巻き
//   戻し / やり直しする。連続 drag 等の merge も Push 時にこちらで判定する。
//
// 使い方:
//   acs::game::editor_core::FUndoStack stack;
//   stack.Init(64);
//
//   // 1 操作: Push に所有権を渡す
//   auto* cmd = acs::New<MoveNodeCommand>(acs::DefaultAllocator(),
//                                          &node, old_pos, new_pos);
//   stack.Push(cmd);
//
//   if (ImGui::MenuItem("Undo", "Ctrl+Z", false, stack.CanUndo())) {
//       stack.Undo();
//   }
//
// 設計選択 (Phase 21a):
//   ・**TUniquePtr<FEditorCommand> を 2 本の TArray に積む**: undo / redo の
//     LIFO スタック。基底ポインタなので polymorphic dispatch (virtual Execute /
//     Undo / Description) で派生型を意識せず巻き戻せる。
//   ・**Push で所有権を奪う (raw pointer 引数 + delete 責任)**: 既存サンプル
//     (FHierarchyPanel 等) と同じく ImGui 連携の単純な C-API 風シグネチャに
//     揃える。caller は `cmd.Release()` で渡す or 自分で New してそのまま渡す。
//     Push 内で TUniquePtr に詰め直してから _undo_stack に PushBack することで
//     その後の代入 / pop で自動 delete される。
//   ・**Push 内で Execute も実行**: GUI コードが「new FEditorCommand → Push」
//     しか書かなくて済む (= Execute 忘れを構造的に防ぐ)。Redo 経路でも
//     FEditorCommand::Execute() を呼ぶので、Execute は何度呼ばれても idempotent
//     な実装にしておく必要がある (Undo を挟まずに連続 Execute は FUndoStack 側
//     で排除済み)。
//   ・**Push は redo stack をクリア**: Undo 後に new edit が来た時点で
//     "redo の未来" は破棄される (Git の reset --hard 相当)。branched history
//     は将来拡張で別レイヤに分離する。
//   ・**max_history 上限 + oldest drop**: 古い undo を 1 件 (FIFO 的に) 落として
//     上限を守る。下端 drop なので TUniquePtr の delete が自動走り、メモリリーク
//     しない。TArray に LIFO + 上限管理しか要らない用途で circular buffer は不要
//     (上限超は希なはず、上限超時に O(N) shift しても害は少ない)。
//   ・**CanMerge / MergeWith**: Push 時に「直前の _undo_stack.Back() が merge
//     可能か」を判定し、可能なら新 cmd を merge してそのまま破棄する (= スタッ
//     ク件数は増えない)。連続 drag が爆発しない用の最重要機能。
//   ・**callback (CommandExecutedCallback)**: editor が「Undo されたから
//     hierarchy を再描画」「Redo されたから dirty フラグ立て直し」等の副反応を
//     書けるようにする。引数は (user, cmd, is_redo) で、Push / Redo 経路は
//     is_redo = true、Undo 経路は cb 呼ばずに済ませる? いや、Undo も通知する
//     のが正しい (= dirty 状態 → clean に戻すケースが要る)。ここでは Push 経路
//     のみ「初回 Execute だから is_redo=false」、Redo 経路は「is_redo=true」、
//     Undo 経路は callback 呼出 (is_redo=false) で表現する。is_redo の意味は
//     「Redo 由来の execution か?」であり、Push と Undo は 0 で同じだが、
//     callback 側で必要なら Description で識別する。
//
// 将来拡張余地 (Phase 21a 範囲外):
//   ・transaction (BeginGroup / EndGroup): Push を一時的に Group に転送して
//     EndGroup で 1 個の CommandGroup 派生にまとめる。
//   ・branched history (git のような分岐 undo): Push で redo stack を破棄せず
//     branch ツリーに移し替える。
//   ・serialization (.acsundo): undo stack 内容をテキスト or バイナリで save。
//     FEditorCommand 派生に virtual Serialize / Deserialize を追加することで
//     対応する。
//   ・"Undo to here" (history list で n 個まとめて undo): n 個 pop を 1 個ずつ
//     回せば実現できるので公開 API 不要。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "memory/UniquePtr.h"

namespace acs::game::editor_core {

class FEditorCommand;

// =============================================================================
// CommandExecutedCallback — Push / Undo / Redo 後の副反応用
// -----------------------------------------------------------------------------
// `user`    : SetOnExecutedCallback の第二引数で渡したコンテキスト。
// `cmd`     : 直前に Execute / Undo された command。Push 経路は merge で
//             差し替わった可能性があるので、必ず最終的にスタックに載った
//             方を返す (= Push 内で merge があった場合は基底 = top の方)。
// `is_redo` : Redo 経路から呼ばれた場合 true、Push / Undo 経路は false。
//             undo / redo の方向は cb 側で必要なら別パラメータを足す。
// =============================================================================
using CommandExecutedCallback =
    void (*)(void* user, const class FEditorCommand* cmd, bool is_redo) noexcept;

// =============================================================================
// FUndoStack — Editor 共通の undo/redo ハブ
// -----------------------------------------------------------------------------
// 非コピー / 非ムーブ、全 noexcept、STL 不使用 (`acs::TArray` + `acs::TUniquePtr`)。
// =============================================================================
class FUndoStack {
public:
    FUndoStack() noexcept  = default;
    ~FUndoStack() noexcept = default;

    FUndoStack(const FUndoStack&)            = delete;
    FUndoStack& operator=(const FUndoStack&) = delete;
    FUndoStack(FUndoStack&&)                 = delete;
    FUndoStack& operator=(FUndoStack&&)      = delete;

    // 初期化。多重 Init 可 (履歴を空にする + 上限を再設定)。max_history は
    // undo + redo それぞれの上限ではなく「undo stack に積める件数の上限」。
    // 0 を渡された場合は default (64) にフォールバックする。
    void Init(u32 max_history = 64) noexcept;

    // 1 件積む。所有権を奪う (内部で TUniquePtr に詰め直して破棄責任を引き取る)。
    // 動作順:
    //   1. cmd == nullptr なら no-op (誤呼び対策、cmd は delete されない)。
    //   2. cmd->Execute() を呼ぶ。
    //   3. 直前 (= _undo_stack.Back()) と CanMerge / MergeWith できれば
    //      `_undo_stack` には PushBack せず、merge 結果として cmd は破棄。
    //   4. merge しなければ _undo_stack に PushBack。
    //   5. _redo_stack を Clear (= new edit が来たので redo の未来は破棄)。
    //   6. _undo_stack.Size() > _max_history なら最古を 1 件捨てる。
    //   7. callback を (cb_user, スタック top の cmd, is_redo=false) で発火。
    void Push(class FEditorCommand* cmd) noexcept;

    // undo を 1 件巻き戻す。動作順:
    //   1. _undo_stack.IsEmpty() なら false を返して no-op。
    //   2. top を取り出して cmd->Undo() を呼ぶ。
    //   3. その cmd を _redo_stack に Move する (= 再度 Redo で execute できる)。
    //   4. callback を (cb_user, cmd, is_redo=false) で発火 (undo は redo 由来
    //      ではないので false)。
    //   5. true を返す。
    bool Undo() noexcept;

    // redo を 1 件やり直す。動作順は Undo の対称:
    //   1. _redo_stack.IsEmpty() なら false を返して no-op。
    //   2. _redo_stack.Back() を取り出して cmd->Execute() を呼ぶ。
    //   3. その cmd を _undo_stack に Move する。
    //   4. callback を (cb_user, cmd, is_redo=true) で発火。
    //   5. true を返す。
    bool Redo() noexcept;

    bool CanUndo() const noexcept;
    bool CanRedo() const noexcept;

    u32 UndoCount() const noexcept;
    u32 RedoCount() const noexcept;

    // top の Description (UI 表示用)。各スタックが空のときは空文字列 "" を返す
    // (nullptr ではない → UI 側で strlen / strcmp が安全)。
    const char* UndoDescription() const noexcept;
    const char* RedoDescription() const noexcept;

    // 全 cmd 破棄。TUniquePtr の dtor で自動 delete される。
    void Clear() noexcept;

    // (cb, user) ペアを 1 個保持する (= 単一購読モード)。複数購読は今のところ
    // 必要性が薄い (= editor のメインループが 1 個この callback で副反応をまとめ
    // て管理する想定)。cb = nullptr を渡せば解除。
    void SetOnExecutedCallback(CommandExecutedCallback cb, void* user) noexcept;

private:
    // _undo_stack が _max_history を超えていれば、最古 1 件を捨てる。
    // PushBack 後にのみ呼ぶ前提なので、超過量は高々 1。
    void DropOldestIfOverflow() noexcept;

    TArray<TUniquePtr<FEditorCommand>> _undo_stack {};
    TArray<TUniquePtr<FEditorCommand>> _redo_stack {};
    u32                             _max_history = 64;
    CommandExecutedCallback         _cb          = nullptr;
    void*                           _cb_user     = nullptr;
};

} // namespace acs::game::editor_core
