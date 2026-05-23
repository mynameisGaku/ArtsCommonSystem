// SPDX-License-Identifier: Apache-2.0
// GameFramework ジャンルキット (visual novel) — DialogueScript
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
//     AudioDirector) の責任とする (DialogueSystem / CinematicsDirector と
//     同じ「副作用ゼロ + callback 駆動」方針)。
//
// 設計選択:
//   ・**文字列を所有しない**: ScriptOp::arg1 / arg2 は const char* のまま。
//     スクリプトデータは literal / バンドル等で別管理する想定 (STL <string>
//     禁止 / Array<char> での deep copy も避けて allocator フリーに保つ)。
//   ・**op_count + ops ポインタを丸ごと受け取る**: LoadScript はポインタを
//     コピーせず内部 Array に複製する (= caller が ops を解放しても安全)。
//     ScriptOp は POD なので Array<ScriptOp> での bulk copy は trivial。
//   ・**ラベルは別 Array で線形検索**: 典型 N < 100 なので OK。同名ラベル
//     登録時は最初の登録のみ有効 (= 上書き禁止)。
//   ・**現在の選択肢は別 Array に展開**: Choice op を踏んだ際に、後続の
//     Choice op を「同じ Say の選択肢群」として束ねるのは仕様が複雑になる
//     ので避ける。代わりに Choice op 1 個が arg1=label / arg2=jump_label の
//     ペアを 1 件持つ。実用上は同じ ScriptChoice 配列を複数 op で並べる
//     と無駄なので、Choice op は 1 個で「選択肢群を提示」を表現する仕様に
//     倒す: arg_u を 「次に続く Choice op の本数」(= group size) として扱い、
//     SelectChoice() が消費する。
//     -> 簡素化: 「Choice op = 選択肢 1 件」、AwaitingChoice 中は
//        連続する Choice op 群を _current_choices に展開しておき、
//        SelectChoice(idx) で jump_label に飛ぶ。
//   ・**callback は kind 別に分ける**: Say / Show・Hide / Background /
//     PlayBgm・StopBgm / PlaySe / ChoicePresent / End の 6 種に分割。
//     CinematicsDirector と同じく汎用 1 個に集約しない方針。
//   ・**Wait op は AwaitingInput には遷移しない**: arg_f 秒経過で自動進行。
//     Say op の末尾で AwaitingInput になり、Advance() で次へ進む契約。
//   ・**非コピー・非ムーブ**: 現在 op_index / state の唯一性を担保するため。
//
// 参考: DialogueSystem (タイプライタ + 選択肢)、CinematicsDirector (timeline)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

// スクリプト命令の種別。
//   Say        : セリフ表示 (arg1=speaker, arg2=text) → AwaitingInput 遷移
//   Show       : キャラポートレート表示 (arg1=character_id, arg2=sprite_id)
//   Hide       : キャラポートレート非表示 (arg1=character_id)
//   Background : 背景切替 (arg1=bg_id)
//   PlayBgm    : BGM 再生 (arg1=bgm_id, arg_f=volume)
//   StopBgm    : BGM 停止
//   PlaySe     : SE 再生 (arg1=se_id, arg_f=volume)
//   Choice     : 選択肢提示 (arg1=label, arg2=jump_label)
//   Wait       : 時間経過待ち (arg_f=seconds) — 自動で進行
//   Jump       : 指定ラベルへジャンプ (arg1=label)
//   EndScene   : スクリプト終了 (End callback 発火)
enum class EScriptOpKind : u8 {
    Say        = 0,
    Show       = 1,
    Hide       = 2,
    Background = 3,
    PlayBgm    = 4,
    StopBgm    = 5,
    PlaySe     = 6,
    Choice     = 7,
    Wait       = 8,
    Jump       = 9,
    EndScene   = 10,
};

// 1 つの命令。文字列は所有しない (literal / バンドル参照)。
struct ScriptOp {
    EScriptOpKind kind  = EScriptOpKind::Say;
    const char*  arg1  = nullptr;  // kind に応じた第 1 引数 (speaker / character_id / bgm_id 等)
    const char*  arg2  = nullptr;  // kind に応じた第 2 引数 (text / sprite_id / jump_label 等)
    f32          arg_f = 0.0f;     // 数値引数 (volume / wait 秒数)
    u32          arg_u = 0u;       // 補助 u32 引数 (将来拡張用 / 未使用なら 0)
};

