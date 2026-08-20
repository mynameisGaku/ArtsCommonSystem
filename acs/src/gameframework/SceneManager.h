// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — FSceneManager
//
// TUniquePtr<FScene> のスタック。top のシーンを毎フレーム update/render する。
// 遷移要求 (Change/Push/Pop) はフレーム境界まで遅延 → 走査中 (Update/Render
// 内) からの構造変更が安全。1 フレーム 1 遷移、複数要求が来た場合は後勝ち。
//
// 機能:
//   ・3 種の遷移 (Change/Push/Pop) と pending state machine
//   ・OnEvent は top のみへ配送
//   ・退場 FScene を **3 フレーム保持** (= フレームインフライト 2 + 1) する ring
//     buffer。GPU が直前フレームで参照中のリソースの use-after-free を防ぐ
//   ・Push 時に旧 top の OnPause、Pop 時に新 top の OnResume を呼ぶ
//   ・OnFixedUpdate を FGame の固定時計から実行アダプター経由で呼び込む
#pragma once

#include "container/Array.h"
#include "memory/UniquePtr.h"
#include "gameframework/Scene.h"

namespace acs::game {

class FGame;
class FRenderContext;
class IInputStateView;
struct FFixedStepInputBufferSnapshot;

/**
 * TUniquePtr<FScene> のスタックを管理し、top のシーンを毎フレーム駆動するマネージャ。
 *
 * @details
 * 遷移要求 (Change/Push/Pop) はフレーム境界まで遅延し、実行アダプターで適用するため、
 * 走査中 (Update/Render 内) からの構造変更が安全。1 フレーム 1 遷移で、複数要求が来た場合は
 * 後勝ち。退場した FScene は GPU が直前フレームで参照中のリソースを use-after-free しないよう、
 * ring buffer で 3 フレーム (フレームインフライト 2 + 1) 保持してから破棄する。Push 時には
 * 旧 top の OnPause、Pop 時には新 top の OnResume を呼ぶ。
 */
class FSceneManager {
public:
    /** 空のシーンスタックを構築する。 */
    FSceneManager() noexcept = default;

    /** マネージャを破棄する (残ったシーンは TUniquePtr が解放)。 */
    ~FSceneManager() noexcept = default;

    /** コピー禁止 (シーンを単独所有するため)。 */
    FSceneManager(const FSceneManager&) = delete;

    /** コピー代入も禁止。 */
    FSceneManager& operator=(const FSceneManager&) = delete;

    /**
     * 現 top を pop して next を push する遷移を要求する (= 単純な画面切替)。
     *
     * @details 即時適用せず次フレーム頭に実行アダプターで適用する。pending が既に立っていれば
     * 上書きする (後勝ち)。next が nullptr の場合は警告ログを出して無視する。
     * @param next 切り替え先のシーン (所有権が移る)。
     */
    void ChangeScene(TUniquePtr<FScene> next) noexcept;

    /**
     * 現 top をスタック上に残したまま next を push する遷移を要求する (= モーダル/ダイアログ)。
     *
     * @details 適用時に旧 top の OnPause が呼ばれる。next が nullptr の場合は警告ログを出して無視する。
     * @param next 上に重ねるシーン (所有権が移る)。
     */
    void PushScene(TUniquePtr<FScene> next) noexcept;

    /** top を pop する遷移を要求する (スタックが 1 枚以下なら適用時に何もせず警告)。 */
    void PopScene() noexcept;

    /**
     * 現在の top シーンを返す。
     *
     * @return top のシーン (スタックが空なら nullptr)。
     */
    FScene* Top() const noexcept;

    /**
     * スタックに積まれたシーン数を返す。
     *
     * @return スタックの深さ。
     */
    u32 Depth() const noexcept;

    /**
     * active sceneが切り替わるたびに進むprocess内epochを返す。
     * @return 現在のepoch。0は世代を使い切りsnapshot照合不能になった状態。
     */
    u64 ActiveSceneEpoch() const noexcept
    {
        return m_ActiveSceneEpoch;
    }

    /**
     * スタックが空かを返す。
     *
     * @return 1 枚もシーンが無ければ true。
     */
    bool IsEmpty() const noexcept
    {
        return m_Stack.IsEmpty();
    }

    /**
     * active scene の未消費固定入力を保存値へ複製する。
     * @param snapshot 保存先。Input サービスが無い場合は未初期化状態を返す。
     * @return 状態を取得できた場合は true。失敗時は snapshot を変更しない。
     */
    bool TryCaptureActiveFixedInputSnapshot(FFixedStepInputBufferSnapshot& snapshot) const noexcept;

    /**
     * active scene の未消費固定入力を保存値から復元する。
     * @param snapshot 復元する入力。Input サービスが無い場合は未初期化状態だけを受理する。
     * @return 状態を復元できた場合は true。失敗時は active scene を変更しない。
     */
    bool TryRestoreActiveFixedInputSnapshot(const FFixedStepInputBufferSnapshot& snapshot) noexcept;

