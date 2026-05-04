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
#include "foundation/Types.h"

namespace acs::mvvm::imgui {

// 編集可能なバインディング (View → ViewModel に書き戻す)
void Bind(const char* label, Observable<f32>& v, f32 v_min, f32 v_max) noexcept;
void Bind(const char* label, Observable<f64>& v, f64 v_min, f64 v_max) noexcept;
void Bind(const char* label, Observable<i32>& v, i32 v_min, i32 v_max) noexcept;
void Bind(const char* label, Observable<u32>& v, u32 v_min, u32 v_max) noexcept;
void Bind(const char* label, Observable<bool>& v) noexcept;

// 読み取り専用 (ラベル + 値の表示のみ、ウィジェット非表示)
void BindReadOnly(const char* label, const Observable<f32>& v) noexcept;
void BindReadOnly(const char* label, const Observable<i32>& v) noexcept;
void BindReadOnly(const char* label, const Observable<bool>& v) noexcept;

// プログレスバー (HP/MP 表示など)
void BindProgress(const char* label, const Observable<f32>& v, f32 v_min, f32 v_max) noexcept;

} // namespace acs::mvvm::imgui
