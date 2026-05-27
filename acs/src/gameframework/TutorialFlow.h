// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R — FTutorialFlow (Phase 2)
//
// 役割:
//   連続する「チュートリアルステップ」を順序付きで表示・完了判定し、ユーザー操作
//   または明示的な前進指示に応じて次ステップへ進めるシンプルな state machine。
//   描画は行わず、現在ステップへの const ポインタを公開するだけ。表示は呼び出し側
//   (UI レイヤ / Scene) が CurrentStep() を見て自前で描く。
//
// 設計上の方針:
//   ・**FSequence との棲み分け**: FSequence は時間ベースの自動進行 (cutscene 等)。
//     FTutorialFlow は「ユーザーが action を達成したら進む」という能動的進行で、
//     timer ベースではない (require_user_action=true の間は dt をいくら積んでも
//     自動 advance しない)。require_user_action=false なら表示後に AdvanceStep
//     を呼ぶだけで素直に次へ進む (ガイダンス表示 → OK ボタンなど)。
//   ・**所有しない const char***: ACS 規約通り <string> 禁止。id / message /
//     highlight_target は文字列リテラル or 長寿命バッファを想定し、寿命は呼び
//     出し側が保証する。
//   ・**非コピー・非ムーブ**: チュートリアルは通常 Scene につき 1 個の長寿命
//     オブジェクトで、誤コピーで state 分裂すると詰むため最初から禁止。
//   ・**Skip は不可逆**: Skip() を呼ぶと m_Completed=true / m_Active=false に
//     遷移し、Reset() しない限りどの query も終了扱い。誤って 2 度目を呼ばれ
//     ても no-op になるよう冪等。
//
// 使い方:
//   class TutorialScene : public Scene {
//       FTutorialFlow m_Tut;
//       void OnEnter() noexcept override {
//           m_Tut.AddStep({"move",  "WASD で移動してみよう", "player", true});
//           m_Tut.AddStep({"jump",  "SPACE でジャンプ",       "player", true});
//           m_Tut.AddStep({"done",  "チュートリアル完了！",  nullptr,  false});
//           m_Tut.Start();
//       }
//       void OnUpdate(f32 dt) noexcept override {
//           m_Tut.Tick(dt);
//           if (input.JustPressed(EKey::W)) m_Tut.NotifyAction("move");
//           if (input.JustPressed(EKey::Space)) m_Tut.NotifyAction("jump");
//       }
//   };
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

// チュートリアルステップ 1 件分。全フィールド非所有 const char* (寿命は呼び出し側保証)。
//   id                   - NotifyAction で参照される識別子 (一意である必要は無いが、
//                          重複すると最初のマッチが採用される)。
//   message              - 表示用テキスト (UI 側が描画)。
//   highlight_target     - UI ハイライト対象 (例: "player" / "inventory_button")。
//                          nullptr 可。本クラスは保持するだけで解釈はしない。
//   require_user_action  - true の場合、NotifyAction で id と一致する action_id が
//                          来るか、AdvanceStep が明示的に呼ばれるまで自動進行
//                          しない。false の場合は AdvanceStep 呼び出しのみで進む
//                          (timer 等は Tick で扱わない、Phase 2 は明示前進のみ)。
struct FTutorialStep {
    const char* id                  = nullptr;
    const char* message             = nullptr;
    const char* highlight_target    = nullptr;
    bool        require_user_action = false;
};

class FTutorialFlow {
public:
    FTutorialFlow()  noexcept = default;
    ~FTutorialFlow() noexcept = default;

    // 通常は Scene につき 1 個の長寿命オブジェクト。state 分裂を避けるため
    // 非コピー・非ムーブ。
    FTutorialFlow(const FTutorialFlow&)            = delete;
    FTutorialFlow& operator=(const FTutorialFlow&) = delete;
    FTutorialFlow(FTutorialFlow&&)                 = delete;
    FTutorialFlow& operator=(FTutorialFlow&&)      = delete;

    // ----- ステップ定義 -----
    // ステップを末尾に追加。Start 前のみ呼ぶ想定だが、走行中に追加されても
    // 末尾に積まれるだけで現在進行には影響しない (Pillar R 簡易方針)。
    void AddStep(const FTutorialStep& step) noexcept;

    // ----- 制御 -----
    // ステップ 0 から表示開始。ステップが 1 件も無い場合は m_Completed=true に
    // 遷移して即終了 (空チュートリアルの no-op)。複数回呼ばれた場合は Reset
    // 相当の再初期化を行う。
    void Start() noexcept;

    // 次のステップへ手動前進。require_user_action のステップで条件達成後、
    // または require_user_action=false のステップでガイダンス確認後に呼ぶ。
    // 最終ステップで呼ばれた場合は m_Completed=true に遷移。inactive 状態
    // (Start 前 / Skip 後 / 完了後) の呼び出しは no-op。
    void AdvanceStep() noexcept;

    // ユーザーが action を実行したことを通知。現在ステップの id と
    // 一致し require_user_action=true なら自動的に AdvanceStep を呼ぶ。
    // action_id == nullptr / inactive 状態 / 不一致は no-op。
    void NotifyAction(const char* action_id) noexcept;

    // ステップ定義を保持したまま走行 state を初期化 (Start 前と同じ状態)。
    void Reset() noexcept;

    // チュートリアル全体を skip して完了扱いにする。冪等 (2 度目以降 no-op)。
    void Skip() noexcept;

    // ----- 状態 query -----
    // Start 後・Skip / 完了前なら true。
    bool IsActive() const noexcept { return m_Active; }

    // 全ステップを通過 or Skip 済みなら true。
    bool IsCompleted() const noexcept { return m_Completed; }

    // 現在のステップ番号 (Start 前は 0)。
    u32 CurrentStepIndex() const noexcept { return m_CurrentStep; }

    // 描画 / 表示用の現在ステップポインタ。Start 前・Skip 後・完了後は nullptr。
    const FTutorialStep* CurrentStep() const noexcept;

    // 登録済みステップ数。
    u32 StepCount() const noexcept;

    // 毎フレーム呼ぶ (Phase 2 では state 確認用 hook、現状内部 timer は持たない
    // が、将来 auto-advance ステップ / hint 出現遅延等のために noexcept fn を予約)。
    void Tick(f32 dt) noexcept;

private:
    TArray<FTutorialStep> m_Steps;
    u32                 m_CurrentStep = 0;
    bool                m_Active       = false;
    bool                m_Completed    = false;
};

} // namespace acs::game
