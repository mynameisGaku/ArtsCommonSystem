// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Container — FJson 実装 (recursive-descent パーサ)
// =============================================================================
#include "container/Json.h"
#include "foundation/Error.h"

#include <cstdlib>   // _strtod_l
#include <cstdio>    // snprintf (エラーメッセージ整形)
#include <clocale>   // _create_locale (ロケール非依存の数値変換)

namespace acs {

namespace {
// パースエラーメッセージは thread_local バッファに置く (ParseJson の戻り FErrorCode
// が指すため、Parser ローカルだと dangling する — review 指摘)。寿命は「同スレッドで
// 次に ParseJson を呼ぶまで」。codebase の reason ポインタ規約と同じ。
thread_local char g_JsonErrBuf[160] = {0};

// ロケール非依存の数値変換用 C ロケール (小数点は常に '.')。strtod は LC_NUMERIC に
// 依存し、',' 小数点ロケールでは JSON の "1.5" を誤読するため _strtod_l を使う。
_locale_t JsonCLocale() noexcept {
    static const _locale_t s_loc = ::_create_locale(LC_ALL, "C");
    return s_loc;
}
} // namespace

// 共有の静的 Null 値 (At/Get が miss したとき返す。const なので安全)。
static const FJsonValue& NullValue() noexcept {
    static const FJsonValue s_null;
    return s_null;
}

// ---- FJsonValue メソッド ---------------------------------------------------

// 特殊メンバは complete-type の本 TU で定義 (再帰所有のため)。
FJsonValue::FJsonValue() noexcept = default;
FJsonValue::~FJsonValue() noexcept = default;
FJsonValue::FJsonValue(FJsonValue&&) noexcept = default;
FJsonValue& FJsonValue::operator=(FJsonValue&&) noexcept = default;

void FJsonValue::Reset(EJsonType t) noexcept {
    m_Type = t;
    m_Bool = false;
    m_Number = 0.0;
    m_String.Clear();
    m_Elems.Clear();
    m_Keys.Clear();
}

i64 FJsonValue::AsInt(i64 def) const noexcept {
    if (m_Type != EJsonType::Number) return def;
    const f64 n = m_Number;
    if (!(n == n)) return def;                          // NaN
    if (n >=  9.2233720368547758e18) return  9223372036854775807LL;        // > I64_MAX → clamp
    if (n <= -9.2233720368547758e18) return -9223372036854775807LL - 1;    // < I64_MIN → clamp
    return static_cast<i64>(n);
}

u32 FJsonValue::AsU32(u32 def) const noexcept {
    if (m_Type != EJsonType::Number) return def;
    const f64 n = m_Number;
    if (!(n == n)) return def;                          // NaN (cast UB 回避)
    if (n <= 0.0) return 0;
    if (n >= 4294967295.0) return 0xFFFFFFFFu;
    return static_cast<u32>(n);
}

u32 FJsonValue::Size() const noexcept { return static_cast<u32>(m_Elems.Size()); }

const FJsonValue& FJsonValue::At(u32 i) const noexcept {
    return (i < m_Elems.Size()) ? m_Elems[i] : NullValue();
}

const FJsonValue* FJsonValue::Find(const char* key) const noexcept {
    if (m_Type != EJsonType::Object || key == nullptr) return nullptr;
    const FStringView want(key);
    for (usize i = 0; i < m_Keys.Size(); ++i) {
        if (m_Keys[i].View() == want) return &m_Elems[i];
    }
    return nullptr;
}

const FJsonValue& FJsonValue::Get(const char* key) const noexcept {
    const FJsonValue* v = Find(key);
    return v ? *v : NullValue();
}

u32 FJsonValue::MemberCount() const noexcept {
    return m_Type == EJsonType::Object ? static_cast<u32>(m_Elems.Size()) : 0;
}

FStringView FJsonValue::MemberKey(u32 i) const noexcept {
    if (m_Type != EJsonType::Object || i >= m_Keys.Size()) return FStringView{};
    return m_Keys[i].View();
}

FJsonValue& FJsonValue::_PushArrayElem() noexcept {
    m_Elems.PushBack(FJsonValue{});
    return m_Elems[m_Elems.Size() - 1];
}

FJsonValue& FJsonValue::_AddMember(FStringView key) noexcept {
    FString k;
    k.Append(key);
    m_Keys.PushBack(Move(k));
    m_Elems.PushBack(FJsonValue{});
    return m_Elems[m_Elems.Size() - 1];
}

// ---- パーサ ----------------------------------------------------------------

namespace {

constexpr u32 kMaxDepth = 256;   // nesting 上限 (stack overflow / DoS 防御)

struct Parser {
    const char* p;
    const char* end;
    u32 line = 1;
    u32 col  = 1;
    u16 err_sub = 0;

    explicit Parser(const char* text, usize len) noexcept
        : p(text), end(text + len) {}

    bool AtEnd() const noexcept { return p >= end; }
    char Peek() const noexcept { return p < end ? *p : '\0'; }

    void Advance() noexcept {
        if (p >= end) return;
        if (*p == '\n') { ++line; col = 1; } else { ++col; }
        ++p;
    }

