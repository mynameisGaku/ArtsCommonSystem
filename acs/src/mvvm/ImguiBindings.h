// SPDX-License-Identifier: Apache-2.0
// Mvvm ↔ ImGui ヘルパ — TObservable<T> をフレーム描画にバインドする
//
// 使い方:
//   FPlayerViewModel vm;
//   // 毎フレーム ImGui::Begin/End の中で:
//   acs::mvvm::imgui::Bind("HP",     vm.hp,   0.0f, 100.0f);
//   acs::mvvm::imgui::Bind("Mana",   vm.mana, 0.0f, 100.0f);
//   acs::mvvm::imgui::Bind("Lv",     vm.level, 1, 99);
//   acs::mvvm::imgui::Bind("無敵",    vm.invincible);
//   acs::mvvm::imgui::BindReadOnly("デバッグ表示", vm.hp);
//
// 各 Bind は:
//   ・TObservable から現在値を取得して ImGui ウィジェット描画
//   ・ユーザーが操作したら TObservable.Set() で書き戻す (= 全監視者に通知)
//
// 設計:
//   ・ImGui の即時 mode に合わせ、毎フレーム呼ぶ前提
//   ・TObservable.Set は値が変わったときだけ通知するので、毎フレーム呼んでも安全
#pragma once

#include "mvvm/Observable.h"
#include "mvvm/Command.h"
#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs { class FString; }

