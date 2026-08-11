// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "gameframework/Forward.h"
#include "gameframework/NodeId.h"

namespace acs::game::inspector {

/**
 * 選択変更を購読側へ通知するコールバック型。
 *
 * @details
 * 同じ selection を再 SelectNode した場合は呼ばれない (no-op)。
 * @param user RegisterCallback の第二引数で渡したコンテキストポインタ。
 * @param from 変更前の選択 (未選択時は invalid)。
 * @param to 変更後の選択 (ClearSelection 時は invalid)。
 */
using SelectionChangeCallback = void (*)(void* user, FNodeId from, FNodeId to) noexcept;

/**
 * 現在選択されている FNodeId を全 editor panel で共有する集中ハブ。
 *
 * @details
 * 1 個のシングルなインスタンスを editor 起動コードが所有し、各 panel が
 * RegisterCallback で選択変更を購読する。現在選択中の 1 ノードだけを保持し、
 * 複数選択は範囲外。FNodeId のみを扱い、選択対象が生きているか / どの AScene に
 * 属するかの検証は購読側の責務。non-copy / non-move、全 noexcept、STL 不使用。
 */
class CSelectionService {
public:
    /** 空状態で構築する (現選択は invalid、callback list は空)。 */
    CSelectionService() noexcept = default;

    /** 破棄する (内部 TArray が callback list を解放)。 */
    ~CSelectionService() noexcept = default;

    /** コピー禁止 (内部 callback 配列の所有を曖昧にしないため)。 */
    CSelectionService(const CSelectionService&)            = delete;

    /** コピー代入も禁止。 */
    CSelectionService& operator=(const CSelectionService&) = delete;

    /** ムーブ禁止。 */
    CSelectionService(CSelectionService&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CSelectionService& operator=(CSelectionService&&)      = delete;

    /**
     * 現選択を invalid に、callback list を空に戻して初期化する。
     *
     * @details 多重呼び出し可。TArray の容量は保持され、再 Init 後の再確保を節約する。
     */
    void Init() noexcept;

    /**
     * 現選択を id に切り替え、値が変化した場合のみ callback を一斉発火する。
     *
     * @details
     * invalid handle (= 既定構築 FNodeId{}) を渡すと ClearSelection と等価。
     * m_Current 更新後に dispatch するため、callback 内で CurrentSelection() を呼ぶと
     * 新値が見える。
     * @param id 新たに選択する FNodeId。
     */
    void SelectNode(FNodeId id) noexcept;

    /**
     * 現選択を invalid に戻す。
     *
     * @details 前選択が valid だった場合のみ callback を発火する。
     */
    void ClearSelection() noexcept;

    /**
     * 現選択を取得する。
     *
     * @return 現選択の FNodeId (未選択時は invalid handle、IsValid() == false)。
     */
    FNodeId CurrentSelection() const noexcept;

    /**
     * 指定 id が現選択と完全一致するかを返す。
     *
     * @details
     * 比較は packed u32 の完全一致 (index + generation 一致時のみ true)。invalid id を
     * 渡した場合、現選択が invalid なら true、それ以外なら false になる。
     * @param id 比較する FNodeId。
     * @return 現選択と一致すれば true。
     */
    bool IsSelected(FNodeId id) const noexcept;

    /**
     * (cb, user) ペアを登録する。
     *
     * @details 同一ペアの重複登録は no-op で弾く。cb が null の場合は登録せず no-op。
     * @param cb 選択変更時に呼ぶコールバック。
     * @param user cb の第一引数に渡すコンテキストポインタ。
     */
    void RegisterCallback(SelectionChangeCallback cb, void* user) noexcept;

    /**
     * (cb, user) に完全一致する登録 1 件を解除する。
     *
     * @details 該当なし、または cb が null の場合は no-op。
     * @param cb 解除するコールバック。
     * @param user 登録時に渡したコンテキストポインタ。
     */
    void UnregisterCallback(SelectionChangeCallback cb, void* user) noexcept;

    /**
     * 登録済みコールバック数を返す。
     *
     * @return 登録済み callback の件数。
     */
    u32 CallbackCount() const noexcept;

    /**
     * 全 callback 登録を破棄し、選択も invalid に戻す。
     *
     * @details
     * ClearSelection 相当の通知は行わない (shutdown 時の一括破棄想定)。購読側がもはや
     * listening しない前提で呼ぶため、dispatch すると dangling user pointer に飛ぶリスクが
     * あるためここでは発火しない。
     */
    void ClearAll() noexcept;

private:
    /**
     * (cb, user) ペアを表す POD エントリ (重複弾き + 順序保持に使う)。
     */
    struct FCallbackEntry {
        /** 選択変更時に呼ぶコールバック。 */
        SelectionChangeCallback cb   = nullptr;

        /** cb の第一引数に渡すコンテキストポインタ。 */
        void*                   user = nullptr;
    };

    /**
     * 登録順に全 callback へ選択変更を dispatch する共通ヘルパ。
     *
     * @details m_Current の更新は呼び出し側で完了させてから呼ぶこと。
     * @param from 変更前の選択。
     * @param to 変更後の選択。
     */
    void FireChange(FNodeId from, FNodeId to) const noexcept;

    /** 現選択 (default = invalid)。 */
    FNodeId               m_Current;

    /** 登録済み callback 群 (登録順を保持)。 */
    TArray<FCallbackEntry> m_Callbacks;
};

} // namespace acs::game::inspector