    /** 退場 FScene を保持するフレーム数 (= フレームインフライト 2 + 1)。 */
    static constexpr u32 kRetireRingSize = 3;

private:
    /** active scene境界の世代を進め、使い切った場合は照合不能な0へ移す。 */
    void AdvanceActiveSceneEpoch_Internal() noexcept;

    /** フレーム頭で保留中の遷移を適用する内部処理。 */
    void ApplyPending_Internal(FGame& game) noexcept;

    /** top シーンへ一フレーム分の入力を提出する内部処理。 */
    bool SubmitFrameInput_Internal(const IInputStateView& input) noexcept;

    /** top シーンの未消費固定入力を初期化する内部処理。 */
    void ResetFixedInput_Internal() noexcept;

    /** top のシーンへ可変刻み更新を流す内部処理。 */
    void Update_Internal(f32 dt) noexcept;

    /** top のシーンへ固定刻み更新を流す内部処理。 */
    void FixedUpdate_Internal(f32 fixed_dt) noexcept;

    /** top のシーンを描画する内部処理。 */
    void Render_Internal(FRenderContext& rc) noexcept;

    /** top のシーンへイベントを配送する内部処理。 */
    void DispatchEvent_Internal(const FEvent& e) noexcept;

    /** 全シーンを終了して破棄する内部処理。 */
    void ShutdownAll_Internal() noexcept;

public:
    /** 内部駆動処理を明示的に呼び出す非所有アダプター。 */
    class FExecutionAdapter final {
    public:
        /** 呼び出し先のシーン管理器を保持する。 */
        explicit FExecutionAdapter(FSceneManager& manager) noexcept : m_Manager(manager)
        {
        }

        /** 保留中のシーン遷移をフレーム境界で適用する。 */
        void ApplyPending(FGame& game) noexcept
        {
            m_Manager.ApplyPending_Internal(game);
        }

        /** top シーンへ一フレーム分の入力を提出する。 */
        bool SubmitFrameInput(const IInputStateView& input) noexcept
        {
            return m_Manager.SubmitFrameInput_Internal(input);
        }

        /** top シーンの未消費固定入力を初期化する。 */
        void ResetFixedInput() noexcept
        {
            m_Manager.ResetFixedInput_Internal();
        }

        /** top のシーンへ可変刻み更新を流す。 */
        void Update(f32 delta_seconds) noexcept
        {
            m_Manager.Update_Internal(delta_seconds);
        }

        /** top のシーンへ固定刻み更新を流す。 */
        void FixedUpdate(f32 fixed_delta_seconds) noexcept
        {
            m_Manager.FixedUpdate_Internal(fixed_delta_seconds);
        }

        /** top のシーンを描画する。 */
        void Render(FRenderContext& context) noexcept
        {
            m_Manager.Render_Internal(context);
        }

        /** top のシーンへイベントを配送する。 */
        void DispatchEvent(const FEvent& event) noexcept
        {
            m_Manager.DispatchEvent_Internal(event);
        }

        /** 全シーンを終了して破棄する。 */
        void ShutdownAll() noexcept
        {
            m_Manager.ShutdownAll_Internal();
        }

    private:
        /** 呼び出し先のシーン管理器。 */
        FSceneManager& m_Manager;
    };

    /** 実行ループ用アダプターを返す。 */
    FExecutionAdapter ExecutionAccess() noexcept
    {
        return FExecutionAdapter(*this);
    }

private:
    /** 保留中の遷移種別。 */
    enum class EOp : u8 {
        None,
        Change,
        Push,
        Pop
    };

    /**
     * 内部 push 処理。next に context/services を attach し、OnEnter を呼ぶ。
     *
     * @param game シーンに紐付ける FGame コンテキスト。
     * @param next push するシーン (所有権が移る)。
     * @param pause_current true なら push 前に旧 top の OnPause を呼ぶ (Change は false、Push は true)。
     */
    void DoPushInternal(FGame& game, TUniquePtr<FScene> next, bool pause_current) noexcept;

    /**
     * 内部 pop 処理。top の OnExit を呼び、ring buffer へ退避してからスタックから外す。
     *
     * @param resume_new true なら pop 後に新 top の OnResume を呼ぶ (Change は false、Pop は true)。
     */
    void DoPopInternal(bool resume_new) noexcept;

    /** シーンスタック (top = Back())。 */
    TArray<TUniquePtr<FScene>> m_Stack;

    /** 保留中の遷移種別。 */
    EOp m_PendingOp = EOp::None;

    /** Change/Push で push する next シーン (Pop では未使用)。 */
    TUniquePtr<FScene> m_PendingArg;

    /** GPU 遅延削除のための退場 FScene ring buffer。 */
    TUniquePtr<FScene> m_Retired[kRetireRingSize];

    /** ring buffer の現在ヘッド (次に release するスロット)。 */
    u32 m_RetireHead = 0;

    /** active sceneの実効的な切替ごとに進む世代。0は使い切りを表す。 */
    u64 m_ActiveSceneEpoch = 1u;
};

} // namespace acs::game
