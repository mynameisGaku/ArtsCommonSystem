// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar U Phase 2 — FContentModerator 実装 (Stub のみ)
//
// 本 .cpp では `IContentModerator` 自体は提供せず、外部 SDK 非依存で常に使える
// `ContentModeratorStub` のみを実装する。Azure Content Safety / Google Perspective /
// OpenAI Moderation / Hive 等の実 SDK 実装は別モジュールで本 I/F を override する。
//
// Stub の判定ロジック (ローカルテキストフィルタ — 本実装):
//   テキストは 2 経路で照合する:
//     (a) 生文字列に対する ASCII 大小文字非感受の contain チェック (素朴 O(n*m))。
//     (b) **正規化文字列**に対する contain チェック。正規化は
//         「ASCII 小文字化 + leet 数字置換 (0→o,1→i,3→e,4→a,5→s,7→t,@→a,$→s) +
//          区切り (空白 / 記号) 除去」を行い、`f u c k` / `f.u.c.k` / `sh1t` 風の
//          spaced / 記号挿入 / 桁置換変種を 1 つの辞書エントリで捕捉する。
//   どちらかの経路でヒットしたら Block。優先順位:
//     1. kHardcodedBlockWords[] にヒット (生 or 正規化)
//        → 例外なく verdict=Block / rating=SexualMinor_HardcodedBlock。
//          Allow に降格する経路は存在しない (= 法令遵守の最重要ガードレール)。
//     2. kBlockedWords[] にヒット (生 or 正規化)
//        → verdict=Block / rating=Mature
//     3. それ以外 → verdict=Allow / rating=Safe
//
// 文字列照合は ASCII 大小文字非感受で素朴な O(n*m) 実装。多言語 (日本語 / 中国語 /
// アラビア語) 辞書や文脈考慮型分類器は実 SDK 接続側 (別モジュール) に委譲する。
#include "gameframework/ContentModerator.h"

#include "foundation/Error.h"
#include "asset/ImageAsset.h"   // 画像 NSFW heuristic 用のローカルデコード (stb_image)
#include "container/Array.h"
#include "memory/Memory.h"      // MemCopy