    void Fail(u16 sub, const char* msg) noexcept {
        if (err_sub != 0) return;   // 最初のエラーを保持
        err_sub = sub;
        // thread_local バッファへ (ParseJson の戻り FErrorCode が指すため、
        // Parser ローカルだと dangling する — review 指摘)。
        std::snprintf(g_JsonErrBuf, sizeof(g_JsonErrBuf),
                      "JSON %s (line %u, col %u)", msg, line, col);
    }

    void SkipWs() noexcept {
        while (p < end) {
            const char c = *p;
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') Advance();
            else break;
        }
    }

    // UTF-8 1 コードポイント追記 (FString へ)。
    static void AppendUtf8(FString& out, u32 cp) noexcept {
        char b[4];
        if (cp <= 0x7F) {
            b[0] = static_cast<char>(cp);
            out.Append(FStringView(b, 1));
        } else if (cp <= 0x7FF) {
            b[0] = static_cast<char>(0xC0 | (cp >> 6));
            b[1] = static_cast<char>(0x80 | (cp & 0x3F));
            out.Append(FStringView(b, 2));
        } else if (cp <= 0xFFFF) {
            b[0] = static_cast<char>(0xE0 | (cp >> 12));
            b[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            b[2] = static_cast<char>(0x80 | (cp & 0x3F));
            out.Append(FStringView(b, 3));
        } else {
            b[0] = static_cast<char>(0xF0 | (cp >> 18));
            b[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            b[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            b[3] = static_cast<char>(0x80 | (cp & 0x3F));
            out.Append(FStringView(b, 4));
        }
    }

    // 16 進 4 桁を読む。失敗で false。
    bool ReadHex4(u32& out) noexcept {
        out = 0;
        for (int i = 0; i < 4; ++i) {
            if (AtEnd()) { Fail(kSubJsonBadEscape, "truncated \\u"); return false; }
            const char c = *p;
            u32 d;
            if (c >= '0' && c <= '9') d = static_cast<u32>(c - '0');
            else if (c >= 'a' && c <= 'f') d = static_cast<u32>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = static_cast<u32>(c - 'A' + 10);
            else { Fail(kSubJsonBadEscape, "bad \\u hex digit"); return false; }
            out = (out << 4) | d;
            Advance();
        }
        return true;
    }

    // 文字列を読む (開き '"' は呼出側が消費済み)。out へ。
    bool ParseString(FString& out) noexcept {
        out.Clear();
        while (true) {
            if (AtEnd()) { Fail(kSubJsonEof, "unterminated string"); return false; }
            const char c = *p;
            if (c == '"') { Advance(); return true; }
            if (c == '\\') {
                Advance();
                if (AtEnd()) { Fail(kSubJsonEof, "unterminated escape"); return false; }
                const char e = *p;
                switch (e) {
                    case '"':  out.Append('"');  Advance(); break;
                    case '\\': out.Append('\\'); Advance(); break;
                    case '/':  out.Append('/');  Advance(); break;
                    case 'b':  out.Append('\b'); Advance(); break;
                    case 'f':  out.Append('\f'); Advance(); break;
                    case 'n':  out.Append('\n'); Advance(); break;
                    case 'r':  out.Append('\r'); Advance(); break;
                    case 't':  out.Append('\t'); Advance(); break;
                    case 'u': {
                        Advance();
                        u32 cp;
                        if (!ReadHex4(cp)) return false;
                        // サロゲートペア (high: D800-DBFF + low: DC00-DFFF)
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (p + 1 < end && p[0] == '\\' && p[1] == 'u') {
                                Advance(); Advance();
                                u32 lo;
                                if (!ReadHex4(lo)) return false;
                                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                } else {
                                    Fail(kSubJsonBadEscape, "bad low surrogate"); return false;
                                }
                            } else {
                                Fail(kSubJsonBadEscape, "lone high surrogate"); return false;
                            }
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            Fail(kSubJsonBadEscape, "lone low surrogate"); return false;
                        }
                        AppendUtf8(out, cp);
                        break;
                    }
                    default: Fail(kSubJsonBadEscape, "bad escape char"); return false;
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                Fail(kSubJsonSyntax, "control char in string"); return false;
            } else {
                out.Append(c);
                Advance();
            }
        }
    }

    // 数値を読む。token 範囲を切り出し strtod。
    bool ParseNumber(f64& out) noexcept {
        const char* start = p;
        if (Peek() == '-') Advance();
        if (Peek() == '0') { Advance(); }
        else if (Peek() >= '1' && Peek() <= '9') { while (Peek() >= '0' && Peek() <= '9') Advance(); }
        else { Fail(kSubJsonBadNumber, "expected digit"); return false; }
        if (Peek() == '.') {
            Advance();
            if (!(Peek() >= '0' && Peek() <= '9')) { Fail(kSubJsonBadNumber, "expected frac digit"); return false; }
            while (Peek() >= '0' && Peek() <= '9') Advance();
        }
        if (Peek() == 'e' || Peek() == 'E') {
            Advance();
            if (Peek() == '+' || Peek() == '-') Advance();
            if (!(Peek() >= '0' && Peek() <= '9')) { Fail(kSubJsonBadNumber, "expected exp digit"); return false; }
            while (Peek() >= '0' && Peek() <= '9') Advance();
        }
        // [start, p) を NUL 終端バッファへ写して strtod。
        const usize n = static_cast<usize>(p - start);
        if (n == 0 || n >= 63) { Fail(kSubJsonBadNumber, "number too long"); return false; }
        char buf[64];
        for (usize i = 0; i < n; ++i) buf[i] = start[i];
        buf[n] = '\0';
        char* term = nullptr;
        // ロケール非依存変換 (',' 小数点ロケールでも "1.5" を正しく読む)。
        // _create_locale 失敗時 JsonCLocale() は NULL を返し得るので、その場合は
        // 通常 strtod へフォールバック (NULL locale を _strtod_l に渡すと UB)。
        const _locale_t loc = JsonCLocale();
        out = loc ? ::_strtod_l(buf, &term, loc) : ::strtod(buf, &term);
        if (term != buf + n) { Fail(kSubJsonBadNumber, "unparsable number"); return false; }
        return true;
    }

    bool ParseValue(FJsonValue& out, u32 depth) noexcept {
        if (depth > kMaxDepth) { Fail(kSubJsonDepth, "nesting too deep"); return false; }
        SkipWs();
        if (AtEnd()) { Fail(kSubJsonEof, "unexpected end of input"); return false; }
        const char c = Peek();
        switch (c) {
            case '{': return ParseObject(out, depth);
            case '[': return ParseArray(out, depth);
            case '"': {
                Advance();
                FString s;
                if (!ParseString(s)) return false;
                out._SetString(s.View());
                return true;
            }
            case 't':
                if (end - p >= 4 && p[1]=='r' && p[2]=='u' && p[3]=='e') { Advance();Advance();Advance();Advance(); out._SetBool(true); return true; }
                Fail(kSubJsonSyntax, "expected 'true'"); return false;
            case 'f':
                if (end - p >= 5 && p[1]=='a'&&p[2]=='l'&&p[3]=='s'&&p[4]=='e') { Advance();Advance();Advance();Advance();Advance(); out._SetBool(false); return true; }
                Fail(kSubJsonSyntax, "expected 'false'"); return false;
            case 'n':
                if (end - p >= 4 && p[1]=='u'&&p[2]=='l'&&p[3]=='l') { Advance();Advance();Advance();Advance(); out._SetNull(); return true; }
                Fail(kSubJsonSyntax, "expected 'null'"); return false;
            default:
                if (c == '-' || (c >= '0' && c <= '9')) {
                    f64 num;
                    if (!ParseNumber(num)) return false;
                    out._SetNumber(num);
                    return true;
                }
                Fail(kSubJsonSyntax, "unexpected character");
                return false;
        }
    }

    bool ParseArray(FJsonValue& out, u32 depth) noexcept {
        Advance();   // '['
        out._MakeArray();
        SkipWs();
        if (Peek() == ']') { Advance(); return true; }
        while (true) {
            FJsonValue& elem = out._PushArrayElem();
            if (!ParseValue(elem, depth + 1)) return false;
            SkipWs();
            const char c = Peek();
            if (c == ',') { Advance(); SkipWs(); continue; }
            if (c == ']') { Advance(); return true; }
            Fail(kSubJsonSyntax, "expected ',' or ']'");
            return false;
        }
    }

    bool ParseObject(FJsonValue& out, u32 depth) noexcept {
        Advance();   // '{'
        out._MakeObject();
        SkipWs();
        if (Peek() == '}') { Advance(); return true; }
        while (true) {
            SkipWs();
            if (Peek() != '"') { Fail(kSubJsonSyntax, "expected string key"); return false; }
            Advance();
            FString key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (Peek() != ':') { Fail(kSubJsonSyntax, "expected ':'"); return false; }
            Advance();
            FJsonValue& val = out._AddMember(key.View());
            if (!ParseValue(val, depth + 1)) return false;
            SkipWs();
            const char c = Peek();
            if (c == ',') { Advance(); continue; }
            if (c == '}') { Advance(); return true; }
            Fail(kSubJsonSyntax, "expected ',' or '}'");
            return false;
        }
    }
};

} // namespace

TResult<FJsonValue> ParseJson(const char* text, usize len) noexcept {
    if (text == nullptr || len == 0) {
        return ACS_ERR(Generic, kSubJsonEof, "JSON: empty input");
    }
    Parser ps(text, len);
    FJsonValue root;
    if (!ps.ParseValue(root, 0)) {
        return ACS_ERR(Generic, ps.err_sub != 0 ? ps.err_sub : kSubJsonSyntax,
                       g_JsonErrBuf[0] ? g_JsonErrBuf : "JSON parse error");
    }
    ps.SkipWs();
    if (!ps.AtEnd()) {
        return ACS_ERR(Generic, kSubJsonTrailing, "JSON: trailing content after root value");
    }
    return TResult<FJsonValue>(OkInit, Move(root));
}

} // namespace acs
