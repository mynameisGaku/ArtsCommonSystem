// SPDX-License-Identifier: Apache-2.0
// GameFramework Tools — editor_core / CUndoStack
//
// 役割:
//   全エディタが共有する undo / redo の中央ハブ。AEditorCommand 派生インスタンス
//   を所有して、Push で 1 操作分を実行・登録し、Undo / Redo でユーザ操作を巻き
//   戻し / やり直しする。連続 drag 等の merge も Push 時にこちらで判定する。
//
// 使い方:
//   acs::game::editor_core::CUndoStack stack;
//   stack.Init(64);
//
//   // 1 操作: Push に所有権を渡す
//   auto command =
//       acs::MakeUnique<acs::game::editor_core::AMoveNodeCommand>(
//           &node, old_pos, new_pos);
//   stack.Push(acs::Move(command));
//
//   if (ImGui::MenuItem("Undo", "Ctrl+Z", false, stack.CanUndo())) {
//       stack.Undo();
//   }
//
// 設計選択:
//   ・**TUniquePtr<AEditorCommand> を 2 本の TArray に積む**: undo / redo の
//     LIFO スタック。基底ポインタなので polymorphic dispatch (virtual Execute /
//     Undo / Description) で派生型を意識せず巻き戻せる。
//   ・**Push で所有権を奪う**: TUniquePtr の確保元を保ったまま Move する経路を
//     標準とし、既定アロケータ由来の既存コード向けに raw pointer 経路も残す。
//     m_UndoStack に TUniquePtr のまま積むため、代入 / pop でも生成時の確保元へ
//     自動的に返される。
//   ・**Push 内で Execute も実行**: GUI コードが「new AEditorCommand → Push」
//     しか書かなくて済む (= Execute 忘れを構造的に防ぐ)。Redo 経路でも
//     AEditorCommand::Execute() を呼ぶので、Execute は何度呼ばれても idempotent
//     な実装にしておく必要がある (Undo を挟まずに連続 Execute は CUndoStack 側
//     で排除済み)。
//   ・**Push は redo stack をクリア**: Undo 後に new edit が来た時点で
//     "redo の未来" は破棄される (Git の reset --hard 相当)。branched history
//     は将来拡張で別レイヤに分離する。
//   ・**max_history 上限 + oldest drop**: 古い undo を 1 件 (FIFO 的に) 落として
//     上限を守る。下端 drop なので TUniquePtr の delete が自動走り、メモリリーク
//     しない。TArray に LIFO + 上限管理しか要らない用途で circular buffer は不要
//     (上限超は希なはず、上限超時に O(N) shift しても害は少ない)。
//   ・**CanMerge / MergeWith**: Push 時に「直前の m_UndoStack.Back() が merge
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
// 将来拡張余地:
//   ・transaction (BeginGroup / EndGroup): Push を一時的に Group に転送して
//     EndGroup で 1 個の CommandGroup 派生にまとめる。
//   ・branched history (git のような分岐 undo): Push で redo stack を破棄せず
//     branch ツリーに移し替える。
//   ・serialization (.acsundo): undo stack 内容をテキスト or バイナリで save。
//     AEditorCommand 派生に virtual Serialize / Deserialize を追加することで
//     対応する。
//   ・"Undo to here" (history list で n 個まとめて undo): n 個 pop を 1 個ずつ
//     回せば実現できるので公開 API 不要。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "gameframework/Forward.h"
#include "memory/UniquePtr.h"

namespace acs::game::editor_core {

/**
 * Push / Undo / Redo 後の副反応を editor 側へ通知するコールバック型。
 *
 * @details
 * editor が「Undo されたから hierarchy を再描画」「Redo されたから dirty
 * フラグを立て直し」等の処理を書けるようにする。第二引数の cmd は直前に
 * Execute / Undo された command で、Push 経路で merge があった場合は最終的に
 * スタックに載った top を返す。is_redo は Redo 経路から呼ばれたときのみ true、
 * Push / Undo 経路は false。
 *
 * @param user SetOnExecutedCallback の第二引数で渡したコンテキスト。
 * @param cmd 直前に Execute / Undo された command (スタック top)。
 * @param is_redo Redo 経路なら true、Push / Undo 経路は false。
 */
using CommandExecutedCallback =
    void (*)(void* user, const AEditorCommand* cmd, bool is_redo) noexcept;

/**
 * 全エディタが共有する undo / redo の中央ハブ。
 *
 * @details
 * AEditorCommand 派生インスタンスを 2 本の LIFO スタック (undo / redo) に
 * TUniquePtr で所有し、Push で 1 操作分を実行・登録、Undo / Redo で巻き戻し /
 * やり直しする。連続 drag 等の merge も Push 時にここで判定する。非コピー /
 * 非ムーブ、全 noexcept、STL 不使用 (acs::TArray + acs::TUniquePtr)。
 */
class CUndoStack {
public:
    /** 空のスタックを構築する (上限は default 64、Init で再設定可)。 */
    CUndoStack() noexcept  = default;

    /** 破棄する (全 cmd は TUniquePtr の dtor で自動 delete される)。 */
    ~CUndoStack() noexcept = default;

    /** コピー禁止 (command の所有権を単独で持つため)。 */
    CUndoStack(const CUndoStack&)            = delete;

    /** コピー代入も禁止。 */
    CUndoStack& operator=(const CUndoStack&) = delete;