namespace acs::game {

namespace {

// =============================================================================
// 画像 NSFW ローカル heuristic (skin-tone 比率法)
// -----------------------------------------------------------------------------
// 外部 SDK / クラウド API なしで動く実分類器。よく知られた肌色ピクセル比率
// (Kovac et al. の RGB 肌色判定) を使い、画像中の肌色ピクセル割合から露出度を
// 推定する。完璧ではない (顔のアップ等で false positive、未成年検出は不可) が、
// 「常に Allow」の seam ではなく、ピクセルを実際に解析して判定する本実装。
//   ・skin_ratio >= 0.55 → Explicit / Block
//   ・skin_ratio >= 0.35 → Mature   / Warn   (要手動確認 = borderline)
//   ・skin_ratio >= 0.18 → Mild     / Allow
//   ・それ未満           → Safe     / Allow
// 透明ピクセル (alpha < 16) は UI パディング等とみなし母数から除外する。
// 注意: 未成年性的搾取 (SexualMinor_HardcodedBlock) は肌色比率では検出できない。
//   その rating は専用の年齢/CSAM 分類器が必須で、本 heuristic の範囲外 (正直)。

// Kovac et al. (2003) のアップライト肌色判定ルール (sRGB 8-bit 前提)。
bool IsSkinTone(u8 r, u8 g, u8 b) noexcept {
    const i32 ri = r, gi = g, bi = b;
    i32 mx = ri; if (gi > mx) mx = gi; if (bi > mx) mx = bi;
    i32 mn = ri; if (gi < mn) mn = gi; if (bi < mn) mn = bi;
    const i32 absRG = (ri > gi) ? (ri - gi) : (gi - ri);
    return ri > 95 && gi > 40 && bi > 20 &&
           (mx - mn) > 15 &&
           absRG > 15 &&
           ri > gi && ri > bi;
}

// RGBA8 ピクセル列から肌色比率を求める。母数は不透明ピクセル数。
f32 SkinRatioRgba8(const u8* rgba, u64 pixel_count) noexcept {
    u64 opaque = 0;
    u64 skin   = 0;
    for (u64 i = 0; i < pixel_count; ++i) {
        const u8 a = rgba[i * 4 + 3];
        if (a < 16) continue;            // 透明 = パディング扱いで除外
        ++opaque;
        if (IsSkinTone(rgba[i * 4 + 0], rgba[i * 4 + 1], rgba[i * 4 + 2])) ++skin;
    }
    if (opaque == 0) return 0.0f;
    return static_cast<f32>(skin) / static_cast<f32>(opaque);
}

// HDR (float RGBA) 用: 各成分を [0,1] clamp → 8-bit 量子化して同じ判定にかける。
f32 SkinRatioRgbaF32(const f32* rgba, u64 pixel_count) noexcept {
    u64 opaque = 0;
    u64 skin   = 0;
    for (u64 i = 0; i < pixel_count; ++i) {
        const f32 af = rgba[i * 4 + 3];
        if (af < 0.0625f) continue;
        ++opaque;
        auto q = [](f32 v) noexcept -> u8 {
            if (v <= 0.0f) return 0;
            if (v >= 1.0f) return 255;
            return static_cast<u8>(v * 255.0f + 0.5f);
        };
        if (IsSkinTone(q(rgba[i * 4 + 0]), q(rgba[i * 4 + 1]), q(rgba[i * 4 + 2]))) ++skin;
    }
    if (opaque == 0) return 0.0f;
    return static_cast<f32>(skin) / static_cast<f32>(opaque);
}

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

// ---- 内部: leet 数字 / 記号 → アルファベット置換 --------------------------
// 桁・記号で母音/子音を偽装する典型的な leet speak を 1 文字単位で正規化する。
// 該当しない文字はそのまま返す (大文字は後段の ToLowerAscii で潰す)。
char DeLeet(char c) noexcept {
    switch (c) {
        case '0': return 'o';
        case '1': return 'i';   // '1' は l/i 双方に使われるが、頻出の i に寄せる
        case '3': return 'e';
        case '4': return 'a';
        case '5': return 's';
        case '7': return 't';
        case '@': return 'a';
        case '$': return 's';
        default:  return c;
    }
}

// ---- 内部: 区切り文字判定 (正規化で除去する文字) --------------------------
// 英数字 (a-z / A-Z / 0-9) 以外をすべて区切りとみなす。空白・記号・句読点を除去し、
// `f u c k` / `f.u.c.k` / `f-u-c-k` を `fuck` に畳む。multibyte (0x80-) も除去対象。
bool IsSeparator(char c) noexcept {
    const bool is_alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    const bool is_digit = (c >= '0' && c <= '9');
    return !(is_alpha || is_digit);
}

// 正規化バッファ長 (= 入力をこのサイズまで詰めて判定)。超過分は切り捨て。
// チャット / ユーザー名想定の上限。NG ワードは短いので末尾切り捨ての実害は小さい。
constexpr u64 kNormalizedBufSize = 512;
// needle 側 (辞書エントリ) 正規化バッファ。NG ワードは短いリテラル前提。
constexpr u64 kNeedleBufSize = 64;

// ---- 内部: 文字列正規化 (汎用) --------------------------------------------
// 入力を「区切り除去 → leet 置換 → ASCII 小文字化」して `dst` へ詰め、NUL 終端する。
// `src` が nullptr のときは空文字列を書き込む。`cap` は dst の容量 (終端込み)。
void NormalizeInto(const char* src, char* dst, u64 cap) noexcept {
    u64 n = 0;
    if (src != nullptr && cap > 0) {
        for (u64 i = 0; src[i] != '\0' && n + 1 < cap; ++i) {
            const char c = src[i];
            if (IsSeparator(c)) continue;        // 空白・記号を畳む
            dst[n++] = ToLowerAscii(DeLeet(c));  // leet 置換 → 小文字
        }
    }
    if (cap > 0) dst[n] = '\0';
}

// ---- 内部: 入力テキスト正規化 ---------------------------------------------
// 入力を正規化して thread_local バッファへ詰め、先頭ポインタを返す (常に NUL 終端)。
// 寿命は次回 NormalizeText() 呼び出しまで (= 1 回の Classify 内で消費する)。
const char* NormalizeText(const char* src) noexcept {
    static thread_local char buf[kNormalizedBufSize];
    NormalizeInto(src, buf, kNormalizedBufSize);
    return buf;
}

// ---- 内部: 生文字列 or 正規化文字列のいずれかにヒットするか ----------------
// 2 経路で照合する:
//   - 生 `text` に対する大小文字非感受 contain (素の表記・スペース込みの語をそのまま捕捉)
//   - 正規化 `normalized` に対する「正規化済み needle」の contain
//     (spaced / leet / 記号挿入の変種を捕捉)。needle 側も同じ規則で畳むので、
//     辞書に "kill yourself" のような空白入りリテラルを書いても正しく一致する。
// `normalized` は呼び出し側で 1 度だけ作って使い回す。
bool MatchesRawOrNormalized(const char* text, const char* normalized, const char* needle) noexcept {
    if (ContainsCaseInsensitive(text, needle)) return true;
    char needle_norm[kNeedleBufSize];
    NormalizeInto(needle, needle_norm, kNeedleBufSize);
    if (needle_norm[0] == '\0') return false;  // 正規化後に空になる needle は無視
    return ContainsCaseInsensitive(normalized, needle_norm);
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
ModerationResult ClassifyText(const char* text) noexcept {
    ModerationResult r{};
    if (text == nullptr || text[0] == '\0') {
        // 空文字は Allow (= 上位層で長さチェックする責務)。
        r.verdict = EModerationVerdict::Allow;
        r.rating  = EContentRating::Safe;
        r.reason  = nullptr;
        return r;
    }

    // 正規化文字列を 1 度だけ作り、両辞書の照合で使い回す。
    // (生表記 + spaced/leet 変種の双方を捕捉するため raw と normalized を併用)
    const char* normalized = NormalizeText(text);

    // ---- (1) ハードコード Block: SexualMinor 関連 keyword -----------------
    for (u64 i = 0; i < kHardcodedBlockWordCount; ++i) {
        if (MatchesRawOrNormalized(text, normalized, kHardcodedBlockWords[i])) {
            r.verdict = EModerationVerdict::Block;
            r.rating  = EContentRating::SexualMinor_HardcodedBlock;
            r.reason  = "hardcoded block: sexual content involving minors";
            return r;
        }
    }

    // ---- (2) 一般 NG ワード -------------------------------------------------
    for (u64 i = 0; i < kBlockedWordCount; ++i) {
        if (MatchesRawOrNormalized(text, normalized, kBlockedWords[i])) {
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
// ContentModeratorStub 実装
// =============================================================================

TResult<ModerationResult> ContentModeratorStub::ModerateText(const char* user_id, const char* text) noexcept {
    (void)user_id;  // Stub では未使用 (実 SDK では通報履歴・レピュテーション参照に使う)
    return ClassifyText(text);
}

TResult<ModerationResult> ContentModeratorStub::ModerateImage(const char* user_id, const u8* image_data, u64 size) noexcept {
    (void)user_id;
    if (image_data == nullptr || size == 0) {
        return TResult<ModerationResult>(
            ACS_ERR(Generic, kSubContentModeratorBadArgument,
                    "ContentModeratorStub::ModerateImage: image_data == nullptr or size == 0"));
    }
    // ---- 画像 NSFW ローカル本実装 -----------------------------------------
    // 外部 SDK なしで実際にピクセルを解析する: stb_image (asset モジュール) で
    // RGBA にデコード → 肌色比率 heuristic で露出度を判定する。デコード不能な
    // バイト列は BadArgument。SexualMinor の専用判定だけは肌色比率では不可能な
    // ため範囲外 (要 専用分類器)。詳細は SkinRatioRgba8 / ClassifyImageRgba8。
    ImageAssetLoader loader;
    TArray<byte> encoded;
    encoded.Resize(static_cast<usize>(size));
    MemCopy(encoded.Data(), image_data, static_cast<usize>(size));
    auto decoded = loader.LoadFromBytes(FAssetId{0}, encoded);
    if (decoded.IsErr()) {
        return TResult<ModerationResult>(
            ACS_ERR(Generic, kSubContentModeratorBadArgument,
                    "ContentModeratorStub::ModerateImage: undecodable image bytes"));
    }
    auto* img = static_cast<FImageAsset*>(decoded.Value().Get());
    const u64 px = static_cast<u64>(img->Width()) * static_cast<u64>(img->Height());
    f32 ratio = 0.0f;
    if (img->EFormat() == EPixelFormat::R32G32B32A32_F) {
        ratio = SkinRatioRgbaF32(reinterpret_cast<const f32*>(img->Pixels()), px);
    } else {
        // ImageAssetLoader は LDR を必ず 4ch RGBA8 に正規化する。
        ratio = SkinRatioRgba8(reinterpret_cast<const u8*>(img->Pixels()), px);
    }
    return ClassifyImageRatio(ratio);
}

// 肌色比率 → ModerationResult のしきい値判定 (ModerateImage と RGBA 直接判定で共用)。
TResult<ModerationResult> ContentModeratorStub::ClassifyImageRatio(f32 skin_ratio) const noexcept {
    ModerationResult r{};
    if (skin_ratio >= 0.55f) {
        r.verdict = EModerationVerdict::Block;
        r.rating  = EContentRating::Explicit;
        r.reason  = "image NSFW heuristic: very high skin-tone ratio (explicit)";
    } else if (skin_ratio >= 0.35f) {
        r.verdict = EModerationVerdict::Warn;
        r.rating  = EContentRating::Mature;
        r.reason  = "image NSFW heuristic: elevated skin-tone ratio (borderline, needs review)";
    } else if (skin_ratio >= 0.18f) {
        r.verdict = EModerationVerdict::Allow;
        r.rating  = EContentRating::Mild;
        r.reason  = nullptr;
    } else {
        r.verdict = EModerationVerdict::Allow;
        r.rating  = EContentRating::Safe;
        r.reason  = nullptr;
    }
    return r;
}

// 既にデコード済みの RGBA8 ピクセルを直接判定する公開 API (エンジンが既に
// テクスチャを CPU 側に持っている場合に再デコードを避けられる)。
TResult<ModerationResult> ContentModeratorStub::ModerateImageRgba8(
        const u8* rgba, u32 width, u32 height) noexcept {
    if (rgba == nullptr || width == 0 || height == 0) {
        return TResult<ModerationResult>(
            ACS_ERR(Generic, kSubContentModeratorBadArgument,
                    "ContentModeratorStub::ModerateImageRgba8: null pixels or zero dimension"));
    }
    const f32 ratio = SkinRatioRgba8(rgba, static_cast<u64>(width) * static_cast<u64>(height));
    return ClassifyImageRatio(ratio);
}

TResult<ModerationResult> ContentModeratorStub::ModerateUserName(const char* name) noexcept {
    // ユーザー名 NG チェックは text と同じ辞書を流用 (Stub なので簡略化)。
    // 実 SDK では「短い文字列内のなりすまし / leet speak / 同形異義字」検出を
    // 強化したサブ API を別途用意することが多い。
    return ClassifyText(name);
}

void ContentModeratorStub::Tick(f32 dt) noexcept {
    (void)dt;  // Stub は非同期キューを持たないので no-op。
}

// =============================================================================
// GetModeratorStub() — Meyer's singleton
// -----------------------------------------------------------------------------
// C++11 以降、関数スコープ static の初期化は thread-safe。追加同期は不要。
// =============================================================================
IContentModerator& GetModeratorStub() noexcept {
    static ContentModeratorStub m_Instance;
    return m_Instance;
}

} // namespace acs::game
