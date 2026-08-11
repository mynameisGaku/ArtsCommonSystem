// SPDX-License-Identifier: Apache-2.0
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
     * と CanMerge / MergeWith できれば Add せず merge して破棄し (連続 drag の
     * 1 件化)、そうでなければ m_UndoStack に Add する。続けて m_RedoStack を
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
    void Push(AEditorCommand* command, IAllocator& allocator) noexcept;

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
     * @details Add 直後にのみ呼ぶ前提なので、超過量は高々 1 件。
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
