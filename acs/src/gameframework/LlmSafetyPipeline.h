// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar U Phase 2 — FLlmSafetyPipeline (LLM 入出力安全パイプ)
//
// 役割:
//   LLM NPC のセリフ生成経路 (= 決定論ゾーンの外側) に対して、入力検証 /
//   jailbreak 検出 / PII 除去 / コンテンツレーティング / トークン予算 /
//   refusal 強制 を 1 つの bit flag 駆動パイプラインとして提供する。
//
//   入力経路 (ユーザー → LLM):
//     ValidateInput(user_text) → FSafetyResult{Pass | Refused | BudgetExceeded}
//
//   出力経路 (LLM → 画面 / NPC セリフ):
//     FilterOutput(llm_response) → FSafetyResult{Pass | Filtered | Refused | BudgetExceeded}
//
// 設計判断:
//   ・**bit flag で個別 on/off**: シーン (チュートリアル / 児童向け / 大人向け) ごとに
//     PII redaction だけ無効化したい等の要望に応えるため、ESvc/FSceneServices と
//     同じ「mask で機能宣言」スタイルに揃える。
//   ・**stub 実装**: Phase 1 (本フェーズ) は単純な文字列長 / keyword チェックのみ。
//     実 jailbreak 検出器・PII regex・コンテンツ分類器は Phase 2 以降で差し込み。
//     現段階でも「常に refusal を返す閾値」を持っているので、上位層は day-0 から
//     "Refused" 経路の表示 (例: NPC が黙る / 別セリフを話す) を必ず書ける。
//   ・**所有しない文字列**: `<string>` 不使用 (ACS 規約)。`FSafetyResult::filtered_text`
//     は呼び出しごとに内部の static thread_local バッファを指す。次回呼び出しで
//     上書きされるため、呼び出し側は **使い終わるまでに必ずコピー or 消費** すること。
//   ・**決定論ゾーン外宣言**: LLM 推論自体が非決定論なので、本パイプラインも
//     `FGame::Tick()` 固定ステップ内で呼ばないこと (= UI スレッド / セリフ表示
//     コールバックから呼ぶ前提)。MlRuntime と同じ契約。
//
// 範囲外 (Phase 2):
//   ・実 jailbreak 検出 (BERT 系分類器、prompt injection corpus 照合)
//   ・PII regex (電話 / メール / 住所 / クレカ番号の各国フォーマット)
//   ・コンテンツレーティングモデル (暴力 / 性的 / 自傷 / ヘイト の 4 軸スコア)
//   ・rate limit / per-user quota (BackendClient と連携で別レイヤ)
//
// ACS 規約:
//   ・STL 不使用 / `<string>` 不使用 / 例外不使用 / 全 noexcept
//   ・非コピー・非ムーブ (state を 1 箇所にとどめる)
#pragma once

#include "foundation/Types.h"
#include "foundation/Log.h"

namespace acs::game {

// =============================================================================
// ESafetyRule — どの安全機能を有効化するかの bit flag
// -----------------------------------------------------------------------------
// `Default` = 全機能 on。ゲーム側で必要に応じて個別に EnableRule(...) で外す。
// =============================================================================
enum class ESafetyRule : u32 {
    None                = 0,
    InputValidation     = 1u << 0,  // 空文字 / 長すぎる入力を弾く
    JailbreakDetection  = 1u << 1,  // "ignore previous instructions" 等の典型句を弾く
    PiiRedaction        = 1u << 2,  // 個人情報を `[REDACTED]` に置換 (Phase 2 では stub)
    EContentRating       = 1u << 3,  // 暴力 / 性的 / 自傷 / ヘイトを弾く (Phase 2 では stub)
    TokenBudget         = 1u << 4,  // 入出力トークン上限 (簡易: 1 token ≒ 4 byte)
    RefusalEnforcement  = 1u << 5,  // キャラ設定逸脱を弾く (Phase 2 では stub)

