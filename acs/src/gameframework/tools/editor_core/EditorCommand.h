// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "foundation/Types.h"
#include "gameframework/Forward.h"
#include "gameframework/ANode.h"
#include "math/Vec.h"

namespace acs::game::editor_core {

/**
 * 全エディタが共有する undo/redo の原子単位 (純粋抽象)。
 *
 * @details
 * 1 操作を 1 AEditorCommand 派生インスタンスとして保持し、
 * 派生クラスは Execute / Undo / Description を必ず override する。
 * CanMerge / MergeWith は default で merge 拒否であり、連続 drag をまとめたい派生
 * のみ override する。ベース型を `TUniquePtr<AEditorCommand>` で CUndoStack に
 * 持たせるため virtual dtor を持ち、複製事故を防ぐため非コピー / 非ムーブとする。
 */
class AEditorCommand {
public:
    /** 空の command を構築する。 */
    AEditorCommand() noexcept          = default;

    /** 派生クラスを polymorphic delete するための仮想デストラクタ。 */
    virtual ~AEditorCommand() noexcept = default;

    /** コピー禁止 (実行済み command の複製は二重 Undo 等の事故源になるため)。 */
    AEditorCommand(const AEditorCommand&)            = delete;

    /** コピー代入も禁止。 */
    AEditorCommand& operator=(const AEditorCommand&) = delete;

    /** ムーブ禁止 (CUndoStack が TUniquePtr で単独所有するため)。 */
    AEditorCommand(AEditorCommand&&)                 = delete;

    /** ムーブ代入も禁止。 */
    AEditorCommand& operator=(AEditorCommand&&)      = delete;

    /**
     * RTTI を使わずに command の種類を識別するための tag を返す。
     *
     * @details
     * 派生クラスは自前の静的アドレス (static const char k; ... return &k;) を返す。
     * CanMerge / MergeWith は「Kind() ポインタが一致するか」を最低条件として派生で
     * 確認することで、不正な cross-type cast を防ぐ。
     * @return command 種類を一意に識別する静的アドレス (default は nullptr = 種類不明・merge 対象外)。
     */
    virtual const void* Kind() const noexcept { return nullptr; }

    /**
     * 「Do」操作を実行する。
     *
     * @details
     * CUndoStack::Push 直後と Redo 経路の両方から呼ばれる。Undo を挟まずに同じ
     * command を 2 回 Execute することは CUndoStack が排除する。
     */
    virtual void Execute() noexcept = 0;

    /**
     * Execute の逆操作を行う。
     *
     * @details Execute → Undo → Execute と往復可能な対称性が要件。
     */
    virtual void Undo() noexcept = 0;

    /**
     * 取り消し履歴 UI 表示用の短い人間可読ラベルを返す。
     *
     * @details
     * "Move Node" / "Add Component" 等。ImGui MenuItem 等にそのまま渡される想定の
     * ため、戻り値は永続文字列 (literal or 派生クラス内 char[N]) であること。
     * @return 取り消し履歴に表示する短いラベル。
     */
    virtual const char* Description() const noexcept = 0;

    /**
     * 直後に Push されようとしている command を自分にマージ可能か返す。
     *
     * @details
     * マージは「同じ対象 (= same target pointer / same FNodeId)」かつ「同じ command
     * kind」のときに限定する。派生クラスは dynamic_cast を使わず Kind() ポインタ比較で
     * 確認すること (ACS は RTTI を避ける)。default は false (= 毎回別エントリとして積む)。
     * @param next 直後に Push されようとしている command。
     * @return マージ可能なら true。
     */
    virtual bool CanMerge(const AEditorCommand& /*next*/) const noexcept {
        return false;
    }

    /**
     * CanMerge が true を返した直後に CUndoStack から呼ばれ、next を取り込む。
     *
     * @details
     * next の「new 値」を自分に取り込み (= 自分の new 値を next の new 値で上書きし)、
     * 自分の old 値はそのまま保つ。これで Undo 1 回で連続 drag 全体を巻き戻せる。
     * default は no-op (CanMerge が false なので呼ばれない想定)。
     * @param next マージ元となる直後の command。
     */
    virtual void MergeWith(const AEditorCommand& /*next*/) noexcept {}
};

/**
 * ANode の position を変更する AEditorCommand 派生実装。
 *
 * @details
 * ANode の位置変更を前後値で往復し、連続 drag を 1 件にまとめる CanMerge 実装も持つ。
 * 同じ m_Target (raw pointer 一致) かつ同じ kind のとき MergeWith で m_NewPos を next の
 * m_NewPos に置き換え、m_OldPos は最初の cmd のものを保つ (= Undo 1 回で初期位置に戻る)。
 * 対象ノードが既に Destroy 済みでも pointer 比較自体は安全だが、実 Execute / Undo 時に
 * 対象が dangling だと UB になるため、editor 側で Destroy 時に undo stack を Clear する等の
 * ハイレベルポリシーで防ぐ。
 */
class AMoveNodeCommand : public AEditorCommand {
public:
    /**
     * 移動対象と前後の position を保持して構築する。
     *
     * @param target 位置を書き換える対象ノード。
     * @param old_pos 変更前の位置 (Undo で復元する値)。
     * @param new_pos 変更後の位置 (Execute で適用する値)。
     */
    AMoveNodeCommand(ANode* target, FVec2 old_pos, FVec2 new_pos) noexcept
        : m_Target(target), m_OldPos(old_pos), m_NewPos(new_pos) {}

