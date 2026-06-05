// SPDX-License-Identifier: Apache-2.0
// ACS Container — FJson 実装 (recursive-descent パーサ)
#include "container/Json.h"
#include "foundation/Error.h"

#include <cstdlib>   // _strtod_l
#include <cstdio>    // snprintf (エラーメッセージ整形)
#include <clocale>   // _create_locale (ロケール非依存の数値変換)

namespace acs {

namespace {
/**
 * パースエラーメッセージ用 thread_local バッファ。
 *
 * @details ParseJson の戻り FErrorCode がこのバッファを指すため、Parser ローカルだと
 * dangling する (review 指摘)。寿命は「同スレッドで次に ParseJson を呼ぶまで」。
 */
thread_local char g_JsonErrBuf[160] = {0};

/**
 * ロケール非依存の数値変換用 C ロケールを返す (小数点は常に '.')。
 *
 * @details strtod は LC_NUMERIC に依存し ',' 小数点ロケールでは JSON の "1.5" を誤読
 * するため _strtod_l に渡す C ロケールを 1 度だけ生成して使い回す。
 * @return C ロケールハンドル (生成失敗時は NULL になり得る)。
 */
_locale_t JsonCLocale() noexcept {
    static const _locale_t s_loc = ::_create_locale(LC_ALL, "C");
    return s_loc;
}
} // namespace

/**
 * At/Get が miss したとき返す共有の静的 Null 値を返す。
 *
 * @return const な静的 Null 値への参照 (chain アクセス用)。
 */
static const FJsonValue& NullValue() noexcept {
    static const FJsonValue s_null;
    return s_null;
}

// 特殊メンバは complete-type の本 TU で定義 (再帰所有のため)。

/** 空の Null 値を構築する。 */
FJsonValue::FJsonValue() noexcept = default;

/** 子要素ごと破棄する。 */
FJsonValue::~FJsonValue() noexcept = default;

/** ムーブ構築する (子の所有権を奪う)。 */
FJsonValue::FJsonValue(FJsonValue&&) noexcept = default;

/** ムーブ代入する (子の所有権を奪う)。 */
FJsonValue& FJsonValue::operator=(FJsonValue&&) noexcept = default;

/**
 * 値を種別 t にリセットしてスカラ・子をすべてクリアする。
 *
 * @param t リセット後の種別。
 */
void FJsonValue::Reset(EJsonType t) noexcept {
    m_Type = t;
    m_Bool = false;
    m_Number = 0.0;
    m_String.Clear();
    m_Elems.Clear();
    m_Keys.Clear();
}

/**
 * 数値を i64 で取り出す (型不一致なら def、NaN は def、範囲外は端値に clamp)。
 *
 * @param def 型不一致時の既定値。
 * @return i64 値または def。
 */
i64 FJsonValue::AsInt(i64 def) const noexcept {
    if (m_Type != EJsonType::Number) return def;
    const f64 n = m_Number;
    if (!(n == n)) return def;                          // NaN
    if (n >=  9.2233720368547758e18) return  9223372036854775807LL;        // > I64_MAX → clamp
    if (n <= -9.2233720368547758e18) return -9223372036854775807LL - 1;    // < I64_MIN → clamp
    return static_cast<i64>(n);
}

/**
 * 数値を u32 で取り出す (型不一致なら def、NaN は def、範囲外は clamp)。
 *
 * @param def 型不一致時の既定値。
 * @return u32 値または def。
 */
u32 FJsonValue::AsU32(u32 def) const noexcept {
    if (m_Type != EJsonType::Number) return def;
    const f64 n = m_Number;
    if (!(n == n)) return def;                          // NaN (cast UB 回避)
    if (n <= 0.0) return 0;
    if (n >= 4294967295.0) return 0xFFFFFFFFu;
    return static_cast<u32>(n);
}

/**
 * 配列/オブジェクトの子要素数を返す。
 *
 * @return m_Elems の要素数。
 */
u32 FJsonValue::Size() const noexcept { return static_cast<u32>(m_Elems.Size()); }

/**
 * 配列要素を index で取得する (範囲外は静的 Null 値)。
 *
 * @param i 要素インデックス。
 * @return i 番目の要素、または静的 Null 値。
 */
const FJsonValue& FJsonValue::At(u32 i) const noexcept {
    return (i < m_Elems.Size()) ? m_Elems[i] : NullValue();
}

/**
 * オブジェクトメンバを key で線形検索する。
 *
 * @param key 探すキー (NUL 終端文字列)。
 * @return 見つかれば値ポインタ、存在しない / 非オブジェクトなら nullptr。
 */
const FJsonValue* FJsonValue::Find(const char* key) const noexcept {
    if (m_Type != EJsonType::Object || key == nullptr) return nullptr;
    const FStringView want(key);
    for (usize i = 0; i < m_Keys.Size(); ++i) {
        if (m_Keys[i].View() == want) return &m_Elems[i];
    }
    return nullptr;
}

/**
 * オブジェクトメンバを key で取得する (miss 時は静的 Null 値で chain 安全)。
 *
 * @param key 探すキー (NUL 終端文字列)。
 * @return 対応する値、または静的 Null 値。
 */
const FJsonValue& FJsonValue::Get(const char* key) const noexcept {
    const FJsonValue* v = Find(key);
    return v ? *v : NullValue();
}

/**
 * オブジェクトのメンバ数を返す (非 Object なら 0)。
 *
 * @return メンバ数。
 */
u32 FJsonValue::MemberCount() const noexcept {
    return m_Type == EJsonType::Object ? static_cast<u32>(m_Elems.Size()) : 0;
}

/**
 * i 番目メンバの key を返す (非 Object / 範囲外は空 view)。
 *
 * @param i メンバインデックス。
 * @return key のビュー、または空 view。
 */
FStringView FJsonValue::MemberKey(u32 i) const noexcept {
    if (m_Type != EJsonType::Object || i >= m_Keys.Size()) return FStringView{};
    return m_Keys[i].View();
}

/**
 * Array に空要素を追加してその参照を返す。
 *
 * @return 追加した空要素への参照。
 */
FJsonValue& FJsonValue::_PushArrayElem() noexcept {
    m_Elems.PushBack(FJsonValue{});
    return m_Elems[m_Elems.Size() - 1];
}

/**
 * Object に key を追加し、対応する value への参照を返す。
 *
 * @param key 追加するメンバ key (内容をコピーする)。
 * @return 追加した value への参照。
 */
FJsonValue& FJsonValue::_AddMember(FStringView key) noexcept {
    FString k;
    k.Append(key);
    m_Keys.PushBack(Move(k));
    m_Elems.PushBack(FJsonValue{});
    return m_Elems[m_Elems.Size() - 1];
}

namespace {

/** nesting の上限 (stack overflow / DoS 防御)。 */
constexpr u32 kMaxDepth = 256;

/**
 * recursive-descent JSON パーサの状態 (カーソル + 行・列 + エラー)。
 *
 * @details 入力範囲 [p, end) を走査し、行・列を追跡しながら値を組み立てる。
 * 最初のエラーだけを err_sub と g_JsonErrBuf に保持する。
 */
struct Parser {
    /** 現在の読み取り位置。 */
    const char* p;

