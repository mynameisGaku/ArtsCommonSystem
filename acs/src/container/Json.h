// SPDX-License-Identifier: Apache-2.0
// ACS Container — FJson (STL 不使用の JSON DOM パーサ)
//
// Aseprite / TexturePacker / Tiled 等が吐く JSON を読むための最小 DOM パーサ。
// 設定ファイル・MOD データ・アセットメタデータにも使える汎用基盤。
//
//   auto r = acs::ParseJson(text, len);
//   if (r.IsOk()) {
//       const FJsonValue& root = r.Value();
//       FStringView name = root.Get("meta").Get("image").AsString();
//       u32 n = root.Get("frames").Size();
//   }
//
// 設計:
//   ・**再帰所有 DOM**: FJsonValue が配列/オブジェクトの子を TArray で所有する
//     (std::vector 流の不完全型メンバ)。move-only。
//   ・**STL 不使用**: 数値は f64、文字列は FString、配列/オブジェクトは TArray。
//   ・**寛容な数値**: int / float / 指数 / 符号 を f64 で受け、AsInt() で切詰める。
//   ・**文字列エスケープ**: \" \\ \/ \b \f \n \r \t \uXXXX (サロゲートペア対応) を
//     UTF-8 にデコードする。
//   ・**防御的**: nesting 深さ上限 (kMaxDepth) で stack overflow / DoS を防ぐ。
//     不正入力は行・列付きエラーで返す (crash しない、全 noexcept)。
//   ・アクセサは「型不一致なら default / 空 / 静的 Null を返す」非例外設計
//     (root.Get("absent").Get("x").AsNumber(0) のような chain が安全)。
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "container/Array.h"
#include "container/String.h"
#include "container/StringView.h"

namespace acs {

/** JSON 値の種別 (Null / Bool / Number / String / Array / Object)。 */
enum class EJsonType : u8 { Null, Bool, Number, String, Array, Object };

class FJsonValue;

/**
 * JSON 値ノード (配列/オブジェクトの子を所有する再帰型、move-only)。
 *
 * @details
 * 数値は f64、文字列は FString、配列要素は m_Elems、オブジェクトの key は m_Keys に
 * 並列で持つ。アクセサは型不一致なら default / 空 / 静的 Null を返す非例外設計のため、
 * 欠損キーを跨ぐ chain アクセスも安全。再帰所有のため特殊メンバは .cpp で定義する。
 */
class FJsonValue {
public:
    /** 空の Null 値を構築する (特殊メンバは再帰所有のため .cpp で定義)。 */
    FJsonValue() noexcept;

    /** 子要素ごと破棄する。 */
    ~FJsonValue() noexcept;

    /**
     * ムーブ構築する (子の所有権を奪う)。
     *
     * @param o ムーブ元。
     */
    FJsonValue(FJsonValue&&) noexcept;

    /**
     * ムーブ代入する (子の所有権を奪う)。
     *
     * @param o ムーブ元。
     * @return *this。
     */
    FJsonValue& operator=(FJsonValue&&) noexcept;

    /** コピー禁止 (子を所有する move-only 型)。 */
    FJsonValue(const FJsonValue&)            = delete;

    /** コピー代入も禁止。 */
    FJsonValue& operator=(const FJsonValue&) = delete;

    /**
     * 値の種別を返す。
     *
     * @return EJsonType。
     */
    EJsonType Type()     const noexcept { return m_Type; }

    /**
     * Null かどうかを返す。
     *
     * @return Null なら true。
     */
    bool      IsNull()   const noexcept { return m_Type == EJsonType::Null; }

    /**
     * Bool かどうかを返す。
     *
     * @return Bool なら true。
     */
    bool      IsBool()   const noexcept { return m_Type == EJsonType::Bool; }

    /**
     * Number かどうかを返す。
     *
     * @return Number なら true。
     */
    bool      IsNumber() const noexcept { return m_Type == EJsonType::Number; }

