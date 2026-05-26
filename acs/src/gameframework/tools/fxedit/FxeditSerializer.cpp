// SPDX-License-Identifier: Apache-2.0
// GameFramework Tools — `.fxedit` テキストシリアライザ実装 (Phase 19b)
//
// テキスト形式の詳細仕様は FxeditSerializer.h を参照。
//
// 実装の主な決定:
//   ・I/O は `acs::FFileSystem::WriteAllText` / `ReadAllText` に委譲。これにより
//     wchar_t パス + GetLastError 連携 + atomic な置き換え無しの単純上書き
//     セマンティクスが他の ACS file 操作と一致する。
//   ・出力 buffer は `acs::TArray<char>` で動的成長させる (emitter 数に依存)。
//     `Reserve(count * kMaxBytesPerEmitter + 64)` で最初から十分な容量を確保し、
//     `AppendXxx` が再 alloc しないようにする (= STL `<sstream>` 不要)。
//   ・数値出力は `std::snprintf "%g"`。完全往復 (round-trip) は保証しないが
//     "目で見て分かる数値" を優先する (= game dev tool 用途、6 桁有効で十分)。
//   ・パースは strtok 等を使わず手書き状態機械にする (`<cstring>` の strtok は
//     非 thread-safe で warning が出るプラットフォームがある)。
//
// 注意:
//   ・本 .cpp は FParticleEmitterDef の完全型を必要とするので、ヘッダ側の
//     forward decl だけでなく `ParticleEffectSystem.h` を include する。
//   ・実 FParticleEmitterDef の color は FVec3 (alpha 無し)、gravity は FVec2。
//     テキスト形式は前方互換用に色 4 成分 / 重力 3 成分まで書く / 読むが、
//     格納できない第 4 成分 (color) / 第 3 成分 (gravity) は破棄する。
//   ・spread_radians は FParticleEmitterDef にフィールドが無いため、書き出し時は
//     0 で固定、読み込み時はパースして捨てる (将来フィールド追加時に値結合)。

#include "gameframework/tools/fxedit/FxeditSerializer.h"

#include "gameframework/ParticleEffectSystem.h"  // ParticleEmitterDef 完全型
#include "platform/FileSystem.h"                 // ReadAllText / WriteAllText
#include "foundation/Log.h"                      // ACS_LOG_WARN
#include "container/Array.h"                     // Array<char>

#include <cstdio>    // std::snprintf
#include <cstdlib>   // std::strtof
#include <cstring>   // std::memcpy / std::strncmp

