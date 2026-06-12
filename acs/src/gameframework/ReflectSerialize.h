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
#include "gameframework/Reflect.h"   // FTypeDesc / FTypeId / EFieldKind / FTypeRegistry

namespace acs::game {

/** フォーマット識別 + バージョン (上位でフォーマット変更時に上げる)。 */
inline constexpr u32 kReflectSerializeMagic = 0xAC5F0001u;

/**
 * 反射メタデータに従って obj を buf[0..cap) へ直列化する。
 *
 * @param d   反射記述子 (FTypeRegistry から得る)。
 * @param obj 直列化するインスタンスの先頭ポインタ。
 * @param buf 出力バッファ。
 * @param cap buf の容量 (バイト)。
 * @return 書き込んだ総バイト数。引数不正や cap 不足なら 0。
 */
u32 SerializeReflected(const FTypeDesc* d, const void* obj, u8* buf, u32 cap) noexcept;

/** 型名でレジストリを引いてから SerializeReflected する (未登録は 0)。 */
u32 SerializeByName(const char* type_name, const void* obj, u8* buf, u32 cap) noexcept;

/**
 * data[0..size) を d の型の obj へ書き戻す (名前一致フィールドのみ復元、未知は無視)。
 *
 * @return 適用できたフィールド数。magic 不正 / 型 id 不一致 / 破損は 0。
 */
u32 DeserializeReflected(const FTypeDesc* d, void* obj, const u8* data, u32 size) noexcept;

/**
 * data 先頭ヘッダの type_id から型を特定し、factory で生成して復元する。
 *
 * @param data        直列化データ。
 * @param size        data のバイト数。
 * @param out_type_id 生成した型の id を返す (破棄に使う、非 null 推奨)。
 * @return 生成・復元したインスタンス (失敗 nullptr)。破棄は FTypeRegistry::Get().Destroy(*out_type_id, p)。
 */
void* CreateFromBytes(const u8* data, u32 size, FTypeId* out_type_id) noexcept;

} // namespace acs::game
