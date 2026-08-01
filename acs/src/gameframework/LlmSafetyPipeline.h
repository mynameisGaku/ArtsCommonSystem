// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar U — CLlmSafetyPipeline (LLM 入出力安全パイプ)
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
//     PII redaction だけ無効化したい等の要望に応えるため、ESvc/CSceneServices と
//     同じ「mask で機能宣言」スタイルに揃える。
//   ・**決定論的ルールベース実装**: jailbreak / refusal は keyword 一致、PII は
//     手書き char スキャナ (`<regex>` 不使用) で実装。常に同じ入力に同じ判定を
//     返すので上位層は "Refused" / "Filtered" 経路の表示 (例: NPC が黙る / 別
//     セリフを話す) を確実に書ける。EContentRating の 4 軸スコアだけは学習済み
//     分類器を要するため、明示的に ML 注入の seam として残してある。
//   ・**所有しない文字列**: `<string>` 不使用 (ACS 規約)。`FSafetyResult::filtered_text`
//     は呼び出しごとに内部の static thread_local バッファを指す。次回呼び出しで
//     上書きされるため、呼び出し側は **使い終わるまでに必ずコピー or 消費** すること。
//   ・**決定論ゾーン外宣言**: LLM 推論自体が非決定論なので、本パイプラインも
//     `CGame::Tick()` 固定ステップ内で呼ばないこと (= UI スレッド / セリフ表示
//     コールバックから呼ぶ前提)。IMlRuntime と同じ契約。
//
// 範囲外 (ML 分類器の差し込み口として残す seam):
//   ・分類器ベース jailbreak 検出 (BERT 系、prompt injection corpus 照合) —
//     現状は keyword 一致で代替。corpus が用意でき次第ここを置き換える。
//   ・住所 PII (各国フォーマットが多様で誤検出が多い) — メール / 電話 / クレカは
//     本実装で検出済み。住所は分類器側の責務とする。
//   ・コンテンツレーティングモデル (暴力 / 性的 / 自傷 / ヘイトの 4 軸スコア) —
//     EContentRating 軸として有効化口だけ用意済み (classifier 注入待ち)。
//   ・rate limit / per-user quota (IBackendClient と連携で別レイヤ)
//
// ACS 規約:
//   ・STL 不使用 / `<string>` 不使用 / 例外不使用 / 全 noexcept
//   ・非コピー・非ムーブ (state を 1 箇所にとどめる)
#pragma once

#include "foundation/Types.h"
#include "foundation/Log.h"

namespace acs::game {

/**
 * どの安全機能を有効化するかの bit flag。
 *
 * @details Default = 全機能 on。ゲーム側で必要に応じて個別に EnableRule(...) で外す。
 */
enum class ESafetyRule : u32 {
    /** 全機能 off。 */
    None                = 0,

    /** 空文字 / 長すぎる入力を弾く。 */
    InputValidation     = 1u << 0,

    /** "ignore previous instructions" 等の jailbreak 典型句を弾く。 */
    JailbreakDetection  = 1u << 1,

    /** メール / 電話 / クレカ番号を [REDACTED] に置換する。 */
    PiiRedaction        = 1u << 2,

    /** 暴力 / 性的 / 自傷 / ヘイトを弾く (ML 分類器 seam、未注入時は no-op)。 */
    ContentRating        = 1u << 3,

    /** 入出力トークン上限を超えた場合に弾く (簡易: 1 token ≒ 4 byte)。 */
    TokenBudget         = 1u << 4,

    /** キャラ逸脱 / メタ発言キーワードを弾く。 */
    RefusalEnforcement  = 1u << 5,

