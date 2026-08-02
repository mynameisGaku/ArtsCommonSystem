// SPDX-License-Identifier: Apache-2.0
// GameFramework Tools — editor_core / CUndoStack 実装
//
// 設計のポイント (詳細はヘッダ参照):
//   ・undo / redo は LIFO の `TArray<TUniquePtr<AEditorCommand>>` で表現する。
//   ・Push は所有権を奪い、Execute を実行、merge 判定し、redo stack を破棄する。
//   ・上限超は最古 1 件を捨てる (PushBack 後にしか発生しないので超過量 = 1)。
//   ・全 noexcept / STL 不使用 / 非コピー・非ムーブ。

#include "gameframework/tools/editor_core/UndoStack.h"
#include "gameframework/tools/editor_core/EditorCommand.h"
#include "memory/Memory.h"   // DefaultAllocator()
#include "foundation/Move.h"

namespace acs::game::editor_core {

/** 履歴を全破棄して上限を再設定する (多重 Init 安全、0 は 64 に丸める)。 */
void CUndoStack::Init(u32 max_history) noexcept {
    // 履歴を全破棄して上限を再設定する。多重 Init 安全。
    // 0 を渡された場合は default (64) にフォールバック (誤渡し対策)。
    m_UndoStack.Clear();
    m_RedoStack.Clear();
    m_MaxHistory = (max_history == 0u) ? 64u : max_history;
    // callback はあえて Init で解除しない (= editor 起動側で SetOnExecutedCallback
    // を Init より先に呼ぶケースを許容)。明示的に解除したい場合は
    // SetOnExecutedCallback(nullptr, nullptr) を呼ぶ。
}

/** command を 1 件積む (所有権を奪い、Execute → merge 判定 → redo 破棄を行う)。 */
void CUndoStack::Push(AEditorCommand* cmd) noexcept {
    IAllocator& allocator = DefaultAllocator();
    Push(TUniquePtr<AEditorCommand>(cmd, &allocator));
}

/** 生ポインタと確保元を受け取り、解放元を保持した所有権へ変換して積む。 */
void CUndoStack::Push(AEditorCommand* command, IAllocator& allocator) noexcept
{
    Push(TUniquePtr<AEditorCommand>(command, &allocator));
}

/** 解放元を保持した command の所有権を受け取り、履歴へ積む。 */
void CUndoStack::Push(TUniquePtr<AEditorCommand> owned) noexcept
{
    // 空 command は silent no-op とし、履歴や callback を変更しない。
    if (!owned) return;

    // 2. Execute を呼ぶ (= "Do action")。
    owned->Execute();

    // 3. 直前の cmd と merge 可能か判定する。merge できれば new cmd は
    //    そのまま破棄され、m_UndoStack 件数は増えない (= 連続 drag 1 件化)。
    //    merge 後の callback は「スタック top の方」を渡す (= 利用側が
    //    "最新状態" を取りに来たときに整合する)。
    bool merged = false;
    if (!m_UndoStack.IsEmpty()) {
        AEditorCommand* prev = m_UndoStack.Back().Get();
        if (prev != nullptr && prev->CanMerge(*owned)) {
            prev->MergeWith(*owned);
            merged = true;
        }
    }

    // 4. merge しなかった場合のみ m_UndoStack に PushBack。
    //    merged のときは owned が scope 抜けで自動 delete される。
    if (!merged) {
        m_UndoStack.PushBack(Move(owned));
    }

    // 5. new edit が来た時点で m_RedoStack の "未来" は破棄される。
    m_RedoStack.Clear();

    // 6. 上限超えチェック。merge 経路で件数は増えないので超過は起こらないが、
    //    防御的に毎回呼んでおく (空 m_UndoStack でも no-op)。
    DropOldestIfOverflow();

    // 7. callback 発火。merge があった場合は top (= 直前 cmd と同一 = merge 先) を
    //    渡す。merge しなかった場合も top (= 今 Push したばかりの cmd) を渡す。
    //    is_redo は false (= Push は初回 Execute、Redo 経路ではない)。
    if (m_Cb != nullptr && !m_UndoStack.IsEmpty()) {
        m_Cb(m_CbUser, m_UndoStack.Back().Get(), /*is_redo=*/false);
    }
}

/** undo を 1 件巻き戻し、その command を redo stack へ移す。 */
bool CUndoStack::Undo() noexcept {
    // 何も積まれていなければ no-op で false を返す (= UI 側で MenuItem の
    // enabled チェックがあっても、race 等で空のときに silent に弾ける)。
    if (m_UndoStack.IsEmpty()) {
        return false;
    }

    // top を取り出す。PopBack は破棄してしまうので、先に Move で別 TUniquePtr に
    // 移してから PopBack する。Move 後の Back() は moved-from 状態 (= 内部
    // pointer が nullptr) なので、PopBack の dtor は何もしない。
    TUniquePtr<AEditorCommand> cmd = Move(m_UndoStack.Back());
    m_UndoStack.PopBack();

    // Undo を実行。基底経由なので派生の override に dispatch される。
    if (cmd) {
        cmd->Undo();
    }

    // redo stack に押し込む (= 次の Redo で再実行可能になる)。
    // callback には Move 前の生ポインタを渡したいので raw を取ってから Move。
    AEditorCommand* raw = cmd.Get();
    m_RedoStack.PushBack(Move(cmd));

    // callback 発火。Undo は Redo 由来ではないので is_redo = false。
    if (m_Cb != nullptr) {
        m_Cb(m_CbUser, raw, /*is_redo=*/false);
    }
    return true;
}

/** redo を 1 件やり直し、その command を undo stack へ戻す (Undo の対称)。 */
bool CUndoStack::Redo() noexcept {
    if (m_RedoStack.IsEmpty()) {
        return false;
    }

    // Undo と対称的に redo stack の top を取り出す。
    TUniquePtr<AEditorCommand> cmd = Move(m_RedoStack.Back());
    m_RedoStack.PopBack();

    // Execute を再実行。AEditorCommand::Execute は idempotent な実装が前提
    // (Undo を挟まずに連続 Execute は CUndoStack 側で発生しない)。
    if (cmd) {
        cmd->Execute();
    }

    // undo stack に戻す (= 再度 Undo で巻き戻せる)。
    AEditorCommand* raw = cmd.Get();
    m_UndoStack.PushBack(Move(cmd));

    // callback 発火。Redo 経由なので is_redo = true。
    if (m_Cb != nullptr) {
        m_Cb(m_CbUser, raw, /*is_redo=*/true);
    }
    return true;
}

/** undo できる command があるかを返す。 */
bool CUndoStack::CanUndo() const noexcept {
    return !m_UndoStack.IsEmpty();
}

/** redo できる command があるかを返す。 */
bool CUndoStack::CanRedo() const noexcept {
    return !m_RedoStack.IsEmpty();
}

/** undo stack に積まれている件数を返す。 */
u32 CUndoStack::UndoCount() const noexcept {
    return static_cast<u32>(m_UndoStack.Size());
}

/** redo stack に積まれている件数を返す。 */
u32 CUndoStack::RedoCount() const noexcept {
    return static_cast<u32>(m_RedoStack.Size());
}

/** undo stack top の Description を返す (空なら "")。 */
const char* CUndoStack::UndoDescription() const noexcept {
    // 空のときは空文字列 (nullptr ではない → strlen / strcmp が安全)。
    if (m_UndoStack.IsEmpty()) {
        return "";
    }
    const AEditorCommand* top = m_UndoStack.Back().Get();
    if (top == nullptr) {
        return "";
    }
    const char* desc = top->Description();
    return (desc != nullptr) ? desc : "";
}

/** redo stack top の Description を返す (空なら "")。 */
const char* CUndoStack::RedoDescription() const noexcept {
    if (m_RedoStack.IsEmpty()) {
        return "";
    }
    const AEditorCommand* top = m_RedoStack.Back().Get();
    if (top == nullptr) {
        return "";
    }
    const char* desc = top->Description();
    return (desc != nullptr) ? desc : "";
}

/** undo / redo 両スタックの全 command を破棄する。 */
void CUndoStack::Clear() noexcept {
    // TUniquePtr の dtor で全 cmd が自動 delete される。TArray::Clear は size を
    // 0 に戻すだけで capacity は維持されるため、その後の Push で再アロケーション
    // 不要 (=「シーン切替時に Clear」のような典型用途で GC 負荷が出ない)。
    m_UndoStack.Clear();
    m_RedoStack.Clear();
}

/** Push / Undo / Redo 後に呼ぶ callback を登録する (nullptr で解除)。 */
void CUndoStack::SetOnExecutedCallback(CommandExecutedCallback cb, void* user) noexcept {
    // cb / user ともそのまま保存。nullptr 解除 OK (= cb は呼ばれなくなる)。
    m_Cb      = cb;
    m_CbUser = user;
}

/** 上限超過時に最古 1 件を捨てる (PushBack 直後にのみ呼ぶ、超過量は高々 1)。 */
void CUndoStack::DropOldestIfOverflow() noexcept {
    // m_UndoStack のみが上限管理対象 (redo は Push 時に Clear されるため自然に
    // 上限以下になる)。
    // 超過件数は最大でも 1 (= 直前の PushBack 1 件分のみ) を想定する。
    while (static_cast<u32>(m_UndoStack.Size()) > m_MaxHistory) {
        // 最古 = index 0 を捨てる。TArray は RemoveAt(0) を持たないので、
        // 自前で 1 つずつ前にずらしてから PopBack する。
        // ・要素は TUniquePtr<AEditorCommand> なのでムーブ可能。
        // ・PopBack 前に index 0 が末尾要素まで移送されていれば、PopBack の
        //   破棄で最古 cmd が dtor 経由で delete される。
        const usize n = m_UndoStack.Size();
        for (usize i = 1; i < n; ++i) {
            m_UndoStack[i - 1] = Move(m_UndoStack[i]);
        }
        m_UndoStack.PopBack();
    }
}

} // namespace acs::game::editor_core
