// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar U Phase 2 — LlmSafetyPipeline 実装 (stub)
//
// 本ファイルは Phase 2 の **スケルトン** 実装。実 ML 分類器 / regex は将来差し込み。
// 現段階で機能するもの:
//   ・InputValidation: 空 / 長すぎる入力 (> 8KB) を Refused
//   ・JailbreakDetection: 既知の prompt injection 典型句 (大小文字非感受) を Refused
//   ・TokenBudget: byte_len/4 概算でトークン超過を BudgetExceeded
//   ・PiiRedaction: TODO (Phase 2 で実装、現状は no-op で Pass)
//   ・EContentRating: TODO (Phase 2 で実装、現状は no-op で Pass)
//   ・RefusalEnforcement: TODO (Phase 2 で実装、現状は no-op で Pass)
//
// 文字列バッファ:
//   `SafetyResult::filtered_text` が指すのは関数内 thread_local バッファ。
//   次回呼び出しまでに使い切る前提 (header コメント参照)。
#include "gameframework/LlmSafetyPipeline.h"

namespace acs::game {

namespace {

// ---- 内部: 文字列長 (`<string.h>` 不使用、明示ループで C 規約準拠) ------------
// strlen 等価。nullptr は 0 を返す (呼び出し側で null チェックを省略可能に)。
u32 StrLen(const char* s) noexcept {
    if (s == nullptr) return 0;
    u32 n = 0;
    while (s[n] != '\0') ++n;
    return n;
}

// 大文字 → 小文字。ASCII 範囲のみ (multibyte は素通し)。
char ToLowerAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

// haystack に needle が出現するか (大小文字非感受、ASCII)。
// needle は短い文字列リテラル前提なので素朴な O(n*m) で十分。
bool ContainsCaseInsensitive(const char* haystack, const char* needle) noexcept {
    if (haystack == nullptr || needle == nullptr) return false;
    if (needle[0] == '\0') return true;
    for (u32 i = 0; haystack[i] != '\0'; ++i) {
        u32 k = 0;
        while (needle[k] != '\0'
               && haystack[i + k] != '\0'
               && ToLowerAscii(haystack[i + k]) == ToLowerAscii(needle[k])) {
            ++k;
        }
        if (needle[k] == '\0') return true;  // 完全一致
    }
    return false;
}

// 簡易トークン推定: 英語/混在テキストに対して 1 token ≒ 4 byte (OpenAI 経験則)。
// 厳密 BPE は不要、予算チェック用途には十分。
u32 EstimateTokens(u32 byte_len) noexcept {
    return (byte_len + 3u) / 4u;
}

// ---- jailbreak 典型句リスト (Phase 1) ----------------------------------------
// 実運用では prompt injection corpus + 分類器に置き換える。本リストは
// 「最低限の防御線」として有名な英語句のみを並べる。日本語版 / 多言語拡張は
// Phase 2 で別ファイル (LlmSafetyPatterns.h) に切り出す予定。
constexpr const char* kJailbreakPatterns[] = {
    "ignore previous instructions",
    "ignore the above",
    "disregard your instructions",
    "you are now",
    "act as if",
    "bypass your guidelines",
    "developer mode",
    "system prompt",
    "jailbreak",
    "DAN mode",                 // "Do Anything Now"
};

constexpr u32 kJailbreakPatternCount =
    sizeof(kJailbreakPatterns) / sizeof(kJailbreakPatterns[0]);

// ---- 文字列バッファ (filtered_text 用) ---------------------------------------
// Phase 1 では PiiRedaction / Filter 処理が空なので入力をそのまま返す。
// 入力ポインタの寿命を呼び出し側が保証できない可能性に備えて thread_local
// バッファにコピーして返す。サイズは入力上限と同じ 8KB。
//
// thread_local にすることで、複数スレッド (例: gameplay スレッドと UI スレッド)
// で別の LlmSafetyPipeline を回しても互いに上書きしない。
constexpr u32 kFilteredBufSize = 8192;

const char* StoreFiltered(const char* src, u32 len) noexcept {
    static thread_local char buf[kFilteredBufSize];
    if (src == nullptr) {
        buf[0] = '\0';
        return buf;
    }
    u32 copy_n = (len < kFilteredBufSize - 1u) ? len : (kFilteredBufSize - 1u);
    for (u32 i = 0; i < copy_n; ++i) buf[i] = src[i];
    buf[copy_n] = '\0';
    return buf;
}

// 入力上限 byte 数 (= filtered バッファサイズ - 1)。これ以上は Refused 扱い。
constexpr u32 kMaxInputBytes  = kFilteredBufSize - 1u;
constexpr u32 kMaxOutputBytes = kFilteredBufSize - 1u;

} // namespace

// =============================================================================
// LlmSafetyPipeline 実装
// =============================================================================
void LlmSafetyPipeline::Init(ESafetyRule rules) noexcept {
    _rules            = rules;
    _refused_count    = 0;
    _filtered_count   = 0;
    _initialized      = true;
    ACS_LOG_DEBUG("LlmSafetyPipeline: Init (rules=0x%08X)", static_cast<u32>(rules));
}

void LlmSafetyPipeline::SetTokenBudget(u32 max_input_tokens, u32 max_output_tokens) noexcept {
    _max_input_tokens  = max_input_tokens;
    _max_output_tokens = max_output_tokens;
}

void LlmSafetyPipeline::SetCharacterAnchor(const char* system_prompt) noexcept {
    _character_anchor = system_prompt;  // 非所有 (寿命は呼び出し側)
}

bool LlmSafetyPipeline::IsRuleEnabled(ESafetyRule rule) const noexcept {
    return SafetyHas(_rules, rule);
}

void LlmSafetyPipeline::EnableRule(ESafetyRule rule, bool enable) noexcept {
    if (enable) {
        _rules = _rules | rule;
    } else {
        _rules = static_cast<ESafetyRule>(
            static_cast<u32>(_rules) & ~static_cast<u32>(rule));
    }
}

void LlmSafetyPipeline::Reset() noexcept {
    _refused_count    = 0;
    _filtered_count   = 0;
    _character_anchor = nullptr;
}

SafetyResult LlmSafetyPipeline::ValidateInput(const char* user_text) noexcept {
    SafetyResult r{};
    const u32 len = StrLen(user_text);
    r.input_tokens = EstimateTokens(len);

    // ---- InputValidation: 空 / 過大 ----------------------------------------
    if (SafetyHas(_rules, ESafetyRule::InputValidation)) {
        if (len == 0) {
            r.verdict        = ESafetyVerdict::Refused;
            r.refusal_reason = "empty input";
            ++_refused_count;
            return r;
        }
        if (len > kMaxInputBytes) {
            r.verdict        = ESafetyVerdict::Refused;
            r.refusal_reason = "input too long";
            ++_refused_count;
            return r;
        }
    }

    // ---- TokenBudget: 概算トークン超過 -------------------------------------
    if (SafetyHas(_rules, ESafetyRule::TokenBudget)
        && _max_input_tokens > 0
        && r.input_tokens > _max_input_tokens) {
        r.verdict        = ESafetyVerdict::BudgetExceeded;
        r.refusal_reason = "input token budget exceeded";
        ++_refused_count;
        return r;
    }

    // ---- JailbreakDetection: 既知の典型句 (Phase 1 stub) -------------------
    if (SafetyHas(_rules, ESafetyRule::JailbreakDetection)) {
        for (u32 i = 0; i < kJailbreakPatternCount; ++i) {
            if (ContainsCaseInsensitive(user_text, kJailbreakPatterns[i])) {
                r.verdict        = ESafetyVerdict::Refused;
                r.refusal_reason = "jailbreak attempt detected";
                ++_refused_count;
                ACS_LOG_WARN("LlmSafetyPipeline: jailbreak pattern matched ('%s')",
                             kJailbreakPatterns[i]);
                return r;
            }
        }
    }

    // ---- Pass: 入力をそのまま (バッファコピー経由で) 返す ------------------
    r.verdict       = ESafetyVerdict::Pass;
    r.filtered_text = StoreFiltered(user_text, len);
    return r;
}

SafetyResult LlmSafetyPipeline::FilterOutput(const char* llm_response) noexcept {
    SafetyResult r{};
    const u32 len = StrLen(llm_response);
    r.output_tokens = EstimateTokens(len);

    // ---- 出力長サニティ: 異常に長い出力は Refused -------------------------
    // (= バッファ溢れを未然に止める防御線。InputValidation の鏡像)
    if (len > kMaxOutputBytes) {
        r.verdict        = ESafetyVerdict::Refused;
        r.refusal_reason = "llm response too long";
        ++_refused_count;
        return r;
    }

    // ---- TokenBudget: 概算トークン超過 -------------------------------------
    if (SafetyHas(_rules, ESafetyRule::TokenBudget)
        && _max_output_tokens > 0
        && r.output_tokens > _max_output_tokens) {
        r.verdict        = ESafetyVerdict::BudgetExceeded;
        r.refusal_reason = "output token budget exceeded";
        ++_refused_count;
        return r;
    }

    // ---- PiiRedaction (TODO Phase 2): 個人情報を [REDACTED] に置換 ----------
    // Phase 2 で電話 / メール / 住所 / クレカ番号の各国フォーマット regex を導入。
    // 現状は no-op で素通し。実装後は r.verdict = Filtered / ++_filtered_count。
    if (SafetyHas(_rules, ESafetyRule::PiiRedaction)) {
        // TODO(phase2): apply PII regex, set verdict = Filtered if redacted
    }

    // ---- EContentRating (TODO Phase 2): 危険スコア超過チェック --------------
    // Phase 2 で暴力 / 性的 / 自傷 / ヘイトの 4 軸スコア分類器を導入。
    // 現状は no-op。
    if (SafetyHas(_rules, ESafetyRule::EContentRating)) {
        // TODO(phase2): run classifier, set verdict = Refused if any score > threshold
    }

    // ---- RefusalEnforcement (TODO Phase 2): キャラ逸脱チェック -------------
    // Phase 2 で `_character_anchor` と応答の semantic similarity を取って、
    // 逸脱度が高ければ Refused。Phase 1 は anchor の存在だけ確認して素通し。
    if (SafetyHas(_rules, ESafetyRule::RefusalEnforcement)) {
        // TODO(phase2): check semantic distance from _character_anchor
        (void)_character_anchor;
    }

    // ---- Pass: 応答をそのまま返す ------------------------------------------
    r.verdict       = ESafetyVerdict::Pass;
    r.filtered_text = StoreFiltered(llm_response, len);
    return r;
}

} // namespace acs::game