    /** 全機能 on (既定値)。 */
    Default = InputValidation | JailbreakDetection | PiiRedaction
            | ContentRating    | TokenBudget        | RefusalEnforcement,
};

/**
 * ESafetyRule の OR 合成。
 *
 * @param a 合成元のマスク。
 * @param b 合成するマスク。
 * @return a と b を OR したマスク。
 */
constexpr ESafetyRule operator|(ESafetyRule a, ESafetyRule b) noexcept {
    return static_cast<ESafetyRule>(static_cast<u32>(a) | static_cast<u32>(b));
}

/**
 * ESafetyRule の AND 合成。
 *
 * @param a 合成元のマスク。
 * @param b 合成するマスク。
 * @return a と b を AND したマスク。
 */
constexpr ESafetyRule operator&(ESafetyRule a, ESafetyRule b) noexcept {
    return static_cast<ESafetyRule>(static_cast<u32>(a) & static_cast<u32>(b));
}

/**
 * マスクに特定フラグが立っているかを返す。
 *
 * @param mask 検査対象のマスク。
 * @param flag 検査するフラグ。
 * @return flag が mask に含まれていれば true。
 */
constexpr bool SafetyHas(ESafetyRule mask, ESafetyRule flag) noexcept {
    return (static_cast<u32>(mask) & static_cast<u32>(flag)) != 0u;
}

/**
 * パイプラインの判定結果。
 *
 * @details Validate / Filter が FSafetyResult::verdict に返す 4 値。
 */
enum class ESafetyVerdict : u32 {
    /** 安全に通過。filtered_text は入力をそのまま返す。 */
    Pass            = 0,

    /** 規約違反 / jailbreak 検出。文字列は返さず refusal_reason を埋める。 */
    Refused         = 1,

    /** 危険箇所だけ除去して通過。filtered_text に置換済み文字列を返す。 */
    Filtered        = 2,

    /** トークン予算超過。上位は LLM 呼び出しを skip すべき。 */
    BudgetExceeded  = 3,
};

/**
 * 1 回の Validate / Filter 呼び出しの結果。
 *
 * @details
 * 文字列は所有しない (= 内部 static バッファへのポインタ)。寿命は次回の
 * ValidateInput / FilterOutput 呼び出しまで。呼び出し側はその前にコピー or 消費すること。
 */
struct FSafetyResult {
    /** 判定結果。 */
    ESafetyVerdict verdict        = ESafetyVerdict::Pass;

    /** 通過後テキスト (Pass / Filtered で有効、それ以外 nullptr)。 */
    const char*   filtered_text  = nullptr;

    /** 拒否理由 (Refused で有効、それ以外 nullptr)。 */
    const char*   refusal_reason = nullptr;

    /** 入力の概算トークン数 (= byte_len / 4)。 */
    u32           input_tokens   = 0;

    /** 出力の概算トークン数 (FilterOutput でのみ有効)。 */
    u32           output_tokens  = 0;
};

/**
 * LLM 入出力の安全パイプライン本体。
 *
 * @details
 * 入力検証 / jailbreak 検出 / PII 除去 / コンテンツレーティング / トークン予算 /
 * refusal 強制を bit flag 駆動で提供する。1 インスタンス = 1 つの NPC キャラクタ前提
 * (CharacterAnchor を 1 個保持する)。複数 NPC を同時運用する場合は NPC ごとに持つ。
 * 典型使用は Init → SetTokenBudget → SetCharacterAnchor のあと、ValidateInput で
 * ユーザー入力を検証し、LLM 生成結果を FilterOutput で検証 / フィルタする。
 */
class CLlmSafetyPipeline {
public:
    /** 既定値で構築する (rules=Default、未初期化状態)。 */
    CLlmSafetyPipeline() noexcept = default;

    /** 破棄する。 */
    ~CLlmSafetyPipeline() noexcept = default;

    /** コピー禁止 (state を 1 箇所にとどめるため)。 */
    CLlmSafetyPipeline(const CLlmSafetyPipeline&)            = delete;

    /** コピー代入も禁止。 */
    CLlmSafetyPipeline& operator=(const CLlmSafetyPipeline&) = delete;