    /** 対象ノードの local position を new_pos に設定する。 */
    void Execute() noexcept override {
        if (m_Target != nullptr) {
            m_Target->SetPosition2D(m_NewPos);
        }
    }

    /** 対象ノードの local position を old_pos に戻す。 */
    void Undo() noexcept override {
        if (m_Target != nullptr) {
            m_Target->SetPosition2D(m_OldPos);
        }
    }

    /**
     * 取り消し履歴 UI 用のラベルを返す。
     *
     * @return 固定文字列 "Move Node"。
     */
    const char* Description() const noexcept override {
        return "Move Node";
    }

    /**
     * この派生型を一意に識別する kind tag を返す。
     *
     * @details next 側も同じアドレスを返せば同一派生型と確定できる (= RTTI 不要のポインタ比較)。
     * @return AMoveNodeCommand 固有の静的アドレス。
     */
    const void* Kind() const noexcept override { return KindTag(); }

    /**
     * 同一 target に対する連続 AMoveNodeCommand のみマージを許可する。
     *
     * @details Kind tag と target ポインタが両方一致するときだけ merge を認める。
     * @param next 直後に Push されようとしている command。
     * @return next が同型かつ同一 target なら true。
     */
    bool CanMerge(const AEditorCommand& next) const noexcept override {
        // 型 (Kind tag) と対象 (target ポインタ) が両方一致するときだけ merge。
        if (next.Kind() != KindTag()) {
            return false;
        }
        // ここまでくれば next は AMoveNodeCommand と確定 (Kind tag は static アドレス
        // で派生クラスを一意に識別している)。安全に static_cast 可能。
        const AMoveNodeCommand& nxt = static_cast<const AMoveNodeCommand&>(next);
        return nxt.m_Target == m_Target;
    }

    /**
     * next の new 値を取り込んで連続 drag を 1 件にまとめる。
     *
     * @details new 値だけ更新し、old 値 (= 連続 drag の始点) は保持する。防御的に Kind を再確認する。
     * @param next マージ元の command (CanMerge を通過済みの前提)。
     */
    void MergeWith(const AEditorCommand& next) noexcept override {
        // CanMerge を経ているのが CUndoStack 側の前提だが、防御的に再確認する。
        if (next.Kind() != KindTag()) {
            return;
        }
        const AMoveNodeCommand& nxt = static_cast<const AMoveNodeCommand&>(next);
        // new 値だけ更新し、old 値 (= 連続 drag の始点) は保持する。
        m_NewPos = nxt.m_NewPos;
    }

    /**
     * 移動対象ノードを返す。
     *
     * @return 対象ノード (未設定なら nullptr)。
     */
    const ANode* Target() const noexcept { return m_Target; }

    /**
     * 変更前の位置を返す。
     *
     * @return Undo で復元する位置。
     */
    FVec2          OldPosition() const noexcept { return m_OldPos; }

    /**
     * 変更後の位置を返す。
     *
     * @return Execute で適用する位置。
     */
    FVec2          NewPosition() const noexcept { return m_NewPos; }

private:
    /**
     * 派生固有の kind 識別用の静的アドレスを返す。
     *
     * @details
     * 内容は使わず、アドレスだけが ID。function-local static にすることで、ヘッダ多重
     * include + 複数 TU 跨ぎで「同一アドレス」を保証する (C++11 以降の保証)。
     * @return AMoveNodeCommand を識別する静的アドレス。
     */
    static const void* KindTag() noexcept {
        static const char kTag = 0;
        return &kTag;
    }

    /** 位置を書き換える対象ノード。 */
    ANode* m_Target  = nullptr;

    /** 変更前の位置 (Undo で復元)。 */
    FVec2    m_OldPos {};

    /** 変更後の位置 (Execute で適用)。 */
    FVec2    m_NewPos {};
};

using FMoveNodeCommand = AMoveNodeCommand;

} // namespace acs::game::editor_core