    /**
     * String かどうかを返す。
     *
     * @return String なら true。
     */
    bool      IsString() const noexcept { return m_Type == EJsonType::String; }

    /**
     * Array かどうかを返す。
     *
     * @return Array なら true。
     */
    bool      IsArray()  const noexcept { return m_Type == EJsonType::Array; }

    /**
     * Object かどうかを返す。
     *
     * @return Object なら true。
     */
    bool      IsObject() const noexcept { return m_Type == EJsonType::Object; }

    /**
     * Bool 値を取り出す (型不一致なら default)。
     *
     * @param def 型不一致時の既定値。
     * @return Bool 値または def。
     */
    bool        AsBool(bool def = false)   const noexcept { return m_Type == EJsonType::Bool   ? m_Bool   : def; }

    /**
     * 数値を f64 で取り出す (型不一致なら default)。
     *
     * @param def 型不一致時の既定値。
     * @return f64 値または def。
     */
    f64         AsNumber(f64 def = 0.0)    const noexcept { return m_Type == EJsonType::Number ? m_Number : def; }

    /**
     * 数値を i64 で取り出す (型不一致なら default、範囲外は clamp)。
     *
     * @details NaN は def を返し、I64 範囲外は端値に clamp する (cast UB 回避)。
     * @param def 型不一致時の既定値。
     * @return i64 値または def。
     */
    i64         AsInt(i64 def = 0)         const noexcept;

    /**
     * 数値を u32 で取り出す (型不一致なら default、範囲外は clamp)。
     *
     * @details NaN は def、負値は 0、U32 上限超過は 0xFFFFFFFF に clamp する (cast UB 回避)。
     * @param def 型不一致時の既定値。
     * @return u32 値または def。
     */
    u32         AsU32(u32 def = 0)         const noexcept;

    /**
     * 数値を f32 で取り出す (型不一致なら default)。
     *
     * @param def 型不一致時の既定値。
     * @return f32 値または def。
     */
    f32         AsF32(f32 def = 0.0f)      const noexcept { return static_cast<f32>(AsNumber(static_cast<f64>(def))); }

    /**
     * 文字列ビューを取り出す (型不一致なら空 view)。
     *
     * @return String の内容を指すビュー、または空 view。
     */
    FStringView AsString()                 const noexcept { return m_Type == EJsonType::String ? m_String.View() : FStringView{}; }

    /**
     * 配列/オブジェクトの要素数を返す。
     *
     * @return 子要素数 (スカラなら 0)。
     */
    u32 Size() const noexcept;

    /**
     * 配列要素を index で取得する。
     *
     * @details 範囲外 / 非配列なら静的 Null 値を返す。
     * @param i 要素インデックス。
     * @return i 番目の要素、または静的 Null 値。
     */
    const FJsonValue& At(u32 i) const noexcept;

    /**
     * オブジェクトメンバを key で検索する。
     *
     * @param key 探すキー (NUL 終端文字列)。
     * @return 見つかれば値ポインタ、存在しない / 非オブジェクトなら nullptr。
     */
    const FJsonValue* Find(const char* key) const noexcept;

    /**
     * オブジェクトメンバを key で取得する (chain アクセス用)。
     *
     * @details 存在しない / 非オブジェクトなら静的 Null 値を返すので chain が安全。
     * @param key 探すキー (NUL 終端文字列)。
     * @return 対応する値、または静的 Null 値。
     */
    const FJsonValue& Get(const char* key) const noexcept;

    /**
     * オブジェクトに key が存在するかを返す。
     *
     * @param key 判定するキー (NUL 終端文字列)。
     * @return 存在すれば true。
     */
    bool Has(const char* key) const noexcept { return Find(key) != nullptr; }

    /**
     * オブジェクトのメンバ数を返す。
     *
     * @return Object なら Size() と同じ、非 Object なら 0。
     */
    u32 MemberCount() const noexcept;

    /**
     * i 番目のメンバの key を返す (value は At(i) で取る)。
     *
     * @param i メンバインデックス。
     * @return key のビュー、非 Object / 範囲外なら空 view。
     */
    FStringView MemberKey(u32 i) const noexcept;