// 1 つの選択肢。Choice op 群から AwaitingChoice 時に展開される。
// 文字列は所有しない (= 元 ScriptOp の arg1 / arg2 を参照)。
struct ScriptChoice {
    const char* label      = nullptr;  // UI に表示する選択肢ラベル
    const char* jump_label = nullptr;  // 選択時のジャンプ先ラベル名
};

// スクリプト全体の状態。
//   Idle           : LoadScript 直後 / Stop 後 (Start 前)
//   Playing        : op 進行中 (Wait / 即座に進む系の op を処理中)
//   AwaitingInput  : Say op 完了後、Advance 待ち
//   AwaitingChoice : Choice op 群を展開済み、SelectChoice 待ち
//   Finished       : EndScene op に到達、または op 列を末尾まで実行完了
enum class EDialogueScriptState : u8 {
    Idle           = 0,
    Playing        = 1,
    AwaitingInput  = 2,
    AwaitingChoice = 3,
    Finished       = 4,
};

// callback signature 群 (全 noexcept、void* user は SetOn*Callback で受け取った文脈)。
using SayCallback           = void(*)(void* user, const char* speaker, const char* text) noexcept;
using ShowHideCallback      = void(*)(void* user, const char* character_id, const char* sprite_id) noexcept;
using BgmSeCallback         = void(*)(void* user, const char* audio_id, f32 volume) noexcept;
using BackgroundCallback    = void(*)(void* user, const char* bg_id) noexcept;
using ChoicePresentCallback = void(*)(void* user, const ScriptChoice* choices, u32 count) noexcept;
using EndCallback           = void(*)(void* user, const char* script_id) noexcept;

class DialogueScript {
public:
    DialogueScript() noexcept = default;
    ~DialogueScript() noexcept = default;

    // 進行状態の唯一性を担保するため非コピー・非ムーブ
    DialogueScript(const DialogueScript&)            = delete;
    DialogueScript& operator=(const DialogueScript&) = delete;
    DialogueScript(DialogueScript&&)                 = delete;
    DialogueScript& operator=(DialogueScript&&)      = delete;

    // ----- セットアップ -----
    // 既定値に戻す (内部状態のみ、callback も含めてリセットしたい場合は ClearAll)。
    void Init() noexcept;

    // op 列をロードする。ops / script_id は所有しない (literal / バンドル参照)。
    // 既存のスクリプトとラベルテーブルは破棄される。State は Idle に遷移。
    // ops == nullptr or op_count == 0 でも安全 (= 空スクリプト、Start で即 Finished)。
    void LoadScript(const ScriptOp* ops, u32 op_count, const char* script_id) noexcept;

    // ラベル → op_index のマッピングを登録。Jump 先 / Start(label) で参照される。
    // 同名ラベル登録時は最初の登録のみ有効 (= 上書き禁止)。op_index 範囲外は no-op。
    void AddLabel(const char* label, u32 op_index) noexcept;

    // 再生開始。start_label == nullptr なら先頭 op から。
    // ラベル未解決 / op 列が空なら即 Finished。
    void Start(const char* start_label = nullptr) noexcept;

    // 完全停止 (State = Idle、_current_choices もクリア)。
    void Stop() noexcept;

    // 全 op / ラベル / callback / state を破棄して初期状態に戻す。
    void ClearAll() noexcept;

    // ----- 進行制御 -----
    bool                IsPlaying() const noexcept;
    EDialogueScriptState State()     const noexcept { return _state; }

    // Say op の AwaitingInput を解除し、次 op へ進む。
    // State != AwaitingInput では no-op。
    void Advance() noexcept;

    // AwaitingChoice の選択を確定。choice_index 範囲外 / State != AwaitingChoice は no-op。
    // 確定後、jump_label を解決して該当 op_index へジャンプする。
    void SelectChoice(u32 choice_index) noexcept;

    // ----- accessors -----
    u32             CurrentOpIndex()  const noexcept { return _current_op_index; }
    const ScriptOp* CurrentOp()       const noexcept;
    u32             CurrentChoiceCount()           const noexcept;
    const ScriptChoice* CurrentChoice(u32 index) const noexcept;