namespace acs::mvvm::imgui {

/**
 * f32 TObservable をスライダで編集する (操作されたら Set で書き戻す)。
 *
 * @param label ウィジェットのラベル。
 * @param v 編集対象の TObservable。
 * @param v_min スライダ下限。
 * @param v_max スライダ上限。
 */
void Bind(const char* label, TObservable<f32>& v, f32 v_min, f32 v_max) noexcept;

/**
 * f64 TObservable をスライダで編集する (操作されたら Set で書き戻す)。
 *
 * @param label ウィジェットのラベル。
 * @param v 編集対象の TObservable。
 * @param v_min スライダ下限。
 * @param v_max スライダ上限。
 */
void Bind(const char* label, TObservable<f64>& v, f64 v_min, f64 v_max) noexcept;

/**
 * i32 TObservable をスライダで編集する (操作されたら Set で書き戻す)。
 *
 * @param label ウィジェットのラベル。
 * @param v 編集対象の TObservable。
 * @param v_min スライダ下限。
 * @param v_max スライダ上限。
 */
void Bind(const char* label, TObservable<i32>& v, i32 v_min, i32 v_max) noexcept;

/**
 * u32 TObservable をスライダで編集する (操作されたら Set で書き戻す)。
 *
 * @param label ウィジェットのラベル。
 * @param v 編集対象の TObservable。
 * @param v_min スライダ下限。
 * @param v_max スライダ上限。
 */
void Bind(const char* label, TObservable<u32>& v, u32 v_min, u32 v_max) noexcept;

/**
 * bool TObservable をチェックボックスで編集する (操作されたら Set で書き戻す)。
 *
 * @param label ウィジェットのラベル。
 * @param v 編集対象の TObservable。
 */
void Bind(const char* label, TObservable<bool>& v) noexcept;

/**
 * FVec2 TObservable を 2 成分スライダで編集する (操作されたら Set で書き戻す)。
 *
 * @param label ウィジェットのラベル。
 * @param v 編集対象の TObservable。
 * @param v_min 各成分の下限。
 * @param v_max 各成分の上限。
 */
void BindSlider2(const char* label, TObservable<FVec2>& v, f32 v_min, f32 v_max) noexcept;

/**
 * FVec3 TObservable を 3 成分スライダで編集する (操作されたら Set で書き戻す)。
 *
 * @param label ウィジェットのラベル。
 * @param v 編集対象の TObservable。
 * @param v_min 各成分の下限。
 * @param v_max 各成分の上限。
 */
void BindSlider3(const char* label, TObservable<FVec3>& v, f32 v_min, f32 v_max) noexcept;

/**
 * FVec3 TObservable を RGB カラーピッカーで編集する。
 *
 * @details FVec3 を 0..1 の RGB として扱う。操作されたら Set で書き戻す。
 * @param label ウィジェットのラベル。
 * @param v 編集対象の TObservable。
 */
void BindColor3(const char* label, TObservable<FVec3>& v) noexcept;

/**
 * i32 TObservable をコンボボックスで編集する (値を選択 index として扱う)。
 *
 * @details 選択されたら index を Set で書き戻す。
 * @param label ウィジェットのラベル。
 * @param v 選択 index を保持する TObservable。
 * @param labels 表示するラベル配列 (count 個、各要素 null 終端文字列)。
 * @param count labels の要素数。
 */
void BindCombo(const char* label, TObservable<i32>& v,
               const char* const* labels, u32 count) noexcept;

/**
 * FString TObservable をテキスト入力で編集する。
 *
 * @details
 * 編集確定時に persistent の内容を Set で書き戻し、非編集中は TObservable の最新値を
 * persistent へ同期する (外部変更の反映)。
 * @param label ウィジェットのラベル。
 * @param v 編集対象の TObservable。
 * @param persistent フレーム間で内容を保持する caller 所有の char バッファ。
 * @param cap persistent の容量 (バイト)。
 */
void BindText(const char* label, TObservable<acs::FString>& v,
              char* persistent, usize cap) noexcept;

/**
 * TObservable の現在値を printf 形式で表示する表示専用ラベル。
 *
 * @details
 * primary template は使われず、プリミティブ型ごとに ImguiBindings.cpp で明示特殊化する。
 * 特殊化が primary template の使用前に見える必要があるため宣言をここに置く。
 * @tparam T 表示する値の型。
 * @param fmt 値を 1 つ受け取る printf 書式 (例: "HP: %d")。
 * @param v 表示する TObservable。
 */
template<typename T>
void BindFormat(const char* fmt, const TObservable<T>& v) noexcept;

/**
 * BindFormat の f32 特殊化宣言。
 *
 * @param fmt f32 を 1 つ受け取る printf 書式。
 * @param v 表示する TObservable<f32>。
 */
template<> void BindFormat<f32>(const char*, const TObservable<f32>&) noexcept;

/**
 * BindFormat の f64 特殊化宣言。
 *
 * @param fmt f64 を 1 つ受け取る printf 書式。
 * @param v 表示する TObservable<f64>。
 */
template<> void BindFormat<f64>(const char*, const TObservable<f64>&) noexcept;

/**
 * BindFormat の i32 特殊化宣言。
 *
 * @param fmt i32 を 1 つ受け取る printf 書式。
 * @param v 表示する TObservable<i32>。
 */
template<> void BindFormat<i32>(const char*, const TObservable<i32>&) noexcept;

/**
 * BindFormat の u32 特殊化宣言。
 *
 * @param fmt u32 を 1 つ受け取る printf 書式。
 * @param v 表示する TObservable<u32>。
 */
template<> void BindFormat<u32>(const char*, const TObservable<u32>&) noexcept;

/**
 * FCommand をボタンとして描画し、クリックで実行する。
 *
 * @details can_execute が false の間は grayout 表示され実行されない。
 * @param label ボタンのラベル。
 * @param cmd 描画・実行する FCommand。
 * @return クリックされ実行できたら true。
 */
bool BindCommand(const char* label, FCommand& cmd) noexcept;

/**
 * f32 TObservable の現在値を読み取り専用テキストで表示する。
 *
 * @param label 先頭に付けるラベル。
 * @param v 表示する TObservable。
 */
void BindReadOnly(const char* label, const TObservable<f32>& v) noexcept;

/**
 * i32 TObservable の現在値を読み取り専用テキストで表示する。
 *
 * @param label 先頭に付けるラベル。
 * @param v 表示する TObservable。
 */
void BindReadOnly(const char* label, const TObservable<i32>& v) noexcept;

/**
 * bool TObservable の現在値を読み取り専用テキストで表示する。
 *
 * @param label 先頭に付けるラベル。
 * @param v 表示する TObservable。
 */
void BindReadOnly(const char* label, const TObservable<bool>& v) noexcept;

/**
 * f32 TObservable の値を [v_min, v_max] で正規化してプログレスバー表示する。
 *
 * @details HP/MP などの割合表示に使う。
 * @param label バーの後ろに付けるラベル。
 * @param v 表示する TObservable。
 * @param v_min 0% に対応する下限。
 * @param v_max 100% に対応する上限。
 */
void BindProgress(const char* label, const TObservable<f32>& v, f32 v_min, f32 v_max) noexcept;

} // namespace acs::mvvm::imgui
