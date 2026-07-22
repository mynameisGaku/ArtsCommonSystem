// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar U — FLlmSafetyPipeline 実装
//
// 各ルールの実装状況:
//   ・InputValidation: 空 / 長すぎる入力 (> 8KB) を Refused
//   ・JailbreakDetection: 既知の prompt injection 典型句 (大小文字非感受) を Refused
//   ・TokenBudget: byte_len/4 概算でトークン超過を BudgetExceeded
//   ・PiiRedaction: 手書き char スキャナでメール / 電話 / クレカ番号を [REDACTED]
//                  に置換 → Filtered (`<regex>` 不使用、RedactPii 参照)
//   ・RefusalEnforcement: キャラ逸脱 / メタ発言キーワード一致で Refused
//   ・EContentRating: 暴力 / 性的 / 自傷 / ヘイトの 4 軸スコアは **ML 分類器を
//                    要する seam** として意図的に no-op (FMlRuntime と同様に
//                    classifier 注入で有効化する設計。文字列一致は誤検出が多い)
//
// 文字列バッファ:
//   `SafetyResult::filtered_text` が指すのは関数内 thread_local バッファ。
//   次回呼び出しまでに使い切る前提 (header コメント参照)。
#include "gameframework/LlmSafetyPipeline.h"

namespace acs::game {

namespace {

/**
 * strlen 等価の文字列長を返す (`<string.h>` 不使用、明示ループ)。
 *
 * @param s 計測する null 終端文字列 (nullptr 可)。
 * @return 文字数 (nullptr は 0)。
 */
u32 StrLen(const char* s) noexcept {
    if (s == nullptr) return 0;
    u32 n = 0;
    while (s[n] != '\0') ++n;
    return n;
}

/**
 * 大文字を小文字に変換する (ASCII 範囲のみ、multibyte は素通し)。
 *
 * @param c 変換する文字。
 * @return 小文字化した文字。
 */
char ToLowerAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

/**
 * haystack に needle が出現するかを大小文字非感受で判定する (ASCII)。
 *
 * @details needle は短い文字列リテラル前提なので素朴な O(n*m) で実装する。
 * @param haystack 検索対象文字列。
 * @param needle 探す部分文字列。
 * @return 出現すれば true (どちらか nullptr は false、空 needle は true)。
 */
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

/**
 * byte 長から概算トークン数を推定する (1 token ≒ 4 byte、OpenAI 経験則)。
 *
 * @details 厳密 BPE は不要、予算チェック用途には十分。
 * @param byte_len テキストの byte 長。
 * @return 概算トークン数 (ceil(byte_len / 4))。
 */
u32 EstimateTokens(u32 byte_len) noexcept {
    return (byte_len + 3u) / 4u;
}

/**
 * 文字が数字かを返す (ASCII、`<ctype.h>` 不使用)。
 *
 * @param c 判定する文字。
 * @return '0'〜'9' なら true。
 */
bool IsDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

/**
 * 文字が英字かを返す (ASCII)。
 *
 * @param c 判定する文字。
 * @return 'a'〜'z' / 'A'〜'Z' なら true。
 */
bool IsAlpha(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/**
 * メール local-part に許される文字かを返す。
 *
 * @details RFC を厳密に追わず、検出目的で「現実のアドレスを取りこぼさない」最小集合に絞る。
 * @param c 判定する文字。
 * @return local-part に許される文字なら true。
 */
bool IsEmailLocalChar(char c) noexcept {
    return IsAlpha(c) || IsDigit(c)
        || c == '.' || c == '_' || c == '%' || c == '+' || c == '-';
}

/**
 * メール domain-part に許される文字かを返す。
 *
 * @param c 判定する文字。
 * @return domain-part に許される文字なら true。
 */
bool IsEmailDomainChar(char c) noexcept {
    return IsAlpha(c) || IsDigit(c) || c == '.' || c == '-';
}

/**
 * 電話 / クレカの「数字グルーピング区切り」とみなす文字かを返す。
 *
 * @details 数字の連なりを跨いで連続桁数を数えるために許容する。
 * @param c 判定する文字。
 * @return 区切り文字 (空白 / ダッシュ / '.' / 括弧 / '+') なら true。
 */
bool IsPhoneSeparator(char c) noexcept {
    return c == ' ' || c == '-' || c == '.' || c == '(' || c == ')' || c == '+';
}

/** jailbreak 典型句リスト (最低限の防御線。実運用では corpus + 分類器に置き換える)。 */
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

/** kJailbreakPatterns の要素数。 */
constexpr u32 kJailbreakPatternCount =
    sizeof(kJailbreakPatterns) / sizeof(kJailbreakPatterns[0]);

/**
 * RefusalEnforcement キーワードリスト (出力側でモデルがメタ発言・規約逸脱した兆候)。
 *
 * @details
 * LLM 応答が「キャラ設定を破って素のアシスタントに戻った」「禁止トピックに踏み込んだ」
 * 典型句を弾く防御線。"as an ai ..." はキャラ離脱、"ignore previous instructions" は
 * 注入文のエコー、"system prompt" / "jailbreak" は内部情報の漏洩兆候を拾う。
 */
constexpr const char* kRefusalKeywords[] = {
    "as an ai language model",
    "as an ai assistant",
    "i am an ai",
    "i'm an ai",
    "i cannot fulfill",
    "i can't fulfill",
    "ignore previous instructions",
    "my system prompt",
    "the system prompt",
    "jailbreak",
    "developer mode",
};

/** kRefusalKeywords の要素数。 */
constexpr u32 kRefusalKeywordCount =
    sizeof(kRefusalKeywords) / sizeof(kRefusalKeywords[0]);

/** filtered_text バッファのサイズ (= 入力上限と同じ 8KB)。 */
constexpr u32 kFilteredBufSize = 8192;

/**
 * 文字列を thread_local バッファにコピーして返す (filtered_text 用)。
 *
 * @details
 * 入力ポインタの寿命を呼び出し側が保証できない可能性に備えて static thread_local
 * バッファにコピーする。thread_local なので複数スレッドで別 pipeline を回しても
 * 互いに上書きしない。バッファ長を超える分は切り詰める。
 * @param src コピー元文字列 (nullptr なら空文字を返す)。
 * @param len src の byte 長。
 * @return thread_local バッファへのポインタ。
 */
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

/** PII 置換トークン。検出した PII スパンを丸ごとこの文字列で上書きする。 */
constexpr char kRedactToken[]   = "[REDACTED]";

/** kRedactToken の文字数 ('\0' を除く)。 */
constexpr u32  kRedactTokenLen  = sizeof(kRedactToken) - 1u;

/**
 * テキスト位置 i から始まるメールアドレス様トークンを検出する。
 *
 * @details
 * 規則: local-part (1+ 文字) → '@' → domain-part に '.' が 1 つ以上含まれ、かつ
 * '@' の後の domain が英数字で始まる。末尾の '.' / '-' は TLD として不正なので削る。
 * `<regex>` 不使用の手書きスキャナ。i の直前が単語境界であることは呼び出し側が保証済み前提。
 * @param text 走査対象テキスト。
 * @param len text の byte 長。
 * @param i 走査開始位置。
 * @return マッチしたスパンの byte 数 (マッチしなければ 0)。
 */
u32 MatchEmail(const char* text, u32 len, u32 i) noexcept {
    u32 p = i;
    // local-part: 1 文字以上のメール許容文字。'@' 直前なので '.' で終わってもよい
    // (検出が目的、厳密 RFC 検証はしない)。
    u32 local_begin = p;
    while (p < len && IsEmailLocalChar(text[p])) ++p;
    if (p == local_begin) return 0;   // local-part 無し
    if (p >= len || text[p] != '@') return 0;
    ++p;                              // '@' を消費
    // domain: 英数字で始まること
    if (p >= len || !(IsAlpha(text[p]) || IsDigit(text[p]))) return 0;
    u32 domain_begin = p;
    bool has_dot = false;
    while (p < len && IsEmailDomainChar(text[p])) {
        if (text[p] == '.') has_dot = true;
        ++p;
    }
    // 末尾の '.' / '-' は TLD として不正なので削る (例: "a@b.com." → "a@b.com")
    while (p > domain_begin && (text[p - 1] == '.' || text[p - 1] == '-')) --p;
    if (!has_dot) return 0;           // domain に '.' が無ければメールとみなさない
    // '.' を削った結果 domain が消えた / TLD 部が空ならメールでない
    if (p <= domain_begin) return 0;
    return p - i;
}

/**
 * テキスト位置 i から始まる「数字グループ run」を走査する。
 *
 * @details
 * 区切り (空白/ダッシュ/括弧/'+'/'.') を数字に挟む形を 1 つの run とみなす。
 * 電話番号・クレカ番号検出の共通土台。先頭が数字でなければ 0 を返す。末尾の区切りは
 * run に含めない (trim)。
 * @param text 走査対象テキスト。
 * @param len text の byte 長。
 * @param i 走査開始位置。
 * @param out_digit_count run に含まれる純粋な数字桁数の出力先。
 * @return run 全体の byte 数 (末尾区切りを除いたスパン。先頭が数字でなければ 0)。
 */
u32 ScanDigitRun(const char* text, u32 len, u32 i, u32* out_digit_count) noexcept {
    *out_digit_count = 0;
    if (i >= len || !IsDigit(text[i])) return 0;
    u32 p          = i;
    u32 digits     = 0;
    u32 last_digit = i;   // 最後に数字を見た位置 +1 (末尾区切りの trim 用)
    while (p < len) {
        if (IsDigit(text[p])) {
            ++digits;
            ++p;
            last_digit = p;
        } else if (IsPhoneSeparator(text[p])) {
            // 区切りは継続。ただし区切りの次が数字でなければ run はここまで。
            if (p + 1 < len && IsDigit(text[p + 1])) {
                ++p;
            } else {
                break;
            }
        } else {
            break;
        }
    }
    *out_digit_count = digits;
    return last_digit - i;   // 末尾区切りを除いたスパン
}

/**
 * 入力位置 i の直前が単語境界かを返す。
 *
 * @details
 * 直前が「数字 / 英字 / '@' / '_' / '%' / '+'」でないことを単語境界とみなす。
 * これにより数字 run / メールトークンを途中から拾うのを防ぐ。
 * @param text 走査対象テキスト。
 * @param i 検査する位置。
 * @return 直前が単語境界なら true (i==0 も true)。
 */
bool IsWordBoundaryBefore(const char* text, u32 i) noexcept {
    if (i == 0) return true;
    char prev = text[i - 1];
    if (IsDigit(prev) || IsAlpha(prev)) return false;
    if (prev == '@' || prev == '_' || prev == '%' || prev == '+') return false;
    // 区切り (空白/ダッシュ/括弧/'.') は run の一部なので、その手前から
    // run を取りこぼさないよう「直前が数字でなければ境界」とみなす。
    return true;
}

/**
 * [REDACTED] トークンを out[*w] に書き込む。
 *
 * @param out 出力バッファ。
 * @param out_cap out の容量。
 * @param w 書き込み位置 (成功時に進める)。
 * @param out_truncated 容量不足のとき true をセットする出力先。
 * @return 書けたら true、容量不足なら false (呼び出し側は走査を打ち切る)。
 */
bool EmitRedactToken(char* out, u32 out_cap, u32* w, bool* out_truncated) noexcept {
    if (*w + kRedactTokenLen >= out_cap) {
        *out_truncated = true;
        return false;
    }
    for (u32 k = 0; k < kRedactTokenLen; ++k) out[(*w)++] = kRedactToken[k];
    return true;
}

/**
 * PII (メール / 電話 / クレカ) を検出して [REDACTED] に置換する。
 *
 * @details
 * 左から 1 パスの貪欲走査。各位置でまずメールを試し ('@' を含むので数字 run より優先)、
 * メールでなければ単語境界かつ数字始まりの数字 run を測る (桁数 10 以上を電話 / クレカとして
 * 置換、それ未満は誤検出回避で無視)。どれにもマッチしなければ 1 文字コピーする。バッファ
 * 溢れ時は途中で打ち切るが '\0' 終端は保証する。
 * @param src 入力テキスト。
 * @param len src の byte 長。
 * @param out 出力バッファ。
 * @param out_cap out の容量。
 * @param out_truncated バッファ溢れで打ち切ったとき true をセットする出力先。
 * @return 1 件以上置換したら true (= Filtered)、無置換なら false (= Pass)。
 */
bool RedactPii(const char* src, u32 len, char* out, u32 out_cap, bool* out_truncated) noexcept {
    *out_truncated = false;
    bool redacted = false;
    u32  w        = 0;   // 書き込み位置

    u32 i = 0;
    while (i < len) {
        // --- 1. メール検出 (単語境界からのみ) ---
        if (IsWordBoundaryBefore(src, i) && IsEmailLocalChar(src[i])) {
            u32 email_span = MatchEmail(src, len, i);
            if (email_span > 0) {
                if (!EmitRedactToken(out, out_cap, &w, out_truncated)) break;
                redacted = true;
                i += email_span;
                continue;
            }
        }

        // --- 2. 電話 / クレカ検出 (単語境界からの数字 run) ---
        if (IsDigit(src[i]) && IsWordBoundaryBefore(src, i)) {
            u32 digit_count = 0;
            u32 span        = ScanDigitRun(src, len, i, &digit_count);
            // クレカ (13〜16 桁) または電話 (10 桁以上) を PII とみなす。
            if (span > 0 && digit_count >= 10) {
                if (!EmitRedactToken(out, out_cap, &w, out_truncated)) break;
                redacted = true;
                i += span;
                continue;
            }
        }

        // --- 3. 通常文字 ---
        if (w + 1 >= out_cap) { *out_truncated = true; break; }
        out[w++] = src[i++];
    }

    out[w < out_cap ? w : out_cap - 1u] = '\0';
    return redacted;
}

/**
 * PII 置換結果を thread_local バッファに書いて返すラッパ。
 *
 * @details バッファ溢れ時は警告ログを出す。
 * @param src 入力テキスト。
 * @param len src の byte 長。
 * @param out_ptr 置換後テキスト (thread_local バッファ) のポインタ出力先。
 * @return 1 件以上置換したか (= Filtered にすべきか)。
 */
bool StoreRedacted(const char* src, u32 len, const char** out_ptr) noexcept {
    static thread_local char buf[kFilteredBufSize];
    bool truncated = false;
    bool redacted  = RedactPii(src, len, buf, kFilteredBufSize, &truncated);
    if (truncated) {
        ACS_LOG_WARN("FLlmSafetyPipeline: redaction buffer truncated (len=%u)", len);
    }
    *out_ptr = buf;
    return redacted;
}

/** 入力上限 byte 数 (= filtered バッファサイズ - 1)。これ以上は Refused 扱い。 */
constexpr u32 kMaxInputBytes  = kFilteredBufSize - 1u;

/** 出力上限 byte 数 (= filtered バッファサイズ - 1)。これ以上は Refused 扱い。 */
constexpr u32 kMaxOutputBytes = kFilteredBufSize - 1u;

} // namespace

void FLlmSafetyPipeline::Init(ESafetyRule rules) noexcept {
    m_Rules            = rules;
    m_RefusedCount    = 0;
    m_FilteredCount   = 0;
    m_Initialized      = true;
    ACS_LOG_DEBUG("FLlmSafetyPipeline: Init (rules=0x%08X)", static_cast<u32>(rules));
}

void FLlmSafetyPipeline::SetTokenBudget(u32 max_input_tokens, u32 max_output_tokens) noexcept {
    m_MaxInputTokens  = max_input_tokens;
    m_MaxOutputTokens = max_output_tokens;
}

void FLlmSafetyPipeline::SetCharacterAnchor(const char* system_prompt) noexcept {
    m_CharacterAnchor = system_prompt;  // 非所有 (寿命は呼び出し側)
}

bool FLlmSafetyPipeline::IsRuleEnabled(ESafetyRule rule) const noexcept {
    return SafetyHas(m_Rules, rule);
}

void FLlmSafetyPipeline::EnableRule(ESafetyRule rule, bool enable) noexcept {
    if (enable) {
        m_Rules = m_Rules | rule;
    } else {
        m_Rules = static_cast<ESafetyRule>(
            static_cast<u32>(m_Rules) & ~static_cast<u32>(rule));
    }
}

void FLlmSafetyPipeline::Reset() noexcept {
    m_RefusedCount    = 0;
    m_FilteredCount   = 0;
    m_CharacterAnchor = nullptr;
}

FSafetyResult FLlmSafetyPipeline::ValidateInput(const char* user_text) noexcept {
    FSafetyResult r{};
    const u32 len = StrLen(user_text);
    r.input_tokens = EstimateTokens(len);

    // ---- InputValidation: 空 / 過大 ----------------------------------------
    if (SafetyHas(m_Rules, ESafetyRule::InputValidation)) {
        if (len == 0) {
            r.verdict        = ESafetyVerdict::Refused;
            r.refusal_reason = "empty input";
            ++m_RefusedCount;
            return r;
        }
        if (len > kMaxInputBytes) {
            r.verdict        = ESafetyVerdict::Refused;
            r.refusal_reason = "input too long";
            ++m_RefusedCount;
            return r;
        }
    }

    // ---- TokenBudget: 概算トークン超過 -------------------------------------
    if (SafetyHas(m_Rules, ESafetyRule::TokenBudget)
        && m_MaxInputTokens > 0
        && r.input_tokens > m_MaxInputTokens) {
        r.verdict        = ESafetyVerdict::BudgetExceeded;
        r.refusal_reason = "input token budget exceeded";
        ++m_RefusedCount;
        return r;
    }

    // ---- JailbreakDetection: 既知の典型句を大小文字非感受で照合 -----------
    if (SafetyHas(m_Rules, ESafetyRule::JailbreakDetection)) {
        for (u32 i = 0; i < kJailbreakPatternCount; ++i) {
            if (ContainsCaseInsensitive(user_text, kJailbreakPatterns[i])) {
                r.verdict        = ESafetyVerdict::Refused;
                r.refusal_reason = "jailbreak attempt detected";
                ++m_RefusedCount;
                ACS_LOG_WARN("FLlmSafetyPipeline: jailbreak pattern matched ('%s')",
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

FSafetyResult FLlmSafetyPipeline::FilterOutput(const char* llm_response) noexcept {
    FSafetyResult r{};
    const u32 len = StrLen(llm_response);
    r.output_tokens = EstimateTokens(len);

    // ---- 出力長サニティ: 異常に長い出力は Refused -------------------------
    // (= バッファ溢れを未然に止める防御線。InputValidation の鏡像)
    if (len > kMaxOutputBytes) {
        r.verdict        = ESafetyVerdict::Refused;
        r.refusal_reason = "llm response too long";
        ++m_RefusedCount;
        return r;
    }

    // ---- TokenBudget: 概算トークン超過 -------------------------------------
    if (SafetyHas(m_Rules, ESafetyRule::TokenBudget)
        && m_MaxOutputTokens > 0
        && r.output_tokens > m_MaxOutputTokens) {
        r.verdict        = ESafetyVerdict::BudgetExceeded;
        r.refusal_reason = "output token budget exceeded";
        ++m_RefusedCount;
        return r;
    }

    // ---- RefusalEnforcement: キャラ逸脱 / メタ発言 / 規約逸脱を弾く ----------
    // モデルが「キャラを脱いで素のアシスタントに戻った」「内部情報を漏らした」
    // 典型句 (kRefusalKeywords) を文字列一致で検出する。1 つでも当たれば、その
    // 応答は NPC のセリフとして表示せず Refused にする (= 上位は NPC を黙らせる /
    // 別セリフに差し替える)。redaction より先に gate するのは、refuse する応答を
    // わざわざ redact しても無駄だから。
    //
    // 設計の seam: 文字列一致は「明確な逸脱」しか捕まえない。曖昧な逸脱
    // (キャラ性格との semantic 距離) は m_CharacterAnchor を埋め込み比較する
    // 分類器が必要 = EContentRating と同じく ML レイヤの差し込み口として残す。
    if (SafetyHas(m_Rules, ESafetyRule::RefusalEnforcement)) {
        for (u32 i = 0; i < kRefusalKeywordCount; ++i) {
            if (ContainsCaseInsensitive(llm_response, kRefusalKeywords[i])) {
                r.verdict        = ESafetyVerdict::Refused;
                r.refusal_reason = "character anchor violation";
                ++m_RefusedCount;
                ACS_LOG_WARN("FLlmSafetyPipeline: refusal keyword matched ('%s')",
                             kRefusalKeywords[i]);
                return r;
            }
        }
        (void)m_CharacterAnchor;  // semantic 比較 (ML seam) で将来使用
    }

    // ---- EContentRating (ML seam): 危険スコア超過チェック -------------------
    // 暴力 / 性的 / 自傷 / ヘイトの 4 軸スコアは学習済み分類器を要するため、
    // ここはモデル差し込み口として意図的に no-op で残す (FMlRuntime と同様に、
    // 別途 classifier を注入して本軸を有効化する設計)。文字列一致で代替すると
    // 誤検出 (false positive) が多く、ゲーム体験を壊すため敢えて実装しない。
    if (SafetyHas(m_Rules, ESafetyRule::ContentRating)) {
        // 分類器未注入のため判定を保留 (= Pass 継続)。注入後は score > threshold で
        // r.verdict = Refused / ++m_RefusedCount。
    }

    // ---- PiiRedaction: メール / 電話 / クレカ番号を [REDACTED] に置換 --------
    // 手書き char スキャナ (RedactPii) で、メール様トークン (@ + dot)、電話様
    // 10 桁以上の数字 run、クレカ様 13〜16 桁 run を検出し置換する。1 件でも
    // 置換したら Filtered として置換済みテキストを返す。
    if (SafetyHas(m_Rules, ESafetyRule::PiiRedaction)) {
        const char* redacted_ptr = nullptr;
        const bool  redacted     = StoreRedacted(llm_response, len, &redacted_ptr);
        if (redacted) {
            r.verdict       = ESafetyVerdict::Filtered;
            r.filtered_text = redacted_ptr;
            ++m_FilteredCount;
            ACS_LOG_DEBUG("FLlmSafetyPipeline: PII redacted from response");
            return r;
        }
        // 無置換 → Pass。redacted_ptr は入力コピーそのものなので再利用する。
        r.verdict       = ESafetyVerdict::Pass;
        r.filtered_text = redacted_ptr;
        return r;
    }

    // ---- Pass: 応答をそのまま返す ------------------------------------------
    r.verdict       = ESafetyVerdict::Pass;
    r.filtered_text = StoreFiltered(llm_response, len);
    return r;
}

} // namespace acs::game
