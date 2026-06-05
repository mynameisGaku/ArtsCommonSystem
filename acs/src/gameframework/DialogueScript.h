// SPDX-License-Identifier: Apache-2.0
// GameFramework ジャンルキット (visual novel) — FDialogueScript
//
// VN 風のシーンスクリプトを再生するための state holder。
// 「セリフ → ポートレート表示 → BGM 切替 → 選択肢 → ジャンプ」といった
// アドベンチャー / ノベルゲームの典型的なフローを、命令列 (ScriptOp[]) と
// ラベルテーブルで宣言的に組み立てる。
//
// 役割:
//   ・命令列の保持と「現在 op_index」のステートマシン進行
//   ・ラベル → op_index 解決による Jump / 選択肢分岐
//   ・Say で「次へ」入力待ち (AwaitingInput) / Choice で選択待ち (AwaitingChoice) /
//     Wait で時間経過待ち (Playing 継続 + 内部タイマ)
//   ・実描画 / 音 / 入力には触らない: 各 op 種別は callback で外部に通知し、
//     ポートレート切替 / BGM 再生 / 選択肢 UI 表示は caller (Scene / UI 層 /
//     FAudioDirector) の責任とする (FDialogueSystem / FCinematicsDirector と
//     同じ「副作用ゼロ + callback 駆動」方針)。
//
// 設計選択:
//   ・**文字列を所有しない**: ScriptOp::arg1 / arg2 は const char* のまま。
//     スクリプトデータは literal / バンドル等で別管理する想定 (STL <string>
//     禁止 / TArray<char> での deep copy も避けて allocator フリーに保つ)。
//   ・**op_count + ops ポインタを丸ごと受け取る**: LoadScript はポインタを
//     コピーせず内部 TArray に複製する (= caller が ops を解放しても安全)。
//     ScriptOp は POD なので TArray<ScriptOp> での bulk copy は trivial。
//   ・**ラベルは別 TArray で線形検索**: 典型 N < 100 なので OK。同名ラベル
//     登録時は最初の登録のみ有効 (= 上書き禁止)。
//   ・**現在の選択肢は別 TArray に展開**: Choice op を踏んだ際に、後続の
//     Choice op を「同じ Say の選択肢群」として束ねるのは仕様が複雑になる
//     ので避ける。代わりに Choice op 1 個が arg1=label / arg2=jump_label の
//     ペアを 1 件持つ。実用上は同じ ScriptChoice 配列を複数 op で並べる
//     と無駄なので、Choice op は 1 個で「選択肢群を提示」を表現する仕様に
//     倒す: arg_u を 「次に続く Choice op の本数」(= group size) として扱い、
//     SelectChoice() が消費する。
//     -> 簡素化: 「Choice op = 選択肢 1 件」、AwaitingChoice 中は
//        連続する Choice op 群を m_CurrentChoices に展開しておき、
//        SelectChoice(idx) で jump_label に飛ぶ。
//   ・**callback は kind 別に分ける**: Say / Show・Hide / Background /
//     PlayBgm・StopBgm / PlaySe / ChoicePresent / End の 6 種に分割。
//     FCinematicsDirector と同じく汎用 1 個に集約しない方針。
//   ・**Wait op は AwaitingInput には遷移しない**: arg_f 秒経過で自動進行。
//     Say op の末尾で AwaitingInput になり、Advance() で次へ進む契約。
//   ・**非コピー・非ムーブ**: 現在 op_index / state の唯一性を担保するため。
//
// 参考: FDialogueSystem (タイプライタ + 選択肢)、FCinematicsDirector (timeline)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

/**
 * スクリプト命令の種別。
 *
 * @details
 * 各 kind が ScriptOp のどの引数を使うかは値ごとのコメントを参照
 * (arg1 / arg2 = const char*、arg_f = 数値引数)。
 */
enum class EScriptOpKind : u8 {
    /** セリフ表示 (arg1=speaker, arg2=text)。AwaitingInput へ遷移。 */
    Say        = 0,