    /** 入力末尾の次を指すポインタ。 */
    const char* end;

    /** 現在行 (1 始まり、エラーメッセージ用)。 */
    u32 line = 1;

    /** 現在列 (1 始まり、エラーメッセージ用)。 */
    u32 col  = 1;

    /** 最初に発生したエラーの subcode (0 = エラーなし)。 */
    u16 err_sub = 0;

    /**
     * 入力テキストからパーサを構築する。
     *
     * @param text 入力 JSON テキスト。
     * @param len text のバイト長。
     */
    explicit Parser(const char* text, usize len) noexcept
        : p(text), end(text + len) {}

    /**
     * 入力終端に達したかを返す。
     *
     * @return p が end 以上なら true。
     */
    bool AtEnd() const noexcept { return p >= end; }

    /**
     * 現在文字を覗き見る (終端なら '\0')。
     *
     * @return 現在位置の文字、終端なら '\0'。
     */
    char Peek() const noexcept { return p < end ? *p : '\0'; }

    /** カーソルを 1 文字進める (改行で行・列を更新)。 */
    void Advance() noexcept {
        if (p >= end) return;
        if (*p == '\n') { ++line; col = 1; } else { ++col; }
        ++p;
    }

    /**
     * パースエラーを記録する (最初の 1 件のみ保持)。
     *
     * @details メッセージを thread_local バッファへ行・列付きで整形する。
     * @param sub error subcode。
     * @param msg エラー内容を表す短い文字列。
     */
    void Fail(u16 sub, const char* msg) noexcept {
        if (err_sub != 0) return;   // 最初のエラーを保持
        err_sub = sub;
        // thread_local バッファへ (ParseJson の戻り FErrorCode が指すため、
        // Parser ローカルだと dangling する — review 指摘)。
        std::snprintf(g_JsonErrBuf, sizeof(g_JsonErrBuf),
                      "JSON %s (line %u, col %u)", msg, line, col);
    }