    /** この値を Null にリセットする (パーサ/テスト用ビルダ)。 */
    void _SetNull()              noexcept { Reset(EJsonType::Null); }

    /**
     * この値を Bool に設定する (パーサ/テスト用ビルダ)。
     *
     * @param b 設定する真偽値。
     */
    void _SetBool(bool b)        noexcept { Reset(EJsonType::Bool);   m_Bool = b; }

    /**
     * この値を Number に設定する (パーサ/テスト用ビルダ)。
     *
     * @param n 設定する数値。
     */
    void _SetNumber(f64 n)       noexcept { Reset(EJsonType::Number); m_Number = n; }

    /**
     * この値を String に設定する (パーサ/テスト用ビルダ)。
     *
     * @param s 設定する文字列ビュー (内容をコピーする)。
     */
    void _SetString(FStringView s) noexcept { Reset(EJsonType::String); m_String.Clear(); m_String.Append(s); }

    /** この値を空の Array にする (パーサ/テスト用ビルダ)。 */
    void _MakeArray()            noexcept { Reset(EJsonType::Array); }

    /** この値を空の Object にする (パーサ/テスト用ビルダ)。 */
    void _MakeObject()           noexcept { Reset(EJsonType::Object); }

    /**
     * Array に空要素を追加し、その参照を返す (パーサ/テスト用ビルダ)。
     *
     * @return 追加した空要素への参照。
     */
    FJsonValue& _PushArrayElem() noexcept;

    /**
     * Object に key を追加し、対応する value への参照を返す (パーサ/テスト用ビルダ)。
     *
     * @param key 追加するメンバ key。
     * @return 追加した value への参照。
     */
    FJsonValue& _AddMember(FStringView key) noexcept;

private:
    /**
     * 値を種別 t にリセットしてスカラ・子をクリアする。
     *
     * @param t リセット後の種別。
     */
    void Reset(EJsonType t) noexcept;

    /** 値の種別。 */
    EJsonType m_Type   = EJsonType::Null;

    /** Bool のときの真偽値。 */
    bool      m_Bool   = false;

    /** Number のときの数値。 */
    f64       m_Number = 0.0;

    /** String のときの文字列。 */
    FString   m_String;

    /** Array/Object の子要素 (Object では m_Keys と並列)。 */
    TArray<FJsonValue> m_Elems;

    /** Object の key 配列 (m_Elems と並列)。 */
    TArray<FString>    m_Keys;
};

/**
 * JSON テキストをパースして DOM を返す。
 *
 * @param text 入力 JSON テキスト。
 * @param len text のバイト長。
 * @return 成功なら root 値、失敗なら line/col 付きエラー。
 */
TResult<FJsonValue> ParseJson(const char* text, usize len) noexcept;

/**
 * JSON テキスト (ビュー) をパースして DOM を返す。
 *
 * @param s 入力 JSON テキストのビュー。
 * @return 成功なら root 値、失敗なら line/col 付きエラー。
 */
inline TResult<FJsonValue> ParseJson(FStringView s) noexcept { return ParseJson(s.Data(), s.Size()); }

/** JSON 構文エラー (予期しないトークン) の error subcode。 */
inline constexpr u16 kSubJsonSyntax    = 1400;

/** nesting が深すぎる場合の error subcode。 */
inline constexpr u16 kSubJsonDepth     = 1401;

/** ルート値の後にゴミが続く場合の error subcode。 */
inline constexpr u16 kSubJsonTrailing  = 1402;

/** 途中で入力が終端した場合の error subcode。 */
inline constexpr u16 kSubJsonEof       = 1403;

/** 数値として解釈できない場合の error subcode。 */
inline constexpr u16 kSubJsonBadNumber = 1404;

/** 不正なエスケープ / \u シーケンスの error subcode。 */
inline constexpr u16 kSubJsonBadEscape = 1405;

} // namespace acs