    /** キャラポートレート表示 (arg1=character_id, arg2=sprite_id)。 */
    Show       = 1,

    /** キャラポートレート非表示 (arg1=character_id)。 */
    Hide       = 2,

    /** 背景切替 (arg1=bg_id)。 */
    Background = 3,

    /** BGM 再生 (arg1=bgm_id, arg_f=volume)。 */
    PlayBgm    = 4,

    /** BGM 停止。 */
    StopBgm    = 5,

    /** SE 再生 (arg1=se_id, arg_f=volume)。 */
    PlaySe     = 6,

    /** 選択肢提示 (arg1=label, arg2=jump_label)。 */
    Choice     = 7,

    /** 時間経過待ち (arg_f=seconds)。経過で自動進行。 */
    Wait       = 8,

    /** 指定ラベルへジャンプ (arg1=label)。 */
    Jump       = 9,

    /** スクリプト終了 (End callback 発火)。 */
    EndScene   = 10,
};

/**
 * 1 つのスクリプト命令。
 *
 * @details 文字列は所有しない (literal / バンドル参照)。POD なので bulk copy は trivial。
 */
struct ScriptOp {
    /** 命令の種別。 */
    EScriptOpKind kind  = EScriptOpKind::Say;

    /** kind に応じた第 1 引数 (speaker / character_id / bgm_id 等)。 */
    const char*  arg1  = nullptr;

    /** kind に応じた第 2 引数 (text / sprite_id / jump_label 等)。 */
    const char*  arg2  = nullptr;

    /** 数値引数 (volume / wait 秒数)。 */
    f32          arg_f = 0.0f;

    /** 補助 u32 引数 (将来拡張用、未使用なら 0)。 */
    u32          arg_u = 0u;
};

/**
 * 1 つの選択肢。
 *
 * @details
 * Choice op 群から AwaitingChoice 時に展開される。文字列は所有しない
 * (= 元 ScriptOp の arg1 / arg2 を参照)。
 */
struct ScriptChoice {
    /** UI に表示する選択肢ラベル。 */
    const char* label      = nullptr;

    /** 選択時のジャンプ先ラベル名。 */
    const char* jump_label = nullptr;
};

/**
 * スクリプト全体の状態。
 */
enum class EDialogueScriptState : u8 {
    /** LoadScript 直後 / Stop 後 (Start 前)。 */
    Idle           = 0,

    /** op 進行中 (Wait / 即座に進む系の op を処理中)。 */
    Playing        = 1,

    /** Say op 完了後、Advance 待ち。 */
    AwaitingInput  = 2,

    /** Choice op 群を展開済み、SelectChoice 待ち。 */
    AwaitingChoice = 3,

    /** EndScene op に到達、または op 列を末尾まで実行完了。 */
    Finished       = 4,
};

/** Say op の通知 callback (speaker / text を渡す、void* user は登録時の文脈)。 */
using SayCallback           = void(*)(void* user, const char* speaker, const char* text) noexcept;

/** Show / Hide op の通知 callback (character_id / sprite_id を渡す)。 */
using ShowHideCallback      = void(*)(void* user, const char* character_id, const char* sprite_id) noexcept;

/** BGM / SE op の通知 callback (audio_id / volume を渡す)。 */
using BgmSeCallback         = void(*)(void* user, const char* audio_id, f32 volume) noexcept;

/** Background op の通知 callback (bg_id を渡す)。 */
using BackgroundCallback    = void(*)(void* user, const char* bg_id) noexcept;

/** Choice op 群の提示 callback (選択肢配列とその本数を渡す)。 */
using ChoicePresentCallback = void(*)(void* user, const ScriptChoice* choices, u32 count) noexcept;

/** スクリプト終了の通知 callback (script_id を渡す)。 */
using EndCallback           = void(*)(void* user, const char* script_id) noexcept;

