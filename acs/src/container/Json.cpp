// SPDX-License-Identifier: Apache-2.0
// ACS Container — FJson 実装 (recursive-descent パーサ)
#include "container/Json.h"
#include "foundation/Error.h"
#include "memory/SystemAllocator.h"

#include <cmath>

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
struct FJsonLocaleState {
    FJsonLocaleState() noexcept : locale(::_create_locale(LC_ALL, "C"))
    {
    }
    ~FJsonLocaleState() noexcept
    {
        if (locale != nullptr) ::_free_locale(locale);
    }

    _locale_t locale = nullptr;
};

_locale_t JsonCLocale() noexcept {
    // _create_locale の所有権も process static のデストラクタで対称に戻す。
    static const FJsonLocaleState s_state;
    return s_state.locale;
}
} // namespace

/**
 * At/Get が miss したとき返す共有の静的 Null 値を返す。
 *
 * @return const な静的 Null 値への参照 (chain アクセス用)。
 */
static const FJsonValue& NullValue() noexcept {
    struct FNullState {
        FNullState() noexcept : value(allocator)
        {
        }

        // value を先に破棄してから allocator を破棄する宣言順にする。
        CSystemAllocator allocator;
        FJsonValue value;
    };
    static const FNullState s_null;
    return s_null.value;
}

// 特殊メンバは complete-type の本 TU で定義 (再帰所有のため)。

/** 空の Null 値を構築する。 */
FJsonValue::FJsonValue() noexcept = default;

/** 指定 allocator を文字列と子配列へ固定して空の Null 値を構築する。 */
FJsonValue::FJsonValue(IAllocator& allocator) noexcept : m_String(allocator), m_Elems(allocator), m_Keys(allocator)
{
}

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
    m_Elems.Reset();
    m_Keys.Reset();
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
u32 FJsonValue::Size() const noexcept { return static_cast<u32>(m_Elems.Num()); }

/**
 * 配列要素を index で取得する (範囲外は静的 Null 値)。
 *
 * @param i 要素インデックス。
 * @return i 番目の要素、または静的 Null 値。
 */
