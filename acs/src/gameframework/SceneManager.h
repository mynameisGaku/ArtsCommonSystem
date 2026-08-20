// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — CSceneManager
//
// TUniquePtr<AScene> のスタック。top のシーンを毎フレーム update/render する。
// 遷移要求 (Change/Push/Pop) はフレーム境界まで遅延 → 走査中 (Update/Render
// 内) からの構造変更が安全。1 フレーム 1 遷移、複数要求が来た場合は後勝ち。
//
// 機能:
//   ・3 種の遷移 (Change/Push/Pop) と pending state machine
//   ・OnEvent は top のみへ配送
//   ・退場 AScene を **3 フレーム保持** (= フレームインフライト 2 + 1) する ring
//     buffer。GPU が直前フレームで参照中のリソースの use-after-free を防ぐ
//   ・Push 時に旧 top の OnPause、Pop 時に新 top の OnResume を呼ぶ
//   ・OnFixedUpdate を CGame の固定時計から実行アダプター経由で呼び込む
#pragma once

#include "container/Array.h"
#include "memory/UniquePtr.h"
#include "gameframework/Forward.h"
#include "gameframework/Scene.h"

namespace acs::game {

class FRenderContext;
class IInputStateView;
struct FFixedStepInputBufferSnapshot;

/**
 * TUniquePtr<AScene> のスタックを管理し、top のシーンを毎フレーム駆動するマネージャ。
 *
 * @details
 * 遷移要求 (Change/Push/Pop) はフレーム境界まで遅延し、実行アダプターで適用するため、
 * 走査中 (Update/Render 内) からの構造変更が安全。1 フレーム 1 遷移で、複数要求が来た場合は
 * 後勝ち。退場した AScene は GPU が直前フレームで参照中のリソースを use-after-free しないよう、
 * ring buffer で 3 フレーム (フレームインフライト 2 + 1) 保持してから破棄する。Push 時には
 * 旧 top の OnPause、Pop 時には新 top の OnResume を呼ぶ。
 */
class CSceneManager {
public:
    /** 空のシーンスタックを構築する。 */
    CSceneManager() noexcept = default;

    /** マネージャを破棄する (残ったシーンは TUniquePtr が解放)。 */
    ~CSceneManager() noexcept = default;

    /** コピー禁止 (シーンを単独所有するため)。 */
    CSceneManager(const CSceneManager&)            = delete;

    /** コピー代入も禁止。 */
    CSceneManager& operator=(const CSceneManager&) = delete;

    /**
     * 現 top を pop して next を push する遷移を要求する (= 単純な画面切替)。
     *
     * @details 即時適用せず次フレーム頭の実行アダプターで実行する。pending が既に立っていれば
     * 上書きする (後勝ち)。next が nullptr の場合は警告ログを出して無視する。
     * @param next 切り替え先のシーン (所有権が移る)。
     */
    void ChangeScene(TUniquePtr<AScene> next) noexcept;

    /**
     * travel context を添えて Change 遷移を要求する。
     *
     * @details context は遷移が適用される瞬間に next へ引き渡され、以降は next が所有する
     * (`AScene::TravelContext<T>()` で読む)。遷移が失敗した場合や、適用前に別の要求で
     * 上書きされた場合、この context は破棄される。
     * @param next 切り替え先のシーン (所有権が移る)。
     * @param context 次のシーンへ持っていく任意データ (所有権が移る。nullptr 可)。
     */
    void ChangeScene(TUniquePtr<AScene> next,
                     TUniquePtr<CSceneTravelContext> context) noexcept;

    /**
     * 現 top をスタック上に残したまま next を push する遷移を要求する (= モーダル/ダイアログ)。
     *
     * @details 適用時に旧 top の OnPause が呼ばれる。next が nullptr の場合は警告ログを出して無視する。
     * @param next 上に重ねるシーン (所有権が移る)。
     */
    void PushScene(TUniquePtr<AScene> next) noexcept;

    /**
     * travel context を添えて Push 遷移を要求する。
     *
     * @param next 上に重ねるシーン (所有権が移る)。
     * @param context 次のシーンへ持っていく任意データ (所有権が移る。nullptr 可)。
     */
    void PushScene(TUniquePtr<AScene> next,
                   TUniquePtr<CSceneTravelContext> context) noexcept;

    /** top を pop する遷移を要求する (スタックが 1 枚以下なら適用時に何もせず警告)。 */
    void PopScene() noexcept;

    /**
     * travel context を添えて Pop 遷移を要求する (モーダルの «結果» を戻す用途)。
     *
     * @details context は pop 後に top へ戻るシーンが受け取る。pop が実行されなかった場合
     * (スタックが 1 枚以下) は破棄される。
     * @param context 戻り先のシーンへ渡す任意データ (所有権が移る。nullptr 可)。
     */
    void PopScene(TUniquePtr<CSceneTravelContext> context) noexcept;

    /**
     * 現在の top シーンを返す。
     *
     * @return top のシーン (スタックが空なら nullptr)。
     */
    AScene* Top()   const noexcept;

    /**
     * スタックに積まれたシーン数を返す。
     *
     * @return スタックの深さ。
     */
    u32     Depth() const noexcept;

    /** active sceneが切り替わるたびに進むprocess内epochを返す。 */
    u64 ActiveSceneEpoch() const noexcept
    {
        return m_ActiveSceneEpoch;
    }

    /**
     * スタックが空かを返す。
     *
     * @return 1 枚もシーンが無ければ true。
     */
    bool    IsEmpty() const noexcept { return m_Stack.IsEmpty(); }

    /** active sceneの未消費固定入力を保存値へ複製する。 */
    bool TryCaptureActiveFixedInputSnapshot(FFixedStepInputBufferSnapshot& snapshot) const noexcept;

