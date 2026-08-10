// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "gameframework/TutorialStep.h"

namespace acs::game {

/**
 * ユーザー操作または明示前進で進む順序付きチュートリアルを管理する。
 *
 * 描画は行わず、現在ステップだけを公開する。無効な状態での前進や通知は何も行わない。
 */
class CTutorialFlow {
public:
    /** ステップ未登録の空フローを構築する。 */
    CTutorialFlow()  noexcept = default;

    /** フローと登録済みステップを破棄する。 */
    ~CTutorialFlow() noexcept = default;

    /** 進行状態の分裂を防ぐためコピー構築を禁止する。 */
    CTutorialFlow(const CTutorialFlow&)            = delete;

    /** 進行状態の分裂を防ぐためコピー代入を禁止する。 */
    CTutorialFlow& operator=(const CTutorialFlow&) = delete;

    /** 進行状態の参照先を固定するためムーブ構築を禁止する。 */
    CTutorialFlow(CTutorialFlow&&)                 = delete;

    /** 進行状態の参照先を固定するためムーブ代入を禁止する。 */
    CTutorialFlow& operator=(CTutorialFlow&&)      = delete;

    /**
     * 指定ステップを末尾へ追加する。
     *
     * 実行中に追加しても現在位置は変更しない。
     */
    void AddStep(const FTutorialStep& step) noexcept;

    /**
     * 先頭ステップから進行を開始する。
     *
     * ステップが無い場合は即座に完了状態へ移る。
     */
    void Start() noexcept;

    /**
     * 現在ステップを完了して次へ進める。
     *
     * 開始前、完了後、またはスキップ後は何も行わない。
     */
    void AdvanceStep() noexcept;

    /**
     * 実行された操作を現在ステップへ通知する。
     *
     * action_id が nullptr、待機中でない、または識別子が不一致の場合は何も行わない。
     */
    void NotifyAction(const char* action_id) noexcept;

    /** 登録済みステップを保ったまま開始前の状態へ戻す。 */
    void Reset() noexcept;

    /** フローをスキップして完了状態へ移す。完了後の呼び出しは何も行わない。 */
    void Skip() noexcept;

    /** 開始後かつ未完了の場合に true を返す。 */
    bool IsActive() const noexcept { return m_Active; }

    /** 全ステップ通過後またはスキップ後に true を返す。 */
    bool IsCompleted() const noexcept { return m_Completed; }

    /** 現在のステップ番号を返す。開始前は 0 を返す。 */
    u32 CurrentStepIndex() const noexcept { return m_CurrentStep; }

    /** 進行中のステップを返す。開始前、完了後、スキップ後は nullptr を返す。 */
    const FTutorialStep* CurrentStep() const noexcept;

    /** 登録済みステップ数を返す。 */
    u32 StepCount() const noexcept;

    /**
     * 毎フレームの経過秒を受け取る。
     *
     * 現在は時間経過による自動遷移を行わない。
     */
    void Tick(f32 dt) noexcept;

private:
    /** 登録済みステップ列。 */
    TArray<FTutorialStep> m_Steps;

    /** 現在のステップ番号。 */
    u32                 m_CurrentStep = 0;

    /** 進行中なら true。 */
    bool                m_Active       = false;

    /** 完了済みなら true。 */
    bool                m_Completed    = false;
};

/** 旧公開名を維持する互換別名。 */
using FTutorialFlow = CTutorialFlow;

} // namespace acs::game