/**
 * VN 風のシーンスクリプトを再生する state holder。
 *
 * @details
 * 命令列 (ScriptOp[]) とラベルテーブルで「セリフ → ポートレート表示 → BGM 切替 →
 * 選択肢 → ジャンプ」といったノベルゲームのフローを宣言的に組み立てる。実描画 /
 * 音 / 入力には触れず、op 種別ごとに callback で外部へ通知する。Say で
 * AwaitingInput、Choice で AwaitingChoice、Wait で時間経過待ち (Playing 継続) と
 * なり、EndScene / 末尾到達で Finished に遷移する。文字列は所有せず、非コピー・
 * 非ムーブ。
 */
class FDialogueScript {
public:
    /** 空状態で構築する。 */
    FDialogueScript() noexcept = default;

    /** 破棄する。 */
    ~FDialogueScript() noexcept = default;

    /** コピー禁止 (進行状態の唯一性を担保するため)。 */
    FDialogueScript(const FDialogueScript&)            = delete;

    /** コピー代入も禁止。 */
    FDialogueScript& operator=(const FDialogueScript&) = delete;

    /** ムーブ禁止 (進行状態の唯一性を担保するため)。 */
    FDialogueScript(FDialogueScript&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FDialogueScript& operator=(FDialogueScript&&)      = delete;

    /**
     * 進行状態を既定値に戻す。
     *
     * @details 内部状態のみ。callback も含めてリセットしたい場合は ClearAll を使う。
     */
    void Init() noexcept;

    /**
     * op 列をロードする。
     *
     * @details
     * ops / script_id は所有しない (literal / バンドル参照、ops は内部 TArray へ
     * deep copy するので caller が解放しても安全)。既存のスクリプトとラベル
     * テーブルは破棄され State は Idle に遷移する。ops == nullptr or op_count == 0
     * でも安全 (= 空スクリプト、Start で即 Finished)。
     * @param ops ロードする命令配列。
     * @param op_count ops の要素数。
     * @param script_id スクリプト識別子 (End callback で渡される、所有しない)。
     */
    void LoadScript(const ScriptOp* ops, u32 op_count, const char* script_id) noexcept;

    /**
     * ラベル → op_index のマッピングを登録する。
     *
     * @details
     * Jump 先 / Start(label) で参照される。同名ラベル登録時は最初の登録のみ有効
     * (= 上書き禁止)。op_index 範囲外は no-op。
     * @param label ラベル名 (所有しない)。
     * @param op_index ラベルが指す op の index。
     */
    void AddLabel(const char* label, u32 op_index) noexcept;

    /**
     * 再生を開始する。
     *
     * @details ラベル未解決 / op 列が空なら即 Finished。
     * @param start_label 開始ラベル (nullptr なら先頭 op から)。
     */
    void Start(const char* start_label = nullptr) noexcept;

    /**
     * 完全停止する (State = Idle、現在の選択肢もクリア)。
     */
    void Stop() noexcept;

    /**
     * 全 op / ラベル / callback / state を破棄して初期状態に戻す。
     */
    void ClearAll() noexcept;

    /**
     * 再生中かを返す。
     *
     * @return Playing / AwaitingInput / AwaitingChoice のいずれかなら true。
     */
    bool                IsPlaying() const noexcept;

    /**
     * 現在の状態を返す。
     *
     * @return 現在の EDialogueScriptState。
     */
    EDialogueScriptState State()     const noexcept { return _state; }

    /**
     * Say op の AwaitingInput を解除し、次 op へ進む。
     *
     * @details State != AwaitingInput では no-op。
     */
    void Advance() noexcept;

    /**
     * AwaitingChoice の選択を確定する。
     *
     * @details
     * choice_index 範囲外 / State != AwaitingChoice は no-op。確定後、jump_label を
     * 解決して該当 op_index へジャンプする (未解決なら Finished)。
     * @param choice_index 選択した選択肢の index。
     */
    void SelectChoice(u32 choice_index) noexcept;

    /**
     * 次に実行する op の index を返す。
     *
     * @return 現在の op_index。
     */
    u32             CurrentOpIndex()  const noexcept { return m_CurrentOpIndex; }

    /**
     * 現在指している op を返す。
     *
     * @return 現在の op へのポインタ (範囲外なら nullptr)。
     */
    const ScriptOp* CurrentOp()       const noexcept;

    /**
     * 提示中の選択肢数を返す。
     *
     * @return AwaitingChoice 中なら選択肢数、それ以外は 0。
     */
    u32             CurrentChoiceCount()           const noexcept;

    /**
     * 提示中の選択肢を index 指定で返す。
     *
     * @param index 取得する選択肢の index。
     * @return 選択肢へのポインタ (AwaitingChoice 外 / 範囲外なら nullptr)。
     */
    const ScriptChoice* CurrentChoice(u32 index) const noexcept;

    /**
     * dt 秒ぶん時間を進める。
     *
     * @details
     * Playing 状態でのみ Wait op の残りタイマを減らし、0 以下になったら次 op を
     * 消化する。dt <= 0 / 非 Playing 時は no-op。
     * @param dt 経過秒。
     */
    void Tick(f32 dt) noexcept;

    /**
     * Say op の通知 callback を登録する。
     *
     * @param cb 登録する callback (nullptr で解除)。
     * @param user cb に渡される文脈ポインタ。
     */
    void SetOnSayCallback          (SayCallback           cb, void* user) noexcept;

    /**
     * Show op の通知 callback を登録する。
     *
     * @param cb 登録する callback (nullptr で解除)。
     * @param user cb に渡される文脈ポインタ。
     */
    void SetOnShowCallback         (ShowHideCallback      cb, void* user) noexcept;

    /**
     * Hide op の通知 callback を登録する。
     *
     * @param cb 登録する callback (nullptr で解除)。
     * @param user cb に渡される文脈ポインタ。
     */
    void SetOnHideCallback         (ShowHideCallback      cb, void* user) noexcept;

    /**
     * Background op の通知 callback を登録する。
     *
     * @param cb 登録する callback (nullptr で解除)。
     * @param user cb に渡される文脈ポインタ。
     */
    void SetOnBackgroundCallback   (BackgroundCallback    cb, void* user) noexcept;

    /**
     * PlayBgm op の通知 callback を登録する。
     *
     * @param cb 登録する callback (nullptr で解除)。
     * @param user cb に渡される文脈ポインタ。
     */
    void SetOnPlayBgmCallback      (BgmSeCallback         cb, void* user) noexcept;

    /**
     * StopBgm op の通知 callback を登録する。
     *
     * @param cb 登録する callback (nullptr で解除)。
     * @param user cb に渡される文脈ポインタ。
     */
    void SetOnStopBgmCallback      (BgmSeCallback         cb, void* user) noexcept;

    /**
     * PlaySe op の通知 callback を登録する。
     *
     * @param cb 登録する callback (nullptr で解除)。
     * @param user cb に渡される文脈ポインタ。
     */
    void SetOnPlaySeCallback       (BgmSeCallback         cb, void* user) noexcept;

    /**
     * Choice op 群の提示 callback を登録する。
     *
     * @param cb 登録する callback (nullptr で解除)。
     * @param user cb に渡される文脈ポインタ。
     */
    void SetOnChoicePresentCallback(ChoicePresentCallback cb, void* user) noexcept;

    /**
     * スクリプト終了の通知 callback を登録する。
     *
     * @param cb 登録する callback (nullptr で解除)。
     * @param user cb に渡される文脈ポインタ。
     */
    void SetOnEndCallback          (EndCallback           cb, void* user) noexcept;

private:
    /**
     * ラベル登録 1 件。
     */
    struct LabelEntry {
        /** ラベル名 (所有しない)。 */
        const char* label    = nullptr;

        /** ラベルが指す op の index。 */
        u32         op_index = 0u;
    };

    /**
     * ラベル名から op_index を解決する。
     *
     * @param label 解決するラベル名 (nullptr 可)。
     * @return 対応する op_index、見つからなければ 0xFFFFFFFFu。
     */
    u32 ResolveLabel(const char* label) const noexcept;

    /**
     * m_CurrentOpIndex を起点に「即進行する op」を消化する。
     *
     * @details
     * Say に当たれば AwaitingInput、Choice 群を踏めば AwaitingChoice、Wait に
     * 当たれば m_WaitRemaining をセットして Playing 継続、EndScene / 末尾到達で
     * Finished に遷移する。Jump ラベル循環はガードカウンタで打ち切り Finished に倒す。
     */
    void RunUntilBlocked() noexcept;

    /**
     * m_CurrentOpIndex 起点から連続する Choice op 群を展開し AwaitingChoice に遷移する。
     *
     * @details 展開後 ChoicePresent callback を発火する。
     */
    void EnterChoiceGroup() noexcept;

    /**
     * 単発の即進行系 op を実行して m_CurrentOpIndex を進める。
     *
     * @details
     * 対象は Show / Hide / Background / PlayBgm / StopBgm / PlaySe / Jump。Jump は
     * 解決した op_index へ飛び (未解決なら Finished)、それ以外は callback を発火して
     * index を 1 進める。
     * @param op 実行する命令。
     */
    void ExecuteImmediateOp(const ScriptOp& op) noexcept;

    /**
     * Finished 状態へ遷移し、End callback を発火する (1 度だけ)。
     */
    void EnterFinished() noexcept;

    /** ロードされた命令列 (deep copy)。 */
    TArray<ScriptOp>       m_Ops;

    /** ラベルテーブル (線形検索)。 */
    TArray<LabelEntry>     m_Labels;

    /** AwaitingChoice 中に展開された選択肢群。 */
    TArray<ScriptChoice> m_CurrentChoices;

    /** LoadScript で渡された ID (所有しない)。 */
    const char* m_ScriptId = nullptr;

    /** 次に実行する op の index。 */
    u32 m_CurrentOpIndex = 0u;

    /** Wait op の残り秒数 (>0 で Playing 継続)。 */
    f32 m_WaitRemaining   = 0.0f;

    /** スクリプト全体の現在状態。 */
    EDialogueScriptState _state = EDialogueScriptState::Idle;

    /** Say op の通知 callback。 */
    SayCallback           m_SayCb          = nullptr;

    /** Say callback に渡す文脈ポインタ。 */
    void* m_SayUser          = nullptr;

    /** Show op の通知 callback。 */
    ShowHideCallback      m_ShowCb         = nullptr;

    /** Show callback に渡す文脈ポインタ。 */
    void* m_ShowUser         = nullptr;

    /** Hide op の通知 callback。 */
    ShowHideCallback      m_HideCb         = nullptr;

    /** Hide callback に渡す文脈ポインタ。 */
    void* m_HideUser         = nullptr;

    /** Background op の通知 callback。 */
    BackgroundCallback    m_BgCb           = nullptr;

    /** Background callback に渡す文脈ポインタ。 */
    void* m_BgUser           = nullptr;

    /** PlayBgm op の通知 callback。 */
    BgmSeCallback         m_PlayBgmCb     = nullptr;

    /** PlayBgm callback に渡す文脈ポインタ。 */
    void* m_PlayBgmUser     = nullptr;

    /** StopBgm op の通知 callback。 */
    BgmSeCallback         m_StopBgmCb     = nullptr;

    /** StopBgm callback に渡す文脈ポインタ。 */
    void* m_StopBgmUser     = nullptr;

    /** PlaySe op の通知 callback。 */
    BgmSeCallback         m_PlaySeCb      = nullptr;

    /** PlaySe callback に渡す文脈ポインタ。 */
    void* m_PlaySeUser      = nullptr;

    /** Choice op 群の提示 callback。 */
    ChoicePresentCallback m_ChoiceCb       = nullptr;

    /** Choice callback に渡す文脈ポインタ。 */
    void* m_ChoiceUser       = nullptr;

    /** スクリプト終了の通知 callback。 */
    EndCallback           m_EndCb          = nullptr;

    /** End callback に渡す文脈ポインタ。 */
    void* m_EndUser          = nullptr;
};

} // namespace acs::game
