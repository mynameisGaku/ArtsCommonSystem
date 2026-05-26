// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar U Phase 2 — ContentModerator 実装 (Stub のみ)
//
// 本 .cpp では `IContentModerator` 自体は提供せず、外部 SDK 非依存で常に使える
// `FContentModeratorStub` のみを実装する。Azure Content Safety / Google Perspective /
// OpenAI Moderation / Hive 等の実 SDK 実装は別モジュールで本 I/F を override する。
//
// Stub の判定ロジック (Phase 1):
//   1. kHardcodedBlockWords[] のいずれかにヒット
//      → 例外なく verdict=Block / rating=SexualMinor_HardcodedBlock。
//        Allow に降格する経路は存在しない (= 法令遵守の最重要ガードレール)。
//   2. kBlockedWords[] のいずれかにヒット
//      → verdict=Block / rating=Mature
//   3. それ以外 → verdict=Allow / rating=Safe
//
// 文字列照合は ASCII 大小文字非感受で素朴な O(n*m) 実装。本格的な多言語対応は
// Phase 2 (実 SDK 接続) に移譲。
#include "gameframework/ContentModerator.h"

#include "foundation/Error.h"

namespace acs::game {

namespace {

// ---- 内部: 文字列長 (`<string.h>` 不使用、明示ループ) ----------------------
// strlen 等価。nullptr は 0 を返す (呼び出し側で null チェックを省略可能に)。
u64 StrLen(const char* s) noexcept {
    if (s == nullptr) return 0;
    u64 n = 0;
    while (s[n] != '\0') ++n;
    return n;
}

// ASCII 大文字 → 小文字。multibyte は素通し。
char ToLowerAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

// haystack に needle が出現するか (大小文字非感受、ASCII)。
// needle が空 ("") なら常に true。haystack==nullptr は常に false。
// needle は短いリテラル前提で素朴な O(n*m)。
bool ContainsCaseInsensitive(const char* haystack, const char* needle) noexcept {
    if (haystack == nullptr || needle == nullptr) return false;
    if (needle[0] == '\0') return true;
    for (u64 i = 0; haystack[i] != '\0'; ++i) {
        u64 k = 0;
        while (needle[k] != '\0'
               && haystack[i + k] != '\0'
               && ToLowerAscii(haystack[i + k]) == ToLowerAscii(needle[k])) {
            ++k;
        }
        if (needle[k] == '\0') return true;  // 完全一致
    }
    return false;
}

// ---- ハードコード Block 辞書 (= 児童性的搾取関連 keyword) ------------------
// このリストにヒットした場合、verdict は **必ず Block** で rating は
// SexualMinor_HardcodedBlock。Allow / Warn に降格する経路は存在しない。
// 実 SDK 接続後もこのガードは seam 層 (Stub) に残し、二重防壁として機能させる。
//
// NOTE: ASCII リテラルで明示的に書く。日本語 / 中国語 / 露語等の追加は Phase 2 で
//       外部 patterns ファイル (i18n 辞書) に分離する。
constexpr const char* kHardcodedBlockWords[] = {
    "child porn",
    "cp video",       // "child porn" の隠語
    "underage sex",
    "minor sex",
    "lolicon porn",
    "shotacon porn",
};
constexpr u64 kHardcodedBlockWordCount =
    sizeof(kHardcodedBlockWords) / sizeof(kHardcodedBlockWords[0]);

// ---- 一般 Block 辞書 (= NG ワード) -----------------------------------------
// 軽度〜重度の不適切表現。実 SDK 未統合時の最低限の防御線として ASCII 英語のみ。
// Phase 2 で多言語 / 文脈考慮型分類器に置き換える。
constexpr const char* kBlockedWords[] = {
    "fuck",
    "shit",
    "bitch",
    "nigger",
    "faggot",
    "rape",
    "kill yourself",
    "kys",
};
constexpr u64 kBlockedWordCount =
    sizeof(kBlockedWords) / sizeof(kBlockedWords[0]);

// ---- 内部判定: text モデレーション本体 ------------------------------------
// kHardcodedBlockWords を先に評価することで、`SexualMinor` 関連 keyword に
// 後段の Allow 降格を絶対に許さない契約を保証する。
FModerationResult ClassifyText(const char* text) noexcept {
    FModerationResult r{};
    if (text == nullptr || text[0] == '\0') {
        // 空文字は Allow (= 上位層で長さチェックする責務)。
        r.verdict = EModerationVerdict::Allow;
        r.rating  = EContentRating::Safe;
        r.reason  = nullptr;
        return r;
    }

    // ---- (1) ハードコード Block: SexualMinor 関連 keyword -----------------
    for (u64 i = 0; i < kHardcodedBlockWordCount; ++i) {
        if (ContainsCaseInsensitive(text, kHardcodedBlockWords[i])) {
            r.verdict = EModerationVerdict::Block;
            r.rating  = EContentRating::SexualMinor_HardcodedBlock;
            r.reason  = "hardcoded block: sexual content involving minors";
            return r;
        }
    }

    // ---- (2) 一般 NG ワード -------------------------------------------------
    for (u64 i = 0; i < kBlockedWordCount; ++i) {
        if (ContainsCaseInsensitive(text, kBlockedWords[i])) {
            r.verdict = EModerationVerdict::Block;
            r.rating  = EContentRating::Mature;
            r.reason  = "blocked: contains prohibited word";
            return r;
        }
    }

    // ---- (3) Allow ----------------------------------------------------------
    r.verdict = EModerationVerdict::Allow;
    r.rating  = EContentRating::Safe;
    r.reason  = nullptr;
    return r;
}

} // namespace

// =============================================================================
// FContentModeratorStub 実装
// =============================================================================

TResult<FModerationResult> FContentModeratorStub::ModerateText(const char* user_id, const char* text) noexcept {
    (void)user_id;  // Stub では未使用 (実 SDK では通報履歴・レピュテーション参照に使う)
    return ClassifyText(text);
}

TResult<FModerationResult> FContentModeratorStub::ModerateImage(const char* user_id, const u8* image_data, u64 size) noexcept {
    (void)user_id;
    if (image_data == nullptr || size == 0) {
        return TResult<FModerationResult>(
            ACS_ERR(Generic, kSubContentModeratorBadArgument,
                    "FContentModeratorStub::ModerateImage: image_data == nullptr or size == 0"));
    }
    // TODO(phase2): 画像 NSFW 分類器 (= ローカル ONNX or リモート API) を導入。
    // 現状は全部 Allow で素通し。実装後は kHardcodedBlockWords と同等の二段判定
    // (= SexualMinor 検出時はハードコード Block) を画像側にも適用すること。
    FModerationResult r{};
    r.verdict = EModerationVerdict::Allow;
    r.rating  = EContentRating::Safe;
    r.reason  = nullptr;
    return r;
}

TResult<FModerationResult> FContentModeratorStub::ModerateUserName(const char* name) noexcept {
    // ユーザー名 NG チェックは text と同じ辞書を流用 (Stub なので簡略化)。
    // 実 SDK では「短い文字列内のなりすまし / leet speak / 同形異義字」検出を
    // 強化したサブ API を別途用意することが多い。
    return ClassifyText(name);
}

void FContentModeratorStub::Tick(f32 dt) noexcept {
    (void)dt;  // Stub は非同期キューを持たないので no-op。
}

// =============================================================================
// GetModeratorStub() — Meyer's singleton
// -----------------------------------------------------------------------------
// C++11 以降、関数スコープ static の初期化は thread-safe。追加同期は不要。
// =============================================================================
IContentModerator& GetModeratorStub() noexcept {
    static FContentModeratorStub _instance;
    return _instance;
}

} // namespace acs::game
