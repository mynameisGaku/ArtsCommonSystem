// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — リフレクション駆動シリアライザ (ReflectSerialize)
// -----------------------------------------------------------------------------
// FReflectField のメタデータ (name / offset / size / kind) を実行時に走査して、
// 任意の ACS_REFLECT* 型を「型を知らずに」バイト列へ往復させる土台。これにより
// scene save/load・editor play mode・ゲーム進行セーブ・prefab 永続化が、すべて
// この 1 プリミティブの薄い利用側として実装できる。
//
// フォーマット (自己記述・name-keyed → フィールドの追加/削除/並べ替えに耐える):
//   [u32 magic][u32 type_id][u32 field_count]
//   per field: [u8 name_len][name…][u8 kind][u8 size][size bytes raw value]
// 復元時は「保存された各フィールド名」を生きた型の FReflectField から探し、name+kind+
// size が一致したものだけ offset へ memcpy する (未知フィールドは読み飛ばし、消えた
// フィールドは factory の既定値が残る)。
//
// 対象外フィールド (スキップ):
//   ・size == 0 … 名前のみ反射 (ACS_RPROP。実体メモリを持たないエディタ用スキーマ)。
//   ・kind == FString … メンバはポインタ (const char**) なので生バイト保存は無意味。
//
// 規約: no-STL / no-exceptions / 全 noexcept / 固定バッファ。エンディアン/アラインは
// 同一マシンの生バイト前提 (将来クロスプラットフォーム対応の余地あり)。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "gameframework/Reflect.h"   // FTypeDesc / FTypeId / EFieldKind / CTypeRegistry

namespace acs::game {

/** フォーマット識別 + バージョン (上位でフォーマット変更時に上げる)。 */
inline constexpr u32 kReflectSerializeMagic = 0xAC5F0001u;
inline constexpr u32 kReflectSerializeMaxFieldCount = 1024u;
inline constexpr u32 kReflectSerializeMaxFieldNameBytes = 255u;
inline constexpr u32 kReflectSerializeMaxFieldValueBytes = 16u;

/** 検証付き復元が返す失敗理由。 */
enum class EReflectSerializeError : u8 {
    None = 0,
    NullDescriptor,
    NullObject,
    NullData,
    TruncatedData,
    InvalidMagic,
    TypeMismatch,
    FieldLimitExceeded,
    InvalidFieldRecord,
    InvalidMetadata,
    NullOutput,
    BufferTooSmall,
    SerializedSizeOverflow,
};

/** 検証付き直列化の結果。容量不足時にも RequiredBytes を返し、出力は変更しない。 */
struct FReflectSerializeResult {
    EReflectSerializeError Error = EReflectSerializeError::None;
    u32 BytesWritten = 0u;
    u32 RequiredBytes = 0u;
    u32 FieldsSerialized = 0u;

    bool Succeeded() const noexcept {
        return Error == EReflectSerializeError::None
            && BytesWritten > 0u && BytesWritten == RequiredBytes;
    }
    explicit operator bool() const noexcept { return Succeeded(); }
};

/** 反射データの検証・復元結果。失敗時は対象オブジェクトを一切変更しない。 */
struct FReflectDeserializeResult {
    EReflectSerializeError Error = EReflectSerializeError::None;
    u32 FieldsApplied = 0u;
    u32 BytesRead = 0u;
    FTypeId SerializedTypeId = 0u;

    bool Succeeded() const noexcept { return Error == EReflectSerializeError::None; }
    explicit operator bool() const noexcept { return Succeeded(); }
};

/** ログ・診断表示用の安定した ASCII エラー名を返す。 */
const char* ReflectSerializeErrorName(EReflectSerializeError error) noexcept;

/**
 * 反射メタデータと必要サイズを検証した後だけ obj を buf へ直列化する。
 *
 * @details buf=nullptr/cap=0 はサイズ照会として使え、BufferTooSmall と RequiredBytes を返す。
 * 容量不足、null 出力、不正メタデータでは buf を一切変更しない。
 */
FReflectSerializeResult TrySerializeReflected(
    const FTypeDesc* d, const void* obj, u8* buf, u32 cap) noexcept;

/**
 * 反射メタデータに従って obj を buf[0..cap) へ直列化する。
 *
 * @param d   反射記述子 (CTypeRegistry から得る)。
 * @param obj 直列化するインスタンスの先頭ポインタ。
 * @param buf 出力バッファ。
 * @param cap buf の容量 (バイト)。
 * @return 書き込んだ総バイト数。引数不正や cap 不足なら 0。失敗時も buf は変更しない。
 */
u32 SerializeReflected(const FTypeDesc* d, const void* obj, u8* buf, u32 cap) noexcept;

/** 型名でレジストリを引いてから SerializeReflected する (未登録は 0)。 */
u32 SerializeByName(const char* type_name, const void* obj, u8* buf, u32 cap) noexcept;

/**
 * data[0..size) を完全検証した後、d の型の obj へ一括適用する。
 *
 * @details
 * ヘッダ、全フィールド境界、フィールド数上限、反射メタデータのメモリ範囲を先に検証する。
 * 破損が後半で見つかっても、obj は呼び出し前の状態から一切変更されない。
 */
FReflectDeserializeResult TryDeserializeReflected(
    const FTypeDesc* d, void* obj, const u8* data, u32 size) noexcept;

/**
 * data[0..size) を d の型の obj へ書き戻す (名前一致フィールドのみ復元、未知は無視)。
 *
 * @details 互換用の簡易 API。詳細な失敗理由が必要なら TryDeserializeReflected を使う。
 * @return 適用できたフィールド数。magic 不正 / 型 id 不一致 / 破損は 0。
 */
u32 DeserializeReflected(const FTypeDesc* d, void* obj, const u8* data, u32 size) noexcept;

/**
 * data 先頭ヘッダの type_id から型を特定し、factory で生成して復元する。
 *
 * @param data        直列化データ。
 * @param size        data のバイト数。
 * @param out_type_id 生成した型の id を返す (破棄に使う、非 null 推奨)。
 * @return 生成・完全復元したインスタンス (失敗 nullptr)。破損時は生成物を内部で破棄する。
 *         破棄は CTypeRegistry::Get().Destroy(*out_type_id, p)。
 */
void* CreateFromBytes(const u8* data, u32 size, FTypeId* out_type_id) noexcept;

} // namespace acs::game