    // ----- フレーム更新 -----
    // dt 秒進める。Wait op のタイマを進めるほか、Playing 状態で
    // 「即進行系の op」(Show/Hide/Background/PlayBgm/...) を消化する。
    // dt <= 0 / 非再生時は op 消化のみ無条件に進めない (= タイマだけ進める)。
    void Tick(f32 dt) noexcept;

    // ----- callback 登録 -----
    void SetOnSayCallback          (SayCallback           cb, void* user) noexcept;
    void SetOnShowCallback         (ShowHideCallback      cb, void* user) noexcept;
    void SetOnHideCallback         (ShowHideCallback      cb, void* user) noexcept;
    void SetOnBackgroundCallback   (BackgroundCallback    cb, void* user) noexcept;
    void SetOnPlayBgmCallback      (BgmSeCallback         cb, void* user) noexcept;
    void SetOnStopBgmCallback      (BgmSeCallback         cb, void* user) noexcept;
    void SetOnPlaySeCallback       (BgmSeCallback         cb, void* user) noexcept;
    void SetOnChoicePresentCallback(ChoicePresentCallback cb, void* user) noexcept;
    void SetOnEndCallback          (EndCallback           cb, void* user) noexcept;

private:
    // ラベル登録 1 件。
    struct LabelEntry {
        const char* label    = nullptr;
        u32         op_index = 0u;
    };

    // ラベル名 → op_index 解決 (見つからなければ UINT32_MAX 相当の 0xFFFFFFFFu)。
    u32 ResolveLabel(const char* label) const noexcept;

    // _current_op_index を起点に「即進行する op」を消化する。
    // Say に当たれば AwaitingInput、Choice 群を踏めば AwaitingChoice、
    // Wait に当たれば _wait_remaining をセットして Playing 継続、
    // EndScene / 末尾到達で Finished に遷移する。
    void RunUntilBlocked() noexcept;

    // _current_op_index 起点から連続する Choice op 群を _current_choices に展開し
    // AwaitingChoice に遷移。ChoicePresent callback を発火する。
    void EnterChoiceGroup() noexcept;

    // 単発 op (= 即進行系: Show / Hide / Background / PlayBgm / StopBgm / PlaySe / Jump)
    // を実行して _current_op_index を 1 進める。op が範囲外なら何もしない。
    void ExecuteImmediateOp(const ScriptOp& op) noexcept;

    // Finished 状態へ遷移し、End callback を発火する (1 度だけ)。
    void EnterFinished() noexcept;

    // ----- データ -----
    Array<ScriptOp>       _ops;             // ロードされた命令列 (deep copy)
    Array<LabelEntry>     _labels;          // ラベルテーブル (線形検索)
    Array<ScriptChoice> _current_choices; // AwaitingChoice 中に展開された選択肢群

    const char* _script_id = nullptr;       // LoadScript で渡された ID (所有しない)

    u32 _current_op_index = 0u;             // 次に実行する op の index
    f32 _wait_remaining   = 0.0f;           // Wait op の残り秒数 (>0 で Playing 継続)

    EDialogueScriptState _state = EDialogueScriptState::Idle;

    // ---- callbacks ----
    SayCallback           _say_cb          = nullptr; void* _say_user          = nullptr;
    ShowHideCallback      _show_cb         = nullptr; void* _show_user         = nullptr;
    ShowHideCallback      _hide_cb         = nullptr; void* _hide_user         = nullptr;
    BackgroundCallback    _bg_cb           = nullptr; void* _bg_user           = nullptr;
    BgmSeCallback         _play_bgm_cb     = nullptr; void* _play_bgm_user     = nullptr;
    BgmSeCallback         _stop_bgm_cb     = nullptr; void* _stop_bgm_user     = nullptr;
    BgmSeCallback         _play_se_cb      = nullptr; void* _play_se_user      = nullptr;
    ChoicePresentCallback _choice_cb       = nullptr; void* _choice_user       = nullptr;
    EndCallback           _end_cb          = nullptr; void* _end_user          = nullptr;
};

} // namespace acs::game
