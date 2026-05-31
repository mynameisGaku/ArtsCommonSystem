// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Container — FJson (STL 不使用の JSON DOM パーサ)
// -----------------------------------------------------------------------------
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

enum class EJsonType : u8 { Null, Bool, Number, String, Array, Object };

class FJsonValue;

// JSON 値ノード。配列/オブジェクトの子を所有する再帰型。move-only。
class FJsonValue {
public:
    // 再帰所有型 (TArray<FJsonValue> メンバ) のため、特殊メンバは .cpp で定義する
    // (= default を宣言時に置くと不完全型でインスタンス化されうるため out-of-line)。
    FJsonValue() noexcept;
    ~FJsonValue() noexcept;
    FJsonValue(FJsonValue&&) noexcept;
    FJsonValue& operator=(FJsonValue&&) noexcept;
    FJsonValue(const FJsonValue&)            = delete;
    FJsonValue& operator=(const FJsonValue&) = delete;

    EJsonType Type()     const noexcept { return m_Type; }
    bool      IsNull()   const noexcept { return m_Type == EJsonType::Null; }
    bool      IsBool()   const noexcept { return m_Type == EJsonType::Bool; }
    bool      IsNumber() const noexcept { return m_Type == EJsonType::Number; }
    bool      IsString() const noexcept { return m_Type == EJsonType::String; }
    bool      IsArray()  const noexcept { return m_Type == EJsonType::Array; }
    bool      IsObject() const noexcept { return m_Type == EJsonType::Object; }

    // スカラ取り出し (型不一致なら default を返す)。
    bool        AsBool(bool def = false)   const noexcept { return m_Type == EJsonType::Bool   ? m_Bool   : def; }
    f64         AsNumber(f64 def = 0.0)    const noexcept { return m_Type == EJsonType::Number ? m_Number : def; }
    // 非有限 (inf/NaN) / 範囲外は安全に clamp / def を返す (cast UB 回避、review 指摘)。
    i64         AsInt(i64 def = 0)         const noexcept;
    u32         AsU32(u32 def = 0)         const noexcept;
    f32         AsF32(f32 def = 0.0f)      const noexcept { return static_cast<f32>(AsNumber(static_cast<f64>(def))); }
    FStringView AsString()                 const noexcept { return m_Type == EJsonType::String ? m_String.View() : FStringView{}; }

    // 配列 (Array) / オブジェクト (Object) 共通の要素数。
    u32 Size() const noexcept;

    // 配列要素を index で取得。範囲外 / 非配列なら静的 Null 値を返す。
    const FJsonValue& At(u32 i) const noexcept;

    // オブジェクトメンバを key で取得。存在しない / 非オブジェクトなら nullptr。
    const FJsonValue* Find(const char* key) const noexcept;
    // key で取得。存在しない場合は静的 Null 値を返す (chain が安全)。
    const FJsonValue& Get(const char* key) const noexcept;
    bool Has(const char* key) const noexcept { return Find(key) != nullptr; }

    // オブジェクトメンバ数 (= Object のとき Size() と同じ、非 Object は 0)。
    u32 MemberCount() const noexcept;
    // i 番目メンバの key。Object 以外 / 範囲外は空 view。value は At(i) で取る。
    FStringView MemberKey(u32 i) const noexcept;

    // ---- パーサ内部用ビルダ (テスト / 構築でも使える) ----
    void _SetNull()              noexcept { Reset(EJsonType::Null); }
    void _SetBool(bool b)        noexcept { Reset(EJsonType::Bool);   m_Bool = b; }
    void _SetNumber(f64 n)       noexcept { Reset(EJsonType::Number); m_Number = n; }
    void _SetString(FStringView s) noexcept { Reset(EJsonType::String); m_String.Clear(); m_String.Append(s); }
    void _MakeArray()            noexcept { Reset(EJsonType::Array); }
    void _MakeObject()           noexcept { Reset(EJsonType::Object); }
    FJsonValue& _PushArrayElem() noexcept;                       // Array に空要素を追加し参照を返す
    FJsonValue& _AddMember(FStringView key) noexcept;            // Object に key を追加し value 参照を返す

private:
    void Reset(EJsonType t) noexcept;

    EJsonType m_Type   = EJsonType::Null;
    bool      m_Bool   = false;
    f64       m_Number = 0.0;
    FString   m_String;
    TArray<FJsonValue> m_Elems;   // Array の要素
    TArray<FString>    m_Keys;    // Object の key (m_Elems と並列)
};

// パース結果。失敗時は line/col 付きのエラー。
TResult<FJsonValue> ParseJson(const char* text, usize len) noexcept;
inline TResult<FJsonValue> ParseJson(FStringView s) noexcept { return ParseJson(s.Data(), s.Size()); }

// Json 用のエラー subcode (ErrCategory::Generic 配下)。
inline constexpr u16 kSubJsonSyntax    = 1400;  // 構文エラー (予期しないトークン)
inline constexpr u16 kSubJsonDepth     = 1401;  // nesting が深すぎる
inline constexpr u16 kSubJsonTrailing  = 1402;  // ルート値の後にゴミ
inline constexpr u16 kSubJsonEof       = 1403;  // 途中で入力終端
inline constexpr u16 kSubJsonBadNumber = 1404;  // 数値として解釈できない
inline constexpr u16 kSubJsonBadEscape = 1405;  // 不正なエスケープ / \u

} // namespace acs