    Default = InputValidation | JailbreakDetection | PiiRedaction
            | EContentRating   | TokenBudget        | RefusalEnforcement,
};

constexpr ESafetyRule operator|(ESafetyRule a, ESafetyRule b) noexcept {
    return static_cast<ESafetyRule>(static_cast<u32>(a) | static_cast<u32>(b));
}
constexpr ESafetyRule operator&(ESafetyRule a, ESafetyRule b) noexcept {
    return static_cast<ESafetyRule>(static_cast<u32>(a) & static_cast<u32>(b));
}
constexpr bool SafetyHas(ESafetyRule mask, ESafetyRule flag) noexcept {
    return (static_cast<u32>(mask) & static_cast<u32>(flag)) != 0u;
}

// =============================================================================
// ESafetyVerdict — パイプラインの判定結果
// -----------------------------------------------------------------------------
//   Pass            = 安全に通過、`filtered_text` は入力をそのまま返す
//   Refused         = 規約違反 / jailbreak 検出 → 文字列は返さず `refusal_reason` を埋める
//   Filtered        = 危険箇所だけ除去して通過 → `filtered_text` に置換済み文字列
//   BudgetExceeded  = トークン予算超過 → 上位は LLM 呼び出しを skip すべし
// =============================================================================
enum class ESafetyVerdict : u32 {
    Pass            = 0,
    Refused         = 1,
    Filtered        = 2,
    BudgetExceeded  = 3,
};

// =============================================================================
// FSafetyResult — 1 回の Validate / Filter 呼び出しの結果
// -----------------------------------------------------------------------------
// 文字列は所有しない (= 内部 static バッファへのポインタ)。寿命は **次回の
// ValidateInput / FilterOutput 呼び出しまで**。呼び出し側はその前にコピー or
// 消費すること。
// =============================================================================
struct FSafetyResult {
    ESafetyVerdict verdict        = ESafetyVerdict::Pass;
    const char*   filtered_text  = nullptr;  // Pass / Filtered で有効、それ以外 nullptr
    const char*   refusal_reason = nullptr;  // Refused で有効、それ以外 nullptr
    u32           input_tokens   = 0;        // 概算トークン数 (= byte_len / 4)
    u32           output_tokens  = 0;        // FilterOutput でのみ有効
};

// =============================================================================
// FLlmSafetyPipeline — 入出力安全パイプライン本体
// -----------------------------------------------------------------------------
// 1 インスタンス = 1 つの NPC キャラクタ前提 (CharacterAnchor を 1 個保持する)。
// 複数 NPC を同時運用する場合は FLlmSafetyPipeline を NPC ごとに持つ。
//
// 典型使用:
//   FLlmSafetyPipeline pipe;
//   pipe.Init();
//   pipe.SetTokenBudget(1024, 512);
//   pipe.SetCharacterAnchor("You are a friendly shopkeeper in a fantasy RPG.");
//
//   auto in_res = pipe.ValidateInput(player_text);
//   if (in_res.verdict != ESafetyVerdict::Pass) { ShowRefusalUi(in_res.refusal_reason); return; }
//
//   auto llm_text = my_llm.Generate(player_text);
//   auto out_res = pipe.FilterOutput(llm_text);
//   if (out_res.verdict == ESafetyVerdict::Refused) { NpcBecomesSilent(); return; }
//   DisplayNpcLine(out_res.filtered_text);
// =============================================================================
class FLlmSafetyPipeline {
public:
    FLlmSafetyPipeline() noexcept = default;
    ~FLlmSafetyPipeline() noexcept = default;

    // 非コピー・非ムーブ (state を 1 箇所にとどめる)
    FLlmSafetyPipeline(const FLlmSafetyPipeline&)            = delete;
    FLlmSafetyPipeline& operator=(const FLlmSafetyPipeline&) = delete;
    FLlmSafetyPipeline(FLlmSafetyPipeline&&)                 = delete;
    FLlmSafetyPipeline& operator=(FLlmSafetyPipeline&&)      = delete;

    // 初期化。bit flag で機能を選択。多重 Init は最新 rules で上書き。
    void Init(ESafetyRule rules = ESafetyRule::Default) noexcept;

    // 入出力それぞれのトークン上限。0 を指定すると当該方向のチェック無効。
    // 規定値: max_input_tokens = 2048, max_output_tokens = 1024。
    void SetTokenBudget(u32 max_input_tokens, u32 max_output_tokens) noexcept;

    // NPC キャラの system prompt (refusal 判定の基準テキスト)。
    // `nullptr` は anchor 無し = RefusalEnforcement が事実上 no-op になる。
    // 文字列の寿命は呼び出し側責務 (パイプライン側はコピーしない)。
    void SetCharacterAnchor(const char* system_prompt) noexcept;

    // ユーザー入力を検証 (LLM 投入前)。
    // - InputValidation: 空 / 長すぎ → Refused
    // - JailbreakDetection: "ignore previous instructions" 等の典型句 → Refused
    // - TokenBudget: input_tokens > max_input_tokens → BudgetExceeded
    FSafetyResult ValidateInput(const char* user_text) noexcept;

    // LLM 応答を検証 / フィルタ (UI 表示前)。
    // - PiiRedaction: 個人情報を `[REDACTED]` に置換 → Filtered (Phase 2: stub)
    // - EContentRating: 危険スコア超過 → Refused (Phase 2: stub)
    // - RefusalEnforcement: キャラ逸脱 → Refused (Phase 2: stub)
    // - TokenBudget: output_tokens > max_output_tokens → BudgetExceeded
    FSafetyResult FilterOutput(const char* llm_response) noexcept;

    // rules マスク操作
    bool       IsRuleEnabled(ESafetyRule rule) const noexcept;
    void       EnableRule(ESafetyRule rule, bool enable) noexcept;
    ESafetyRule Rules() const noexcept { return _rules; }

    // 統計 (テレメトリ / デバッグ用)
    u32  RefusedCount()  const noexcept { return _refused_count; }
    u32  FilteredCount() const noexcept { return _filtered_count; }

    // 統計とキャラ anchor を初期状態に戻す (rules / budget は保持)。
    void Reset() noexcept;

private:
    ESafetyRule  _rules               = ESafetyRule::Default;
    u32         _max_input_tokens    = 2048;
    u32         _max_output_tokens   = 1024;
    const char* _character_anchor    = nullptr;  // 非所有
    u32         _refused_count       = 0;
    u32         _filtered_count      = 0;
    bool        _initialized         = false;
};

} // namespace acs::game