    /** ムーブ禁止。 */
    CUndoStack(CUndoStack&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CUndoStack& operator=(CUndoStack&&)      = delete;

    /**
     * 履歴を空にして上限を設定する (多重 Init 安全)。
     *
     * @details
     * max_history は undo / redo それぞれの上限ではなく「undo stack に積める
     * 件数の上限」。0 を渡された場合は default (64) にフォールバックする。
     * callback は Init では解除しない (起動側が Init より先に登録するケースを許容)。
     * @param max_history undo stack に積める件数の上限 (0 は 64 に丸める)。
     */
    void Init(u32 max_history = 64) noexcept;

    /**
     * command を 1 件積む (所有権を奪い、その場で Execute する)。
     *
     * @details
     * cmd == nullptr なら silent no-op。それ以外は受け取った直後に TUniquePtr へ
     * 詰め直して破棄責任を引き取り、cmd->Execute() を呼ぶ。直前 (m_UndoStack.Back())
     * と CanMerge / MergeWith できれば PushBack せず merge して破棄し (連続 drag の
     * 1 件化)、そうでなければ m_UndoStack に PushBack する。続けて m_RedoStack を
     * Clear し (new edit で redo の未来は破棄)、上限超過なら最古 1 件を捨て、
     * 最後に callback を (cb_user, スタック top, is_redo=false) で発火する。
     * @param cmd 積む command (所有権が移る。nullptr は no-op)。
     */
    void Push(AEditorCommand* cmd) noexcept;

    /**
     * 解放元を保持した command を 1 件積む。
     *
     * @details MakeUniqueIn などで既定以外のアロケータから生成した command は、
     * このオーバーロードへ Move することで生成時の解放元を維持できる。
     * @param command 積む command。所有権が移る。空なら no-op。
     */
    void Push(TUniquePtr<AEditorCommand> command) noexcept;

    /**
     * 生ポインタとその確保元を明示して command を 1 件積む。
     *
     * @param command 積む command。所有権が移る。nullptr は no-op。
     * @param allocator command を解放するアロケータ。
     */
    void Push(AEditorCommand* command, FAllocator& allocator) noexcept;

    /**
     * undo を 1 件巻き戻す。
     *
     * @details
     * m_UndoStack が空なら false を返して no-op。そうでなければ top を取り出して
     * cmd->Undo() を呼び、その cmd を m_RedoStack へ Move する (再度 Redo で実行可)。
     * 最後に callback を (cb_user, cmd, is_redo=false) で発火する (Undo は Redo
     * 由来ではないので false)。
     * @return 巻き戻したら true、undo stack が空なら false。
     */
    bool Undo() noexcept;

    /**
     * redo を 1 件やり直す (Undo の対称操作)。
     *
     * @details
     * m_RedoStack が空なら false を返して no-op。そうでなければ top を取り出して
     * cmd->Execute() を再実行し、その cmd を m_UndoStack へ Move する。最後に
     * callback を (cb_user, cmd, is_redo=true) で発火する。
     * @return やり直したら true、redo stack が空なら false。
     */
    bool Redo() noexcept;

    /**
     * undo できる command があるかを返す。
     *
     * @return undo stack が空でなければ true。
     */
    bool CanUndo() const noexcept;

    /**
     * redo できる command があるかを返す。
     *
     * @return redo stack が空でなければ true。
     */
    bool CanRedo() const noexcept;

    /**
     * undo stack に積まれている件数を返す。
     *
     * @return undo 可能な command の数。
     */
    u32 UndoCount() const noexcept;

    /**
     * redo stack に積まれている件数を返す。
     *
     * @return redo 可能な command の数。
     */
    u32 RedoCount() const noexcept;

    /**
     * undo stack top の Description を返す (UI 表示用)。
     *
     * @details スタックが空または top が null のときは空文字列 "" を返す (nullptr ではない → UI 側で strlen / strcmp が安全)。
     * @return top command の説明文字列 (無ければ "")。
     */
    const char* UndoDescription() const noexcept;

    /**
     * redo stack top の Description を返す (UI 表示用)。
     *
     * @details スタックが空または top が null のときは空文字列 "" を返す。
     * @return top command の説明文字列 (無ければ "")。
     */
    const char* RedoDescription() const noexcept;

    /** undo / redo 両スタックの全 command を破棄する (TUniquePtr の dtor で自動 delete)。 */
    void Clear() noexcept;

    /**
     * Push / Undo / Redo 後に呼ぶ callback を登録する (単一購読モード)。
     *
     * @details (cb, user) ペアを 1 個だけ保持する。cb = nullptr を渡せば解除。
     * @param cb 発火させる callback (nullptr で解除)。
     * @param user callback の第一引数として渡すコンテキスト。
     */
    void SetOnExecutedCallback(CommandExecutedCallback cb, void* user) noexcept;

private:
    /**
     * m_UndoStack が上限を超えていれば最古の command を捨てる。
     *
     * @details PushBack 直後にのみ呼ぶ前提なので、超過量は高々 1 件。
     */
    void DropOldestIfOverflow() noexcept;

    /** undo の LIFO スタック (command の所有権を持つ)。 */
    TArray<TUniquePtr<AEditorCommand>> m_UndoStack {};

    /** redo の LIFO スタック (Push のたびに Clear される)。 */
    TArray<TUniquePtr<AEditorCommand>> m_RedoStack {};

    /** undo stack に積める件数の上限。 */
    u32                             m_MaxHistory = 64;

    /** Push / Undo / Redo 後に発火する callback (未設定なら nullptr)。 */
    CommandExecutedCallback         m_Cb          = nullptr;

    /** callback へ渡すユーザコンテキスト。 */
    void*                           m_CbUser     = nullptr;
};

using FUndoStack = CUndoStack;

} // namespace acs::game::editor_core