    /** ムーブ禁止。 */
    CLlmSafetyPipeline(CLlmSafetyPipeline&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CLlmSafetyPipeline& operator=(CLlmSafetyPipeline&&)      = delete;

    /**
     * パイプラインを初期化する。
     *
     * @details bit flag で機能を選択する。多重 Init は最新 rules で上書きし統計をリセットする。
     * @param rules 有効化する安全機能のマスク (既定は全機能 on)。
     */
    void Init(ESafetyRule rules = ESafetyRule::Default) noexcept;

    /**
     * 入出力それぞれのトークン上限を設定する。
     *
     * @details 0 を指定すると当該方向のチェックを無効にする。規定値は 2048 / 1024。
     * @param max_input_tokens 入力トークン上限。
     * @param max_output_tokens 出力トークン上限。
     */
    void SetTokenBudget(u32 max_input_tokens, u32 max_output_tokens) noexcept;

    /**
     * NPC キャラの system prompt (refusal 判定の基準テキスト) を設定する。
     *
     * @details nullptr は anchor 無し = RefusalEnforcement が事実上 no-op になる。文字列はコピーしない。
     * @param system_prompt キャラ anchor のテキスト (寿命は呼び出し側責務)。
     */
    void SetCharacterAnchor(const char* system_prompt) noexcept;

    /**
     * ユーザー入力を検証する (LLM 投入前)。
     *
     * @details
     * InputValidation で空 / 長すぎを Refused、JailbreakDetection で典型句を Refused、
     * TokenBudget で input_tokens 超過を BudgetExceeded にする。問題なければ Pass。
     * @param user_text 検証するユーザー入力。
     * @return 判定結果 (Pass 時は filtered_text に入力コピー)。
     */
    FSafetyResult ValidateInput(const char* user_text) noexcept;

    /**
     * LLM 応答を検証 / フィルタする (UI 表示前)。
     *
     * @details
     * 判定順は出力長サニティ → TokenBudget → RefusalEnforcement → EContentRating → PiiRedaction。
     * キャラ逸脱 / メタ発言キーワード一致や危険スコア超過は Refused、メール / 電話 / クレカ番号は
     * [REDACTED] に置換して Filtered、トークン超過は BudgetExceeded にする。
     * @param llm_response 検証する LLM 応答。
     * @return 判定結果 (Filtered 時は filtered_text に置換済みテキスト)。
     */
    FSafetyResult FilterOutput(const char* llm_response) noexcept;

    /**
     * 指定ルールが有効かを返す。
     *
     * @param rule 検査するルール。
     * @return 有効なら true。
     */
    bool       IsRuleEnabled(ESafetyRule rule) const noexcept;

    /**
     * 指定ルールの有効 / 無効を切り替える。
     *
     * @param rule 切り替えるルール。
     * @param enable true で有効化、false で無効化。
     */
    void       EnableRule(ESafetyRule rule, bool enable) noexcept;

    /**
     * 現在の rules マスクを返す。
     *
     * @return 有効化されている安全機能のマスク。
     */
    ESafetyRule Rules() const noexcept { return m_Rules; }

    /**
     * これまでに Refused / BudgetExceeded とした回数を返す。
     *
     * @return 拒否カウント。
     */
    u32  RefusedCount()  const noexcept { return m_RefusedCount; }

    /**
     * これまでに PII 置換で Filtered とした回数を返す。
     *
     * @return フィルタカウント。
     */
    u32  FilteredCount() const noexcept { return m_FilteredCount; }

    /** 統計とキャラ anchor を初期状態に戻す (rules / budget は保持)。 */
    void Reset() noexcept;

private:
    /** 有効化されている安全機能のマスク。 */
    ESafetyRule  m_Rules               = ESafetyRule::Default;

    /** 入力トークン上限 (0 でチェック無効)。 */
    u32         m_MaxInputTokens    = 2048;

    /** 出力トークン上限 (0 でチェック無効)。 */
    u32         m_MaxOutputTokens   = 1024;

    /** キャラ anchor の system prompt (非所有)。 */
    const char* m_CharacterAnchor    = nullptr;

    /** Refused / BudgetExceeded の累計回数。 */
    u32         m_RefusedCount       = 0;

    /** Filtered (PII 置換) の累計回数。 */
    u32         m_FilteredCount      = 0;

    /** Init 済みフラグ。 */
    bool        m_Initialized         = false;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FLlmSafetyPipeline = CLlmSafetyPipeline;

} // namespace acs::game
