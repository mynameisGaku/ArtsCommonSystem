// Mvvm ↔ ImGui ヘルパ — Observable<T> をフレーム描画にバインドする
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
//   ・Observable から現在値を取得して ImGui ウィジェット描画
//   ・ユーザーが操作したら Observable.Set() で書き戻す (= 全監視者に通知)
//
// 設計:
//   ・ImGui の即時 mode に合わせ、毎フレーム呼ぶ前提
//   ・Observable.Set は値が変わったときだけ通知するので、毎フレーム呼んでも安全
#pragma once

#include "mvvm/Observable.h"
#include "mvvm/Command.h"
#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs { class String; }

namespace acs::mvvm::imgui {

// === 編集可能 (View → ViewModel に書き戻す) ===
void Bind(const char* label, Observable<f32>& v, f32 v_min, f32 v_max) noexcept;
void Bind(const char* label, Observable<f64>& v, f64 v_min, f64 v_max) noexcept;
void Bind(const char* label, Observable<i32>& v, i32 v_min, i32 v_max) noexcept;
void Bind(const char* label, Observable<u32>& v, u32 v_min, u32 v_max) noexcept;
void Bind(const char* label, Observable<bool>& v) noexcept;

// === Vec2/Vec3 スライダ ===
void BindSlider2(const char* label, Observable<Vec2>& v, f32 v_min, f32 v_max) noexcept;
void BindSlider3(const char* label, Observable<Vec3>& v, f32 v_min, f32 v_max) noexcept;

// === RGB カラーピッカー (Vec3 を 0..1 の RGB として扱う) ===
void BindColor3(const char* label, Observable<Vec3>& v) noexcept;

// === コンボボックス (i32 を index として、ラベル配列から選ぶ) ===
//   labels   : 表示するラベルの配列 (count 個、null終端文字列)
//   count    : labels の要素数
void BindCombo(const char* label, Observable<i32>& v,
               const char* const* labels, u32 count) noexcept;

// === 文字列入力 (String 型 Observable を編集) ===
//   persistent : caller が持つ char バッファ (フレーム間で内容保持される必要あり)
//   cap        : persistent の容量 (バイト)
void BindText(const char* label, Observable<acs::String>& v,
              char* persistent, usize cap) noexcept;

// === printf 形式の表示専用ラベル (例: "HP: %d") ===
template<typename T>
void BindFormat(const char* fmt, const Observable<T>& v) noexcept;
// プリミティブ型は明示インスタンシエーション (ImguiBindings.cpp 側で実装)
extern template void BindFormat<f32>(const char*, const Observable<f32>&) noexcept;
extern template void BindFormat<f64>(const char*, const Observable<f64>&) noexcept;
extern template void BindFormat<i32>(const char*, const Observable<i32>&) noexcept;
extern template void BindFormat<u32>(const char*, const Observable<u32>&) noexcept;

// === Command (ボタン) ===
//   can_execute=false の間は grayout 表示
bool BindCommand(const char* label, Command& cmd) noexcept;

// === 読み取り専用テキスト ===
void BindReadOnly(const char* label, const Observable<f32>& v) noexcept;
void BindReadOnly(const char* label, const Observable<i32>& v) noexcept;
void BindReadOnly(const char* label, const Observable<bool>& v) noexcept;

// === プログレスバー (HP/MP 表示など) ===
void BindProgress(const char* label, const Observable<f32>& v, f32 v_min, f32 v_max) noexcept;

} // namespace acs::mvvm::imgui