const FJsonValue& FJsonValue::At(u32 i) const noexcept {
    return (i < m_Elems.Num()) ? m_Elems[i] : NullValue();
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
    for (usize i = 0; i < m_Keys.Num(); ++i) {
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
    return m_Type == EJsonType::Object ? static_cast<u32>(m_Elems.Num()) : 0;
}

/**
 * i 番目メンバの key を返す (非 Object / 範囲外は空 view)。
 *
 * @param i メンバインデックス。
 * @return key のビュー、または空 view。
 */
FStringView FJsonValue::MemberKey(u32 i) const noexcept {
    if (m_Type != EJsonType::Object || i >= m_Keys.Num()) return FStringView{};
    return m_Keys[i].View();
}

/**
 * Array に空要素を追加してその参照を返す。
 *
 * @return 追加した空要素への参照。
 */
FJsonValue& FJsonValue::_PushArrayElem() noexcept {
    m_Elems.Add(FJsonValue{*m_String.GetAllocator()});
    return m_Elems[m_Elems.Num() - 1];
}

/**
 * Object に key を追加し、対応する value への参照を返す。
 *
 * @param key 追加するメンバ key (内容をコピーする)。
 * @return 追加した value への参照。
 */
FJsonValue& FJsonValue::_AddMember(FStringView key) noexcept {
    // 値と同じ allocator で所有キーを作る。
    FString k(*m_String.GetAllocator());
    k.Append(key);
    m_Keys.Add(Move(k));
    m_Elems.Add(FJsonValue{*m_String.GetAllocator()});
    return m_Elems[m_Elems.Num() - 1];
}

/**
 * Object に所有済み key を追加し、対応する value への参照を返す。
 *
 * @param key 所有権を移すメンバ key。
 * @return 追加した value への参照。
 */
FJsonValue& FJsonValue::_AddMember(FString&& key) noexcept {
    m_Keys.Add(Move(key));
    m_Elems.Add(FJsonValue{*m_String.GetAllocator()});
    return m_Elems[m_Elems.Num() - 1];
}

namespace {

/** nesting の上限 (stack overflow / DoS 防御)。 */
constexpr u32 kMaxDepth = 256;

/** JSON の 1 byte 分類フラグ。 */
enum EJsonCharClass : u8 {
    /** JSON 空白文字。 */
    kJsonWhitespace = 1u << 0u,
    /** ASCII 数字。 */
    kJsonDigit = 1u << 1u,
    /** JSON 文字列では直接記述できない制御文字。 */
    kJsonControl = 1u << 2u,
    /** 文字列終端の二重引用符。 */
    kJsonQuote = 1u << 3u,
    /** escape 開始の逆斜線。 */
    kJsonEscape = 1u << 4u
};

/**
 * 1 byte の JSON 構文上の性質を分類する。
 *
 * @param Value 0 から 255 までの byte 値。
 * @return EJsonCharClass の組み合わせ。
 */
constexpr u8 ClassifyJsonByte(usize Value) noexcept
{
    // 条件に一致した分類フラグを蓄積する。
    u8 Result = 0u;
    if (Value == 0x09u || Value == 0x0Au || Value == 0x0Du || Value == 0x20u) {
        Result |= kJsonWhitespace;
    }
    if (Value >= static_cast<usize>('0') && Value <= static_cast<usize>('9')) {
        Result |= kJsonDigit;
    }
    if (Value < 0x20u) Result |= kJsonControl;
    if (Value == static_cast<usize>('"')) Result |= kJsonQuote;
    if (Value == static_cast<usize>('\\')) Result |= kJsonEscape;
    return Result;
}

/**
 * ASCII byte を 16 進 1 桁へ変換する。
 *
 * @param Value 判定する byte 値。
 * @return 0 から 15 の値。不正文字なら -1。
 */
constexpr i8 JsonHexNibble(usize Value) noexcept
{
    if (Value >= static_cast<usize>('0') && Value <= static_cast<usize>('9')) {
        return static_cast<i8>(Value - static_cast<usize>('0'));
    }
    if (Value >= static_cast<usize>('a') && Value <= static_cast<usize>('f')) {
        return static_cast<i8>(Value - static_cast<usize>('a') + 10u);
    }
    if (Value >= static_cast<usize>('A') && Value <= static_cast<usize>('F')) {
        return static_cast<i8>(Value - static_cast<usize>('A') + 10u);
    }
    return static_cast<i8>(-1);
}

/** constexpr 配列生成用のインデックス列。 */
template<usize... Indices>
struct TJsonIndexSequence {};

/** Count 個の昇順インデックス列を再帰生成する。 */
template<usize Count, usize... Indices>
struct TMakeJsonIndexSequence : TMakeJsonIndexSequence<Count - 1u, Count - 1u, Indices...> {};

/** 再帰終端で完成したインデックス列を公開する。 */
template<usize... Indices>
struct TMakeJsonIndexSequence<0u, Indices...> {
    /** 完成したインデックス列型。 */
    using Type = TJsonIndexSequence<Indices...>;
};

/** インデックス列に対応する JSON 分類表。 */
template<typename Sequence>
struct TJsonCharacterTables;

/**
 * 256 byte の分類表と hex 変換表を constexpr 展開する。
 * parser hot path では範囲比較の分岐列を 1 回の table lookup に置き換える。
 */
template<usize... Indices>
struct TJsonCharacterTables<TJsonIndexSequence<Indices...>> {
    /** byte ごとの構文分類表。 */
    inline static constexpr u8 Classes[sizeof...(Indices)] = {ClassifyJsonByte(Indices)...};

    /** byte ごとの 16 進変換表。 */
    inline static constexpr i8 HexNibbles[sizeof...(Indices)] = {JsonHexNibble(Indices)...};
};

/** 全 256 byte を網羅する JSON 分類表。 */
using FJsonCharacterTables = TJsonCharacterTables<typename TMakeJsonIndexSequence<256u>::Type>;

/**
 * char に対応する JSON 分類フラグを返す。
 *
 * @param Value 判定する byte。
 * @return EJsonCharClass の組み合わせ。
 */
ACS_FORCEINLINE u8 JsonClass(char Value) noexcept
{
    return FJsonCharacterTables::Classes[static_cast<u8>(Value)];
}

/**
 * char が ASCII 数字かを判定する。
 *
 * @param Value 判定する byte。
 * @return 数字なら true。
 */
ACS_FORCEINLINE bool IsJsonDigit(char Value) noexcept
{
    return (JsonClass(Value) & kJsonDigit) != 0u;
}

/**
 * recursive-descent JSON パーサの状態 (カーソル + 行・列 + エラー)。
 *
 * @details 入力範囲 [p, end) を走査し、行・列を追跡しながら値を組み立てる。
 * 最初のエラーだけを err_sub と g_JsonErrBuf に保持する。
 */
struct FParser {
    /** 現在の読み取り位置。 */
    const char* p;

    /** 入力末尾の次を指すポインタ。 */
    const char* end;

    /** DOM と一時文字列の確保に使う allocator。 */
    IAllocator* allocator;

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
     * @param InAllocator DOM と一時文字列に使う allocator。
     */
    explicit FParser(const char* text, usize len, IAllocator& InAllocator) noexcept : p(text), end(text + len), allocator(&InAllocator) {}

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
        std::snprintf(g_JsonErrBuf, sizeof(g_JsonErrBuf), "JSON %s (line %u, col %u)", msg, line, col);
    }

    /** 空白文字 (スペース/タブ/CR/LF) を読み飛ばす。 */
    void SkipWs() noexcept {
        while (p < end && (JsonClass(*p) & kJsonWhitespace) != 0u) {
            Advance();
        }
    }

    /**
     * コードポイントを UTF-8 にエンコードして FString へ追記する。
     *
     * @param out 追記先の文字列。
     * @param cp Unicode コードポイント。
     * @return 追記できたら true、容量不足なら false。
     */
    static bool AppendUtf8(FString& out, u32 cp) noexcept {
        // UTF-8 の最大 4 byte を保持する一時領域。
        char b[4];
        if (cp <= 0x7F) {
            b[0] = static_cast<char>(cp);
            return out.TryAppend(FStringView(b, 1));
        } else if (cp <= 0x7FF) {
            b[0] = static_cast<char>(0xC0 | (cp >> 6));
            b[1] = static_cast<char>(0x80 | (cp & 0x3F));
            return out.TryAppend(FStringView(b, 2));
        } else if (cp <= 0xFFFF) {
            b[0] = static_cast<char>(0xE0 | (cp >> 12));
            b[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            b[2] = static_cast<char>(0x80 | (cp & 0x3F));
            return out.TryAppend(FStringView(b, 3));
        } else {
            b[0] = static_cast<char>(0xF0 | (cp >> 18));
            b[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            b[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            b[3] = static_cast<char>(0x80 | (cp & 0x3F));
            return out.TryAppend(FStringView(b, 4));
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
        // \u の固定 4 桁を順に取り込む。
        for (int i = 0; i < 4; ++i) {
            if (AtEnd()) { Fail(kSubJsonBadEscape, "truncated \\u"); return false; }
            // 現在文字の 16 進値。
            const i8 Nibble = FJsonCharacterTables::HexNibbles[static_cast<u8>(*p)];
            if (Nibble < 0) {
                Fail(kSubJsonBadEscape, "bad \\u hex digit");
                return false;
            }
            out = (out << 4u) | static_cast<u32>(Nibble);
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
            // 通常文字の連続区間は 1 回で追記し、1 byte ごとの確保判定を避ける。
            const char* const runStart = p;
            while (p < end) {
                // 現在 byte の構文分類。
                const u8 cls = JsonClass(*p);
                if ((cls & (kJsonControl | kJsonQuote | kJsonEscape)) != 0u) break;
                ++p;
                ++col;
            }
            if (p != runStart && !out.TryAppend(FStringView(runStart, static_cast<usize>(p - runStart)))) {
                Fail(kSubJsonSize, "string too large");
                return false;
            }
            if (AtEnd()) { Fail(kSubJsonEof, "unterminated string"); return false; }

            // 連続区間の直後にある特殊 byte の分類。
            const u8 cls = JsonClass(*p);
            if ((cls & kJsonQuote) != 0u) {
                Advance();
                return true;
            }
            if ((cls & kJsonControl) != 0u) {
                Fail(kSubJsonSyntax, "control char in string");
                return false;
            }

            Advance();
            if (AtEnd()) { Fail(kSubJsonEof, "unterminated escape"); return false; }
            // 逆斜線に続く escape 識別子。
            const char e = *p;
            // 1 byte escape のデコード結果。
            char decoded = '\0';
            // \u 以外の 1 byte escape なら true。
            bool hasDecodedByte = true;
            switch (e) {
                case '"':  decoded = '"';  break;
                case '\\': decoded = '\\'; break;
                case '/':  decoded = '/';  break;
                case 'b':  decoded = '\b'; break;
                case 'f':  decoded = '\f'; break;
                case 'n':  decoded = '\n'; break;
                case 'r':  decoded = '\r'; break;
                case 't':  decoded = '\t'; break;
                case 'u': {
                    hasDecodedByte = false;
                    Advance();
                    // Unicode コードポイントの上位側。
                    u32 cp;
                    if (!ReadHex4(cp)) return false;
                    // サロゲートペア (high: D800-DBFF + low: DC00-DFFF)
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (p + 1 < end && p[0] == '\\' && p[1] == 'u') {
                            Advance(); Advance();
                            // サロゲートペアの下位側。
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
                    if (!AppendUtf8(out, cp)) {
                        Fail(kSubJsonSize, "string too large");
                        return false;
                    }
                    break;
                }
                default: Fail(kSubJsonBadEscape, "bad escape char"); return false;
            }
            if (hasDecodedByte) {
                Advance();
                if (!out.TryAppend(decoded)) {
                    Fail(kSubJsonSize, "string too large");
                    return false;
                }
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
        // 数値トークンの開始位置。
        const char* const start = p;
        if (Peek() == '-') Advance();
        if (Peek() == '0') { Advance(); }
        else if (Peek() >= '1' && Peek() <= '9') { while (IsJsonDigit(Peek())) Advance(); }
        else { Fail(kSubJsonBadNumber, "expected digit"); return false; }
        if (Peek() == '.') {
            Advance();
            if (!IsJsonDigit(Peek())) { Fail(kSubJsonBadNumber, "expected frac digit"); return false; }
            while (IsJsonDigit(Peek())) Advance();
        }
        if (Peek() == 'e' || Peek() == 'E') {
            Advance();
            if (Peek() == '+' || Peek() == '-') Advance();
            if (!IsJsonDigit(Peek())) { Fail(kSubJsonBadNumber, "expected exp digit"); return false; }
            while (IsJsonDigit(Peek())) Advance();
        }
        // [start, p) を NUL 終端バッファへ写して strtod。
        // 数値トークンの byte 長。
        const usize n = static_cast<usize>(p - start);
        if (n == 0 || n >= 63) { Fail(kSubJsonBadNumber, "number too long"); return false; }
        // strtod 用の NUL 終端一時領域。
        char buf[64];
        for (usize i = 0; i < n; ++i) buf[i] = start[i];
        buf[n] = '\0';
        // strtod が返す変換終端。
        char* term = nullptr;
        // ロケール非依存変換 (',' 小数点ロケールでも "1.5" を正しく読む)。
        // _create_locale 失敗時 JsonCLocale() は NULL を返し得るので、その場合は
        // 通常 strtod へフォールバック (NULL locale を _strtod_l に渡すと UB)。
        // JSON 小数点を固定する C ロケール。
        const _locale_t loc = JsonCLocale();
        out = loc ? ::_strtod_l(buf, &term, loc) : ::strtod(buf, &term);
        if (term != buf + n) {
            Fail(kSubJsonBadNumber, "unparsable number");
            return false;
        }
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
        // 値種別を決める先頭文字。
        const char c = Peek();
        switch (c) {
            case '{': return ParseObject(out, depth);
            case '[': return ParseArray(out, depth);
            case '"': {
                Advance();
                // パース対象と同じ allocator を使う一時文字列。
                FString s(*allocator);
                if (!ParseString(s)) return false;
                out._SetString(Move(s));
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
                if (c == '-' || IsJsonDigit(c)) {
                    // 変換した JSON 数値。
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
            // 末尾へ新規追加した配列要素。
            FJsonValue& elem = out._PushArrayElem();
            if (!ParseValue(elem, depth + 1)) return false;
            SkipWs();
            // 要素後に続く区切り文字。
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
            // パース対象と同じ allocator を使う所有キー。
            FString key(*allocator);
            if (!ParseString(key)) return false;
            SkipWs();
            if (Peek() != ':') { Fail(kSubJsonSyntax, "expected ':'"); return false; }
            Advance();
            // キーに対応して新規追加した値。
            FJsonValue& val = out._AddMember(Move(key));
            if (!ParseValue(val, depth + 1)) return false;
            SkipWs();
            // メンバ後に続く区切り文字。
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
    return ParseJson(text, len, DefaultAllocator());
}

/**
 * 指定 allocator で JSON テキストをパースして DOM を返す。
 *
 * @param text 入力 JSON テキスト。
 * @param len text のバイト長。
 * @param allocator DOM と一時文字列に使う allocator。
 * @return 成功なら root 値、失敗なら line/col 付きエラー。
 */
TResult<FJsonValue> ParseJson(const char* text, usize len, IAllocator& allocator) noexcept {
    if (text == nullptr || len == 0) {
        return ACS_ERR(Generic, kSubJsonEof, "JSON: empty input");
    }
    if (len > kMaxJsonInputBytes) {
        return ACS_ERR(Generic, kSubJsonSize, "JSON: input exceeds size limit");
    }

    // 前回の失敗メッセージを次回の呼び出しへ持ち越さない。
    g_JsonErrBuf[0] = '\0';
    // 入力範囲とエラー位置を追跡するパーサ。
    FParser ps(text, len, allocator);
    // 呼出側 allocator を保持するルート値。
    FJsonValue root(allocator);
    if (!ps.ParseValue(root, 0)) {
        return ACS_ERR(Generic, ps.err_sub != 0 ? ps.err_sub : kSubJsonSyntax, g_JsonErrBuf[0] ? g_JsonErrBuf : "JSON parse error");
    }
    ps.SkipWs();
    if (!ps.AtEnd()) {
        return ACS_ERR(Generic, kSubJsonTrailing, "JSON: trailing content after root value");
    }
    return TResult<FJsonValue>(OkInit, Move(root));
}

namespace {

/**
 * JSON 出力へ byte 列を上限内で追記する。
 *
 * @param Output 追記先。
 * @param Fragment 追記する byte 列。
 * @param MaxBytes 許容する最大出力 byte 数。
 * @return 追記できたら true、上限超過または容量不足なら false。
 */
bool TryAppendJsonFragment(FString& Output, FStringView Fragment, usize MaxBytes) noexcept
{
    if (Output.Size() > MaxBytes || Fragment.Size() > MaxBytes - Output.Size()) {
        return false;
    }
    return Output.TryAppend(Fragment);
}

/**
 * JSON 出力へ 1 byte を上限内で追記する。
 *
 * @param Output 追記先。
 * @param Byte 追記する byte。
 * @param MaxBytes 許容する最大出力 byte 数。
 * @return 追記できたら true、上限超過または容量不足なら false。
 */
bool TryAppendJsonByte(FString& Output, char Byte, usize MaxBytes) noexcept
{
    return Output.Size() < MaxBytes && Output.TryAppend(Byte);
}

/**
 * JSON 文字列を escape しながら出力する。
 *
 * 埋め込み NUL を含む制御文字は \u00XX とし、UTF-8 の非 ASCII byte は
 * 入力 byte 列をそのまま保持する。
 *
 * @param Value 書き出す文字列。
 * @param Output 追記先。
 * @param MaxBytes 許容する最大出力 byte 数。
 * @return 書き出せたら true、上限超過または容量不足なら false。
 */
bool TryWriteJsonString(FStringView Value, FString& Output, usize MaxBytes) noexcept
{
    if (!TryAppendJsonByte(Output, '"', MaxBytes)) return false;

    // 未処理 byte の位置。
    usize Cursor = 0u;
    while (Cursor < Value.Size()) {
        // escape 不要な連続区間の開始位置。
        const usize RunStart = Cursor;
        while (Cursor < Value.Size()) {
            // 現在の符号なし byte 値。
            const u8 Byte = static_cast<u8>(Value[Cursor]);
            if (Byte < 0x20u || Byte == static_cast<u8>('"') || Byte == static_cast<u8>('\\')) {
                break;
            }
            ++Cursor;
        }
        if (Cursor != RunStart && !TryAppendJsonFragment(Output, FStringView(Value.Data() + RunStart, Cursor - RunStart), MaxBytes)) {
            return false;
        }
        if (Cursor == Value.Size()) break;

        // escape 対象の符号なし byte 値。
        const u8 Byte = static_cast<u8>(Value[Cursor++]);
        // 短い escape 表現。該当しない制御文字なら nullptr。
        const char* Escape = nullptr;
        switch (Byte) {
            case '"':  Escape = "\\\""; break;
            case '\\': Escape = "\\\\"; break;
            case '\b': Escape = "\\b";  break;
            case '\f': Escape = "\\f";  break;
            case '\n': Escape = "\\n";  break;
            case '\r': Escape = "\\r";  break;
            case '\t': Escape = "\\t";  break;
            default: break;
        }
        if (Escape != nullptr) {
            if (!TryAppendJsonFragment(Output, FStringView(Escape), MaxBytes)) {
                return false;
            }
            continue;
        }

        // \u00XX の 16 進文字表。
        static constexpr char Hex[] = "0123456789ABCDEF";
        // 制御文字を表す固定長 escape。
        const char Encoded[6] = {'\\', 'u', '0', '0', Hex[(Byte >> 4u) & 0x0Fu], Hex[Byte & 0x0Fu]};
        if (!TryAppendJsonFragment(Output, FStringView(Encoded, sizeof(Encoded)), MaxBytes)) {
            return false;
        }
    }
    return TryAppendJsonByte(Output, '"', MaxBytes);
}

/**
 * JSON DOM の 1 値を再帰的に書き出す。
 *
 * @param Value 書き出す値。
 * @param Output 追記先。
 * @param MaxDepth 許容する最大 nesting 深さ。
 * @param MaxBytes 許容する最大出力 byte 数。
 * @param Depth 現在の nesting 深さ。
 * @return 書き出せたら true、上限超過・非有限数・容量不足なら false。
 */
bool TryWriteJsonValue(const FJsonValue& Value, FString& Output, u32 MaxDepth, usize MaxBytes, u32 Depth) noexcept
{
    if (Depth > MaxDepth || Depth > kMaxDepth) return false;

    switch (Value.Type()) {
        case EJsonType::Null:
            return TryAppendJsonFragment(Output, FStringView("null"), MaxBytes);
        case EJsonType::Bool:
            return TryAppendJsonFragment(Output, Value.AsBool() ? FStringView("true") : FStringView("false"), MaxBytes);
        case EJsonType::Number: {
            // 書き出す倍精度値。
            const f64 Number = Value.AsNumber();
            if (!std::isfinite(Number)) return false;
            // 最大精度の数値文字列を保持する一時領域。
            char Buffer[32];
            // JSON 小数点を固定する C ロケール。
            const _locale_t Locale = JsonCLocale();
            // 終端 NUL を除く整形 byte 数。
            const int Count = Locale ? ::_snprintf_l(Buffer, sizeof(Buffer), "%.17g", Locale, Number) : std::snprintf(Buffer, sizeof(Buffer), "%.17g", Number);
            if (Count <= 0 || static_cast<usize>(Count) >= sizeof(Buffer)) {
                return false;
            }
            return TryAppendJsonFragment(Output, FStringView(Buffer, static_cast<usize>(Count)), MaxBytes);
        }
        case EJsonType::String:
            return TryWriteJsonString(Value.AsString(), Output, MaxBytes);
        case EJsonType::Array: {
            if (!TryAppendJsonByte(Output, '[', MaxBytes)) return false;
            // 配列の現在要素番号。
            for (u32 Index = 0; Index < Value.Size(); ++Index) {
                if (Index != 0u && !TryAppendJsonByte(Output, ',', MaxBytes)) {
                    return false;
                }
                if (!TryWriteJsonValue(Value.At(Index), Output, MaxDepth, MaxBytes, Depth + 1u)) {
                    return false;
                }
            }
            return TryAppendJsonByte(Output, ']', MaxBytes);
        }
        case EJsonType::Object: {
            if (!TryAppendJsonByte(Output, '{', MaxBytes)) return false;
            // オブジェクトの現在メンバ番号。
            for (u32 Index = 0; Index < Value.MemberCount(); ++Index) {
                if (Index != 0u && !TryAppendJsonByte(Output, ',', MaxBytes)) {
                    return false;
                }
                if (!TryWriteJsonString(Value.MemberKey(Index), Output, MaxBytes) || !TryAppendJsonByte(Output, ':', MaxBytes) || !TryWriteJsonValue(Value.At(Index), Output, MaxDepth, MaxBytes, Depth + 1u)) {
                    return false;
                }
            }
            return TryAppendJsonByte(Output, '}', MaxBytes);
        }
    }
    return false;
}

} // namespace

bool TryWriteJson(const FJsonValue& Value, FString& Output, u32 MaxDepth, usize MaxBytes) noexcept
{
    // 失敗時に Output を変えないための staging 文字列。
    FString Staged(*Output.GetAllocator());
    if (!TryWriteJsonValue(Value, Staged, MaxDepth, MaxBytes, 0u)) return false;
    Output = Move(Staged);
    return true;
}

} // namespace acs