    /** active sceneの未消費固定入力を保存値から復元する。 */
    bool TryRestoreActiveFixedInputSnapshot(const FFixedStepInputBufferSnapshot& snapshot) noexcept;

    /** 内部駆動処理を明示的に呼び出す非所有アダプター。 */
    class FExecutionAdapter final {
    public:
        /** 呼び出し先のシーン管理器を保持する。 */
        explicit FExecutionAdapter(CSceneManager& manager) noexcept : m_Manager(manager)
        {
        }

        /** 保留中のシーン遷移をフレーム境界で適用する。 */
        void ApplyPending(CGame& game) noexcept
        {
            m_Manager.ApplyPending_Internal(game);
        }

        /** topシーンへ一フレーム分の入力を提出する。 */
        bool SubmitFrameInput(const IInputStateView& input) noexcept
        {
            return m_Manager.SubmitFrameInput_Internal(input);
        }

        /** topシーンの未消費固定入力を初期化する。 */
        void ResetFixedInput() noexcept
        {
            m_Manager.ResetFixedInput_Internal();
        }

        /** topシーンへ可変刻み更新を流す。 */
        void Update(f32 scaled_delta_seconds, f32 unscaled_delta_seconds, u64 frame_number) noexcept
        {
            m_Manager.Update_Internal(scaled_delta_seconds, unscaled_delta_seconds, frame_number);
        }

        /** topシーンへ固定刻み更新を流す。 */
        void FixedUpdate(f32 fixed_delta_seconds) noexcept
        {
            m_Manager.FixedUpdate_Internal(fixed_delta_seconds);
        }

        /** topシーンを描画する。 */
        void Render(FRenderContext& context) noexcept
        {
            m_Manager.Render_Internal(context);
        }

        /** topシーンへイベントを配送する。 */
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
        CSceneManager& m_Manager;
    };

    /** 実行ループ用アダプターを返す。 */
    FExecutionAdapter ExecutionAccess() noexcept
    {
        return FExecutionAdapter(*this);
    }

    /** 退場 AScene を保持するフレーム数 (= フレームインフライト 2 + 1)。 */
    static constexpr u32 kRetireRingSize = 3;

private:
    /** active scene境界の世代を進め、使い切った場合は照合不能な0へ移す。 */
    void AdvanceActiveSceneEpoch_Internal() noexcept;

    /** フレーム頭で保留中の遷移を適用する内部処理。 */
    void ApplyPending_Internal(CGame& game) noexcept;

    /** topシーンへ一フレーム分の入力を提出する内部処理。 */
    bool SubmitFrameInput_Internal(const IInputStateView& input) noexcept;

    /** topシーンの未消費固定入力を初期化する内部処理。 */
    void ResetFixedInput_Internal() noexcept;

    /** topシーンへ可変刻み更新を流す内部処理。 */
    void Update_Internal(f32 scaled_delta_seconds, f32 unscaled_delta_seconds, u64 frame_number) noexcept;

    /** topシーンへ固定刻み更新を流す内部処理。 */
    void FixedUpdate_Internal(f32 fixed_delta_seconds) noexcept;

    /** topシーンを描画する内部処理。 */
    void Render_Internal(FRenderContext& context) noexcept;

    /** topシーンへイベントを配送する内部処理。 */
    void DispatchEvent_Internal(const FEvent& event) noexcept;

    /** 全シーンを終了して破棄する内部処理。 */
    void ShutdownAll_Internal() noexcept;

    /** 保留中の遷移種別。 */
    enum class EOp : u8 { None, Change, Push, Pop };

    /**
     * 内部 push 処理。next に context/services を attach し、OnEnter を呼ぶ。
     *
     * @param game シーンに紐付ける CGame コンテキスト。
     * @param next push するシーン (所有権が移る)。
     * @param pause_current true なら push 前に旧 top の OnPause を呼ぶ (Change は false、Push は true)。
     */
    bool PrepareScene(CGame& Game, AScene& Scene) noexcept;

    /** 準備済み scene を stack へ移し、必要なら旧 top を一時停止する。 */
    bool CommitPush(TUniquePtr<AScene> Scene, bool PauseCurrent) noexcept;

    /**
     * 内部 pop 処理。top の OnExit を呼び、ring buffer へ退避してからスタックから外す。
     *
     * @param resume_new true なら pop 後に新 top の OnResume を呼ぶ (Change は false、Pop は true)。
     * @param context 戻り先の top へ渡す travel context (無ければ空。OnResume より前に差し込む)。
     */
    void DoPopInternal(bool resume_new,
                       TUniquePtr<CSceneTravelContext> context) noexcept;

    /** シーンスタック (top = Back())。 */
    TArray<TUniquePtr<AScene>> m_Stack;

    /** 保留中の遷移種別。 */
    EOp                      m_PendingOp   = EOp::None;

    /** Change/Push で push する next シーン (Pop では未使用)。 */
    TUniquePtr<AScene>       m_PendingArg;

    /** 保留中の遷移に添えられた travel context (適用時に遷移先のシーンへ渡す)。 */
    TUniquePtr<CSceneTravelContext> m_PendingContext;

    /** GPU 遅延削除のための退場 AScene ring buffer。 */
    TUniquePtr<AScene>       m_Retired[kRetireRingSize];

    /** ring buffer の現在ヘッド (次に release するスロット)。 */
    u32 m_RetireHead = 0;

    /** active sceneの実効的な切替ごとに進む世代。0は使い切りを表す。 */
    u64 m_ActiveSceneEpoch = 1u;
};

} // namespace acs::game

namespace acs {

/** scene描画コンテキストをトップレベルから参照する正規入口。 */
using game::FRenderContext;

} // namespace acs
