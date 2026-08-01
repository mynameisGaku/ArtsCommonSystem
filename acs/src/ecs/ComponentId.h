// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "threading/Atomic.h"

namespace acs {

/** コンポーネント型 ID (0..kMaxComponentTypes-1、ストレージ配列の添字に使う)。 */
using FComponentTypeId = u32;

/** ビルド内で安定したコンパイル時コンポーネント署名。永続化 ID には使用しない。 */
using FComponentSignatureId = u64;

/** 旧名を使う既存コード向けの互換別名。 */
using ComponentTypeId = FComponentTypeId;

/** 旧名を使う既存コード向けの互換別名。 */
using ComponentSignatureId = FComponentSignatureId;

/** 同時に扱えるコンポーネント型の上限 (Slots 配列の長さ)。 */
inline constexpr FComponentTypeId kMaxComponentTypes = 256;

namespace ecs_detail {
/** 全 T 共通の採番カウンタ (次に割り当てる ID を保持)。 */
inline TAtomic<u32> g_next_component_type_id{0};

/** 非nullでNUL終端された型署名文字列をFNV-1aでハッシュ化する。 */
constexpr FComponentSignatureId HashComponentSignature(const char* text) noexcept
{
    /** FNV-1a の途中値。 */
    FComponentSignatureId hash = 14695981039346656037ull;
    while (*text != '\0') {
        hash ^= static_cast<u8>(*text++);
        hash *= 1099511628211ull;
    }
    return hash;
}

/** 一文字が型名の一部として扱われる安全側の文字かを返す。 */
constexpr bool IsComponentSignatureIdentifierByte(char value) noexcept
{
    /** 符号拡張を避けた符号なし文字値。 */
    const u8 byte_value = static_cast<u8>(value);
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') || value == '_' || value == '$' || byte_value >= 0x80u;
}

/** 指定位置に対象文字列があるかを返す。 */
constexpr bool HasComponentSignatureTextAt(const char* text, usize position, const char* expected) noexcept
{
    /** 比較中の文字位置。 */
    usize expected_position = 0;
    while (expected[expected_position] != '\0') {
        if (text[position + expected_position] != expected[expected_position]) {
            return false;
        }
        ++expected_position;
    }
    return true;
}

/** 非nullでNUL終端された署名から対象関数名を探し、未検出ならusize最大値を返す。 */
constexpr usize FindComponentSignatureFunction(const char* text) noexcept
{
    /** 対象とする関数名。 */
    constexpr const char* kFunctionName = "acs::ecs_detail::StaticComponentSignature";
    /** 対象関数名の文字数。 */
    constexpr usize kFunctionNameLength = 41u;
    /** 対象文字列を走査する位置。 */
    usize position = 0;
    for (; text[position] != '\0'; ++position) {
        /** 関数名の左側が別の識別子へ接続していないかの判定。 */
        const bool valid_left_boundary = position == 0u || (!IsComponentSignatureIdentifierByte(text[position - 1u]) && text[position - 1u] != ':');
        if (valid_left_boundary && HasComponentSignatureTextAt(text, position, kFunctionName) && text[position + kFunctionNameLength] == '(') {
            return position;
        }
    }
    return static_cast<usize>(-1);
}

/** 指定位置に補正対象となる正規型名が一つの名前としてあるかを返す。 */
constexpr bool IsCanonicalComponentSignatureToken(const char* text, usize position, usize function_position) noexcept
{
    /** 正規型の名前表記。 */
    constexpr const char* kCanonicalToken = "FComponentSignatureId";
    /** 正規型名の文字数。 */
    constexpr usize kCanonicalTokenLength = 21u;
    /** 正規型名を比較する文字位置。 */
    usize token_position = 0;
    for (; token_position < kCanonicalTokenLength; ++token_position) {
        if (text[position + token_position] != kCanonicalToken[token_position]) {
            return false;
        }
    }
    if (text[position + kCanonicalTokenLength] != ' ') {
        return false;
    }
    if (position == 0u) {
        return position < function_position;
    }
    if (position < 2u || text[position - 1u] != ':' || text[position - 2u] != ':') {
        return false;
    }
    /** 許可する完全修飾名の直前部分。 */
    constexpr const char* kAcsQualifier = "acs::";
    /** 名前空間表記の文字数。 */
    constexpr usize kAcsQualifierLength = 5u;
    if (position < kAcsQualifierLength) {
        return false;
    }
    /** 名前空間表記を比較する文字位置。 */
    usize qualifier_position = 0;
    for (; qualifier_position < kAcsQualifierLength; ++qualifier_position) {
        if (text[position - kAcsQualifierLength + qualifier_position] != kAcsQualifier[qualifier_position]) {
            return false;
        }
    }
    if (position > kAcsQualifierLength && (IsComponentSignatureIdentifierByte(text[position - kAcsQualifierLength - 1u]) || text[position - kAcsQualifierLength - 1u] == ':')) {
        return false;
    }
    if (position < function_position) {
        return true;
    }
    /** 型名直後の空白を越えた文字位置。 */
    const usize binding_position = position + kCanonicalTokenLength + 1u;
    return text[binding_position] == '=' && text[binding_position + 1u] == ' ';
}

/** 非nullでNUL終端された署名内の既知の型別名だけを旧表記へ戻し、対象関数がなければ通常どおりハッシュ化する。 */
constexpr FComponentSignatureId HashCompatibleComponentSignature(const char* text) noexcept
{
    /** 対象関数名の開始位置。 */
    const usize function_position = FindComponentSignatureFunction(text);
    if (function_position == static_cast<usize>(-1)) {
        return HashComponentSignature(text);
    }
    /** FNV-1a の途中値。 */
    FComponentSignatureId hash = 14695981039346656037ull;
    /** 現在処理している文字位置。 */
    usize position = 0;
    while (text[position] != '\0') {
        if (IsCanonicalComponentSignatureToken(text, position, function_position)) {
            ++position;
        }
        hash ^= static_cast<u8>(text[position]);
        hash *= 1099511628211ull;
        ++position;
    }
    return hash;
}

/** コンパイラが生成する型署名からコンポーネント署名を作る。 */
template<typename T>
constexpr FComponentSignatureId StaticComponentSignature() noexcept
{
#if defined(_MSC_VER)
    return HashCompatibleComponentSignature(__FUNCSIG__);
#else
    return HashCompatibleComponentSignature(__PRETTY_FUNCTION__);
#endif
}
} // namespace ecs_detail

/** 型ごとに実行時 ID を一度だけ割り当てて返す。 */
template<typename T>
FComponentTypeId GetComponentTypeId() noexcept;

/**
 * コンパイル時クエリ・振り分け用の型特性。
 *
 * @details Signature は型パックの比較・特殊化に使い、World ストレージの密な添字は
 * 従来どおり RuntimeId() の動的代替経路を使う。これによりプラグイン型の後付け互換を保つ。
 */
template<typename T>
struct TComponentTypeTraits {
    /** コンパイル時に求めた型署名。 */
    static constexpr FComponentSignatureId Signature = ecs_detail::StaticComponentSignature<T>();

    /** World 内部で使う密な実行時 ID を返す。 */
    static FComponentTypeId RuntimeId() noexcept
    {
        return GetComponentTypeId<T>();
    }
};

/** 型 T のコンパイル時署名を返す。 */
template<typename T>
constexpr FComponentSignatureId GetComponentSignatureId() noexcept
{
    return TComponentTypeTraits<T>::Signature;
}

/**
 * 型 T に固有な FComponentTypeId を返す (初回呼び出しで採番、以降はキャッシュ)。
 *
 * @details
 * 関数静的変数で型ごとに 1 度だけ割り当てる (C++ の magic statics によりスレッド
 * セーフ)。採番自体は TAtomic の FetchAdd なので、複数スレッドから同じ型 T を初めて
 * 呼んでも一意な値に確定する。割り当ては呼び出し順依存のため、決定的な値は保証しない。
 * @tparam T ID を割り当てるコンポーネント型。
 * @return 型 T に対応する FComponentTypeId。
 */
template<typename T>
FComponentTypeId GetComponentTypeId() noexcept {
    static const FComponentTypeId id = ecs_detail::g_next_component_type_id.FetchAdd(1);
    return id;
}

} // namespace acs