namespace acs::game::fxedit {

// =============================================================================
// 内部ヘルパ
// =============================================================================
namespace {

// ASCII printable 範囲かどうか (' ' を除く制御文字を排除する用途)。
inline bool IsAsciiPrintable(char c) noexcept {
    return c >= 0x20 && c < 0x7F;
}

// 任意の終端文字列について長さを計算 (max_len で打ち切り)。
// `std::strlen` 相当だが上限付きで NUL なし入力の暴走を防ぐ。
inline usize StrLenBounded(const char* s, usize max_len) noexcept {
    if (s == nullptr) return 0;
    usize n = 0;
    while (n < max_len && s[n] != '\0') ++n;
    return n;
}

// out_buf に NUL 終端文字列を append し、終端 NUL は書かない (まとめて
// 後で書く)。capacity を超えそうな場合は false を返す。
inline bool AppendStr(TArray<char>& out, const char* s, usize len) noexcept {
    const usize old = out.Size();
    out.Resize(old + len);
    if (out.Size() != old + len) return false;  // alloc 失敗
    std::memcpy(out.Data() + old, s, len);
    return true;
}

inline bool AppendChar(TArray<char>& out, char c) noexcept {
    const usize old = out.Size();
    out.Resize(old + 1);
    if (out.Size() != old + 1) return false;
    out[old] = c;
    return true;
}

// "<key> v0 [v1 [v2 [v3]]]\n" 行を append する。
//  ・count = 値の個数 (1..4)。
//  ・prefix が non-null なら "<prefix> " を頭に付ける ("E0 " など)。
bool AppendKeyValueLine(TArray<char>& out,
                        const char*  prefix,
                        const char*  key,
                        const f32*   values,
                        u32          count) noexcept {
    char line[160];
    int n = 0;
    if (prefix != nullptr) {
        n = std::snprintf(line, sizeof(line), "%s %s", prefix, key);
    } else {
        n = std::snprintf(line, sizeof(line), "%s", key);
    }
    if (n < 0 || static_cast<usize>(n) >= sizeof(line)) return false;
    for (u32 i = 0; i < count && i < 4u; ++i) {
        int add = std::snprintf(line + n, sizeof(line) - static_cast<usize>(n),
                                " %g", values[i]);
        if (add < 0) return false;
        n += add;
        if (static_cast<usize>(n) >= sizeof(line)) return false;
    }
    if (static_cast<usize>(n) + 1 >= sizeof(line)) return false;
    line[n++] = '\n';
    return AppendStr(out, line, static_cast<usize>(n));
}

// "<prefix> name \"<value>\"\n" を append する (引用符付き文字列 key 用)。
bool AppendNameLine(TArray<char>& out,
                    const char*  prefix,
                    const char*  name) noexcept {
    char line[96];
    const char* safe_name = name != nullptr ? name : "";
    // 簡素化のため、name 内の `"` は `'` に置き換える防御。
    char sanitized[FFxeditSerializer::kMaxEmitterName + 1];
    usize j = 0;
    for (usize i = 0;
         safe_name[i] != '\0' && j < FFxeditSerializer::kMaxEmitterName;
         ++i) {
        char c = safe_name[i];
        if (!IsAsciiPrintable(c) || c == '"') c = '_';
        sanitized[j++] = c;
    }
    sanitized[j] = '\0';

    int n = std::snprintf(line, sizeof(line), "%s name \"%s\"\n",
                          prefix, sanitized);
    if (n < 0 || static_cast<usize>(n) >= sizeof(line)) return false;
    return AppendStr(out, line, static_cast<usize>(n));
}

// 1 行をテキスト本体から取り出す。
//  text[pos..] から `\n` または `\0` まで読み、行内容を out_line に書き込み
//  (NUL 終端付き、末尾 \r は除去)、`pos` を次の行頭まで進める。
//  out_line は kMaxLineLength + 1 バイト確保しておく前提。
//  戻り値: 何か行を取り出した (空行含む) なら true、入力が末端なら false。
bool ReadOneLine(const char* text,
                 usize       text_len,
                 usize&      pos,
                 char*       out_line,
                 usize       out_line_capacity) noexcept {
    if (pos >= text_len || text[pos] == '\0') return false;
    usize n = 0;
    while (pos < text_len && text[pos] != '\n' && text[pos] != '\0') {
        if (n + 1 < out_line_capacity) {  // 末尾 NUL 用に 1 残す
            out_line[n++] = text[pos];
        }
        ++pos;
    }
    if (pos < text_len && text[pos] == '\n') ++pos;
    // 末尾 \r を除去 (Windows CRLF 対応)。
    if (n > 0 && out_line[n - 1] == '\r') --n;
    out_line[n] = '\0';
    return true;
}

// "ACS_FXEDIT" を line から探して version を読む。
// 失敗時は 0 を返す。
u32 ParseMagicLine(const char* line) noexcept {
    const char* p = line;
    // 先頭の空白スキップ。
    while (*p == ' ' || *p == '\t') ++p;
    const char* magic = FFxeditSerializer::kMagic;
    usize mlen = 0;
    while (magic[mlen] != '\0') ++mlen;
    if (std::strncmp(p, magic, mlen) != 0) return 0;
    p += mlen;
    if (*p != ' ' && *p != '\t') return 0;
    while (*p == ' ' || *p == '\t') ++p;
    // 残りを十進数として読む。
    u32 v = 0;
    bool any = false;
    while (*p >= '0' && *p <= '9') {
        v = v * 10u + static_cast<u32>(*p - '0');
        ++p;
        any = true;
    }
    return any ? v : 0u;
}

// "E<digits>" prefix から emitter index を取り出す。失敗時は false。
// key_begin..key_end は line 内のスライス (key_end は exclusive)。
bool ParseEmitterIndex(const char* key_begin,
                      const char* key_end,
                      u32&        out_index) noexcept {
    if (key_begin >= key_end) return false;
    if (*key_begin != 'E' && *key_begin != 'e') return false;
    const char* p = key_begin + 1;
    if (p >= key_end || *p < '0' || *p > '9') return false;
    u32 v = 0;
    while (p < key_end && *p >= '0' && *p <= '9') {
        v = v * 10u + static_cast<u32>(*p - '0');
        ++p;
    }
    if (p != key_end) return false;
    out_index = v;
    return true;
}

// key 文字列が固定リテラル lit と等しいか比較 (key は空白で区切られたスライス)。
bool KeyEquals(const char* key_begin,
              const char* key_end,
              const char* lit) noexcept {
    usize i = 0;
    while (key_begin + i < key_end && lit[i] != '\0') {
        if (key_begin[i] != lit[i]) return false;
        ++i;
    }
    return (key_begin + i == key_end) && lit[i] == '\0';
}

// `"<value>"` 形式から中身を抽出して dst に格納する (NUL 終端、上限あり)。
// 引用符が無い場合は失敗 → false。
bool ExtractQuotedName(const char* line,
                      char*       dst,
                      usize       dst_capacity) noexcept {
    const char* p = line;
    while (*p != '\0' && *p != '"') ++p;
    if (*p != '"') return false;
    ++p;
    usize i = 0;
    while (*p != '\0' && *p != '"' && i + 1 < dst_capacity) {
        dst[i++] = *p;
        ++p;
    }
    dst[i] = '\0';
    return true;
}

} // namespace

// =============================================================================
// SkipWhitespace
// =============================================================================
const char* FFxeditSerializer::SkipWhitespace(const char* p) noexcept {
    if (p == nullptr) return nullptr;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    return p;
}

// =============================================================================
// ParseLine — "<key> v0 [v1] [v2] [v3]" を構文解析
// -----------------------------------------------------------------------------
// out_key は line 内の先頭文字を指すポインタ (NUL 終端ではない、長さは
// 呼出側で再計算)。値が存在しないスロットには 0.0f を入れる。
// =============================================================================
bool FFxeditSerializer::ParseLine(const char*  line,
                                 const char*& out_key,
                                 f32&         out_v0,
                                 f32&         out_v1,
                                 f32&         out_v2,
                                 f32&         out_v3) noexcept {
    out_key = nullptr;
    out_v0 = out_v1 = out_v2 = out_v3 = 0.0f;
    if (line == nullptr) return false;

    const char* p = SkipWhitespace(line);
    if (*p == '\0' || *p == '#') return false;  // 空行 or コメント
    out_key = p;

    // key 範囲は次の空白まで (ただし scan ループ後にトークン参照は使わない)。
    while (*p != '\0' && *p != ' ' && *p != '\t') ++p;

    // 数値トークンを最大 4 個まで読む。
    f32* slots[4] = { &out_v0, &out_v1, &out_v2, &out_v3 };
    u32 idx = 0;
    while (idx < 4u && *p != '\0' && *p != '\n') {
        p = SkipWhitespace(p);
        if (*p == '\0' || *p == '\n' || *p == '#') break;
        char* endp = nullptr;
        // strtof は const-correct でないため一時的に const_cast。
        // 入力文字列は本関数内では mutate しない (endp も読み取り目的)。
        const f32 v = std::strtof(p, &endp);
        if (endp == p) break;     // 数値ではない (name 行など)
        *slots[idx++] = v;
        p = endp;
    }
    return true;
}

// =============================================================================
// ParseHeaderVersion — 先頭非空行で magic + version を検査
// =============================================================================
u32 FFxeditSerializer::ParseHeaderVersion(const char* text, u32 text_len) noexcept {
    if (text == nullptr || text_len == 0u) return 0u;
    char line[kMaxLineLength + 1];
    usize pos = 0;
    while (ReadOneLine(text, text_len, pos, line, sizeof(line))) {
        const char* p = SkipWhitespace(line);
        if (*p == '\0' || *p == '#') continue;  // 空行 / コメントは飛ばす
        return ParseMagicLine(line);
    }
    return 0u;
}

// =============================================================================
// Save — defs[count] と names[count] を .fxedit テキストへ書き出す
// =============================================================================
TResult<void, FErrorCode> FFxeditSerializer::Save(const wchar_t*            file_path,
                                               const FParticleEmitterDef* defs,
                                               const char* const*        names,
                                               u32                       count) noexcept {
    if (file_path == nullptr || (count > 0u && (defs == nullptr || names == nullptr))) {
        return ACS_ERR(IO, kSub_NullArgs, "FFxeditSerializer::Save: null argument");
    }

    TArray<char> out;
    out.Reserve(static_cast<usize>(count) * kMaxBytesPerEmitter + 128u);

    // ----- header -------------------------------------------------------
    {
        char hdr[64];
        int n = std::snprintf(hdr, sizeof(hdr), "%s %u\n", kMagic, kCurrentVersion);
        if (n < 0 || static_cast<usize>(n) >= sizeof(hdr) ||
            !AppendStr(out, hdr, static_cast<usize>(n))) {
            return ACS_ERR(IO, kSub_BufferOverflow,
                           "FFxeditSerializer::Save: header buffer overflow");
        }
        n = std::snprintf(hdr, sizeof(hdr), "EMITTER count %u\n", count);
        if (n < 0 || static_cast<usize>(n) >= sizeof(hdr) ||
            !AppendStr(out, hdr, static_cast<usize>(n))) {
            return ACS_ERR(IO, kSub_BufferOverflow,
                           "FFxeditSerializer::Save: header count overflow");
        }
    }

    // ----- emitter blocks ----------------------------------------------
    for (u32 i = 0; i < count; ++i) {
        char prefix[16];
        int pn = std::snprintf(prefix, sizeof(prefix), "E%u", i);
        if (pn < 0 || static_cast<usize>(pn) >= sizeof(prefix)) {
            return ACS_ERR(IO, kSub_BufferOverflow,
                           "FFxeditSerializer::Save: prefix overflow");
        }

        const FParticleEmitterDef& d = defs[i];
        const char* nm = (names != nullptr) ? names[i] : nullptr;

        // name は引用符付きで書き出す。
        if (!AppendNameLine(out, prefix, nm)) {
            return ACS_ERR(IO, kSub_BufferOverflow,
                           "FFxeditSerializer::Save: name line overflow");
        }

        // スカラ key 群。
        f32 v;
        v = d.emit_rate_per_sec;
        if (!AppendKeyValueLine(out, prefix, "emit_rate", &v, 1u))
            return ACS_ERR(IO, kSub_BufferOverflow, "FFxeditSerializer::Save: emit_rate");
        v = d.lifetime_sec;
        if (!AppendKeyValueLine(out, prefix, "lifetime_sec", &v, 1u))
            return ACS_ERR(IO, kSub_BufferOverflow, "FFxeditSerializer::Save: lifetime_sec");
        v = d.burst_count;
        if (!AppendKeyValueLine(out, prefix, "burst_count", &v, 1u))
            return ACS_ERR(IO, kSub_BufferOverflow, "FFxeditSerializer::Save: burst_count");
        v = d.speed_min;
        if (!AppendKeyValueLine(out, prefix, "speed_min", &v, 1u))
            return ACS_ERR(IO, kSub_BufferOverflow, "FFxeditSerializer::Save: speed_min");
        v = d.speed_max;
        if (!AppendKeyValueLine(out, prefix, "speed_max", &v, 1u))
            return ACS_ERR(IO, kSub_BufferOverflow, "FFxeditSerializer::Save: speed_max");
        v = d.scale_start;
        if (!AppendKeyValueLine(out, prefix, "scale_start", &v, 1u))
            return ACS_ERR(IO, kSub_BufferOverflow, "FFxeditSerializer::Save: scale_start");
        v = d.scale_end;
        if (!AppendKeyValueLine(out, prefix, "scale_end", &v, 1u))
            return ACS_ERR(IO, kSub_BufferOverflow, "FFxeditSerializer::Save: scale_end");

        // gravity (3 components, z=0 padding for forward compat)。
        {
            f32 g[3] = { d.gravity.x, d.gravity.y, 0.0f };
            if (!AppendKeyValueLine(out, prefix, "gravity", g, 3u))
                return ACS_ERR(IO, kSub_BufferOverflow, "FFxeditSerializer::Save: gravity");
        }
        // color_start / color_end (4 components, a=1 padding)。
        {
            f32 c[4] = { d.color_start.x, d.color_start.y, d.color_start.z, 1.0f };
            if (!AppendKeyValueLine(out, prefix, "color_start", c, 4u))
                return ACS_ERR(IO, kSub_BufferOverflow, "FFxeditSerializer::Save: color_start");
        }
        {
            f32 c[4] = { d.color_end.x, d.color_end.y, d.color_end.z, 1.0f };
            if (!AppendKeyValueLine(out, prefix, "color_end", c, 4u))
                return ACS_ERR(IO, kSub_BufferOverflow, "FFxeditSerializer::Save: color_end");
        }
        // spread_radians: 構造体に未配備のため 0 固定 (前方互換 placeholder)。
        {
            f32 z = 0.0f;
            if (!AppendKeyValueLine(out, prefix, "spread_radians", &z, 1u))
                return ACS_ERR(IO, kSub_BufferOverflow, "FFxeditSerializer::Save: spread_radians");
        }
    }

    // 終端 NUL を書かない (WriteAllText は与えたバイト列をそのまま書く)。
    auto wr = FFileSystem::WriteAllBytes(file_path,
                                        reinterpret_cast<const byte*>(out.Data()),
                                        out.Size());
    if (wr.IsErr()) {
        // FFileSystem 由来の OS error はそのまま subcode を載せ替えて再ラップ。
        ACS_LOG_WARN("FFxeditSerializer::Save WriteAllBytes failed: os=%u", wr.Error().os_error);
        return ACS_ERR_OS(IO, kSub_IOFailure,
                          "FFxeditSerializer::Save: WriteAllBytes failed",
                          wr.Error().os_error);
    }
    return Ok();
}

// =============================================================================
// Load — `.fxedit` テキストを out_defs / out_name_buffer に復元
// =============================================================================
TResult<u32, FErrorCode> FFxeditSerializer::Load(const wchar_t*       file_path,
                                              FParticleEmitterDef*  out_defs,
                                              char*                out_name_buffer,
                                              u32                  name_buffer_capacity,
                                              u32                  max_emitters) noexcept {
    if (file_path == nullptr || out_defs == nullptr ||
        out_name_buffer == nullptr || max_emitters == 0u ||
        name_buffer_capacity == 0u) {
        return ACS_ERR(IO, kSub_NullArgs, "FFxeditSerializer::Load: null argument");
    }
    if (!FFileSystem::Exists(file_path)) {
        return ACS_ERR(IO, kSub_FileNotFound,
                       "FFxeditSerializer::Load: file not found");
    }

    auto rr = FFileSystem::ReadAllText(file_path);
    if (rr.IsErr()) {
        return ACS_ERR_OS(IO, kSub_IOFailure,
                          "FFxeditSerializer::Load: ReadAllText failed",
                          rr.Error().os_error);
    }
    const TArray<char>& text = rr.Value();
    const usize text_len = text.Size() > 0 ? text.Size() - 1u : 0u; // 末尾 NUL を除いた長さ
    const char* text_ptr = text.Size() > 0 ? text.Data() : "";

    // ---- magic + version 検査 -----------------------------------------
    const u32 ver = ParseHeaderVersion(text_ptr,
                                       static_cast<u32>(text_len > 0xFFFFFFFFu ? 0xFFFFFFFFu : text_len));
    if (ver == 0u) {
        return ACS_ERR(IO, kSub_BadMagic,
                       "FFxeditSerializer::Load: missing or invalid ACS_FXEDIT magic");
    }
    if (ver != kCurrentVersion) {
        ACS_LOG_WARN("FFxeditSerializer::Load: unsupported version %u (expected %u)",
                     ver, kCurrentVersion);
        return ACS_ERR(IO, kSub_BadVersion,
                       "FFxeditSerializer::Load: unsupported version");
    }

    // ---- 1 pass parse ---------------------------------------------------
    // 既定 def + 空 name で全 slot を初期化しておき、行ごとに上書きしていく。
    for (u32 i = 0; i < max_emitters; ++i) {
        out_defs[i] = FParticleEmitterDef{};
    }
    // 名前 buffer は全 NUL 化 (使われない領域は読み手が "" として扱える)。
    for (u32 i = 0; i < name_buffer_capacity; ++i) out_name_buffer[i] = '\0';

    // 各 emitter slot に対し、出力名 buffer 上のオフセットを事前計算する。
    // 1 emitter あたり (kMaxEmitterName + 1) バイト確保。容量が足りない場合は
    // 確保可能な数だけサポート (= 残り emitter の name 部分は空文字列に縮退)。
    const u32 per_name = static_cast<u32>(kMaxEmitterName) + 1u;
    const u32 nameable = (name_buffer_capacity / per_name);

    char line[kMaxLineLength + 1];
    usize pos = 0;
    u32 declared_count = 0u;        // EMITTER count N で宣言された個数
    bool saw_emitter_count = false;
    u32 max_seen_index    = 0u;     // 実際にロード済の最大 E<n> + 1
    bool header_consumed  = false;  // 先頭 magic 行をスキップ済みか

    while (ReadOneLine(text_ptr, text_len, pos, line, sizeof(line))) {
        const char* p = SkipWhitespace(line);
        if (*p == '\0' || *p == '#') continue;
        if (!header_consumed) {
            // 既に ParseHeaderVersion で先頭を確認しているので、ここでは
            // 先頭 magic 行が出てきたら消化するだけ。
            if (std::strncmp(p, kMagic, 10) == 0) {  // "ACS_FXEDIT" = 10 文字
                header_consumed = true;
                continue;
            }
            // magic 行が前置されていないが ParseHeaderVersion は成功している
            // ケース (= 余計な空白を含むが ParseHeaderVersion が許容したケース)
            // を考慮し、header_consumed = true で continue せずに通常処理に進む。
            header_consumed = true;
        }

        // ---- "EMITTER count N" 行 ---------------------------------
        if (std::strncmp(p, "EMITTER", 7) == 0 &&
            (p[7] == ' ' || p[7] == '\t')) {
            const char* q = SkipWhitespace(p + 7);
            if (std::strncmp(q, "count", 5) == 0 && (q[5] == ' ' || q[5] == '\t')) {
                q = SkipWhitespace(q + 5);
                char* endp = nullptr;
                long c = std::strtol(q, &endp, 10);
                if (endp != q && c >= 0) {
                    declared_count    = static_cast<u32>(c);
                    saw_emitter_count = true;
                }
            }
            continue;
        }

        // ---- "E<idx> <key> ..." 行 ---------------------------------
        // key は最初のトークン (= "E<idx>")。
        const char* tok_begin = p;
        const char* tok_end   = p;
        while (*tok_end != '\0' && *tok_end != ' ' && *tok_end != '\t') ++tok_end;
        u32 idx = 0u;
        if (!ParseEmitterIndex(tok_begin, tok_end, idx)) continue;
        if (idx >= max_emitters) {
            // 受け入れ容量超過は subcode を返して中断。
            return ACS_ERR(IO, kSub_TooManyEmitters,
                           "FFxeditSerializer::Load: emitter index exceeds max_emitters");
        }

        // 次のトークンが key2 (e.g. "emit_rate" / "name" / "gravity" ...)。
        const char* k2_begin = SkipWhitespace(tok_end);
        if (*k2_begin == '\0') continue;
        const char* k2_end = k2_begin;
        while (*k2_end != '\0' && *k2_end != ' ' && *k2_end != '\t') ++k2_end;

        FParticleEmitterDef& d = out_defs[idx];

        // ---- name 行 (引用符付き文字列) ---------------------------
        if (KeyEquals(k2_begin, k2_end, "name")) {
            if (idx < nameable) {
                char* dst = out_name_buffer + static_cast<usize>(idx) *
                            static_cast<usize>(per_name);
                (void)ExtractQuotedName(k2_end, dst, per_name);
            }
            if (idx + 1u > max_seen_index) max_seen_index = idx + 1u;
            continue;
        }

        // ---- 残りは数値行: 値部分を改めて strtof で読む -----------
        f32 vals[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        const char* vp = k2_end;
        u32 nv = 0;
        while (nv < 4u) {
            vp = SkipWhitespace(vp);
            if (*vp == '\0' || *vp == '#') break;
            char* endp = nullptr;
            f32 v = std::strtof(vp, &endp);
            if (endp == vp) break;
            vals[nv++] = v;
            vp = endp;
        }
        if (nv == 0u) continue;  // 値の無い未知 key は無視

        if      (KeyEquals(k2_begin, k2_end, "emit_rate"))      d.emit_rate_per_sec = vals[0];
        else if (KeyEquals(k2_begin, k2_end, "lifetime_sec"))   d.lifetime_sec      = vals[0];
        else if (KeyEquals(k2_begin, k2_end, "burst_count"))    d.burst_count       = vals[0];
        else if (KeyEquals(k2_begin, k2_end, "speed_min"))      d.speed_min         = vals[0];
        else if (KeyEquals(k2_begin, k2_end, "speed_max"))      d.speed_max         = vals[0];
        else if (KeyEquals(k2_begin, k2_end, "scale_start"))    d.scale_start       = vals[0];
        else if (KeyEquals(k2_begin, k2_end, "scale_end"))      d.scale_end         = vals[0];
        else if (KeyEquals(k2_begin, k2_end, "gravity")) {
            // テキスト形式は 3 成分まで読むが、FVec2 のため z は破棄。
            d.gravity = FVec2{ vals[0], vals[1] };
        }
        else if (KeyEquals(k2_begin, k2_end, "color_start")) {
            // alpha (vals[3]) は破棄。
            d.color_start = FVec3{ vals[0], vals[1], vals[2] };
        }
        else if (KeyEquals(k2_begin, k2_end, "color_end")) {
            d.color_end   = FVec3{ vals[0], vals[1], vals[2] };
        }
        // "spread_radians" 及び未知 key は無視 (前方互換)。

        if (idx + 1u > max_seen_index) max_seen_index = idx + 1u;
    }

    // declared_count と実観測 index の整合性チェック。
    // declared_count が信用できる (saw_emitter_count) なら優先、
    // それ以外は max_seen_index を返す。
    u32 result_count = saw_emitter_count ? declared_count : max_seen_index;
    if (result_count > max_emitters) {
        ACS_LOG_WARN("FFxeditSerializer::Load: declared count %u clamped to max %u",
                     result_count, max_emitters);
        result_count = max_emitters;
    }
    return TResult<u32, FErrorCode>(OkInit, static_cast<u32&&>(result_count));
}

} // namespace acs::game::fxedit