    /** 空白文字 (スペース/タブ/CR/LF) を読み飛ばす。 */
    void SkipWs() noexcept {
        while (p < end) {
            const char c = *p;
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') Advance();
            else break;
        }
    }

    /**
     * コードポイントを UTF-8 にエンコードして FString へ追記する。
     *
     * @param out 追記先の文字列。
     * @param cp Unicode コードポイント。
     */
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

    /**
     * 16 進 4 桁を読み取る (\u エスケープ用)。
     *
     * @param out 読み取った値の出力先。
     * @return 4 桁とも読めたら true、途中終端 / 不正桁なら false。
     */
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

    /**
     * 文字列本体を読み取る (開き '"' は呼出側が消費済み)。
     *
     * @details エスケープ \" \\ \/ \b \f \n \r \t \uXXXX (サロゲートペア対応) をデコード
     * する。制御文字や未終端はエラー。
     * @param out デコード結果の出力先。
     * @return 閉じ '"' まで読めたら true、エラーなら false。
     */
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

    /**
     * 数値トークンを読み取って f64 に変換する。
     *
     * @details token 範囲を切り出し NUL 終端バッファへ写してロケール非依存の strtod で
     * 変換する。形式不正や 63 文字超過はエラー。
     * @param out 変換結果の出力先。
     * @return 変換できたら true、エラーなら false。
     */
    bool ParseNumber(f64& out) noexcept {
        const char* const start = p;
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

    /**
     * 任意の JSON 値を 1 つパースする (型を先読みして分岐)。
     *
     * @details depth で nesting を追跡し kMaxDepth 超過でエラー。object/array/string/
     * true/false/null/number を判別して対応するパーサへ振り分ける。
     * @param out パース結果の出力先。
     * @param depth 現在の nesting 深さ。
     * @return 成功なら true、エラーなら false。
     */
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

    /**
     * 配列 '[ ... ]' をパースする (開き '[' は呼出側で確認済み)。
     *
     * @param out 結果を格納する Array 値。
     * @param depth 現在の nesting 深さ。
     * @return 成功なら true、エラーなら false。
     */
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

    /**
     * オブジェクト '{ ... }' をパースする (開き '{' は呼出側で確認済み)。
     *
     * @param out 結果を格納する Object 値。
     * @param depth 現在の nesting 深さ。
     * @return 成功なら true、エラーなら false。
     */
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

/**
 * JSON テキストをパースして DOM を返す。
 *
 * @details 空入力・パース失敗・ルート値後のゴミをそれぞれ subcode 付きエラーで返す。
 * 成功時は root 値をムーブして返す。
 * @param text 入力 JSON テキスト。
 * @param len text のバイト長。
 * @return 成功なら root 値、失敗なら line/col 付きエラー。
 */
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
