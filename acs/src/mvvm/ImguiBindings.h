// SPDX-License-Identifier: Apache-2.0
// Mvvm ↔ ImGui ヘルパ — FObservable<T> をフレーム描画にバインドする
//
// 使い方:
//   PlayerViewModel vm;
//   // 毎フレーム ImGui::Begin/End の中で:
//   acs::mvvm::imgui::Bind("HP",     vm.hp,   0.0f, 100.0f);
//   acs::mvvm::imgui::Bind("Mana",   vm.mana, 0.0f, 100.0f);
//   acs::mvvm::imgui::Bind("Lv",     vm.level, 1, 99);
//   acs::mvvm::imgui::Bind("無敵",    vm.invincible);
//   acs::mvvm::imgui::BindReadOnly("デバッグ表示", vm.hp);
//
// 各 Bind は:
//   ・FObservable から現在値を取得して ImGui ウィジェット描画
//   ・ユーザーが操作したら FObservable.Set() で書き戻す (= 全監視者に通知)
//
// 設計:
//   ・ImGui の即時 mode に合わせ、毎フレーム呼ぶ前提
//   ・FObservable.Set は値が変わったときだけ通知するので、毎フレーム呼んでも安全
#pragma once

#include "mvvm/Observable.h"
#include "mvvm/Command.h"
#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs { class FString; }

namespace acs::mvvm::imgui {

// === 編集可能 (View → FViewModel に書き戻す) ===
void Bind(const char* label, FObservable<f32>& v, f32 v_min, f32 v_max) noexcept;
void Bind(const char* label, FObservable<f64>& v, f64 v_min, f64 v_max) noexcept;
void Bind(const char* label, FObservable<i32>& v, i32 v_min, i32 v_max) noexcept;
void Bind(const char* label, FObservable<u32>& v, u32 v_min, u32 v_max) noexcept;
void Bind(const char* label, FObservable<bool>& v) noexcept;

// === FVec2/FVec3 スライダ ===
void BindSlider2(const char* label, FObservable<FVec2>& v, f32 v_min, f32 v_max) noexcept;
void BindSlider3(const char* label, FObservable<FVec3>& v, f32 v_min, f32 v_max) noexcept;

// === RGB カラーピッカー (FVec3 を 0..1 の RGB として扱う) ===
void BindColor3(const char* label, FObservable<FVec3>& v) noexcept;

// === コンボボックス (i32 を index として、ラベル配列から選ぶ) ===
//   labels   : 表示するラベルの配列 (count 個、null終端文字列)
//   count    : labels の要素数
void BindCombo(const char* label, FObservable<i32>& v,
               const char* const* labels, u32 count) noexcept;

// === 文字列入力 (FString 型 FObservable を編集) ===
//   persistent : caller が持つ char バッファ (フレーム間で内容保持される必要あり)
//   cap        : persistent の容量 (バイト)
void BindText(const char* label, FObservable<acs::FString>& v,
              char* persistent, usize cap) noexcept;

// === printf 形式の表示専用ラベル (例: "HP: %d") ===
template<typename T>
void BindFormat(const char* fmt, const FObservable<T>& v) noexcept;
// プリミティブ型ごとの実装は ImguiBindings.cpp で明示的に特殊化している。
// 特殊化の「宣言」をここに置く（primary template が使われる前に特殊化が
// 見えている必要がある — extern template との併用は標準上 ill-formed）。
template<> void BindFormat<f32>(const char*, const FObservable<f32>&) noexcept;
template<> void BindFormat<f64>(const char*, const FObservable<f64>&) noexcept;
template<> void BindFormat<i32>(const char*, const FObservable<i32>&) noexcept;
template<> void BindFormat<u32>(const char*, const FObservable<u32>&) noexcept;

// === FCommand (ボタン) ===
//   can_execute=false の間は grayout 表示
bool BindCommand(const char* label, FCommand& cmd) noexcept;

// === 読み取り専用テキスト ===
void BindReadOnly(const char* label, const FObservable<f32>& v) noexcept;
void BindReadOnly(const char* label, const FObservable<i32>& v) noexcept;
void BindReadOnly(const char* label, const FObservable<bool>& v) noexcept;

// === プログレスバー (HP/MP 表示など) ===
void BindProgress(const char* label, const FObservable<f32>& v, f32 v_min, f32 v_max) noexcept;

} // namespace acs::mvvm::imgui
