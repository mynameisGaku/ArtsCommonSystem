// SPDX-License-Identifier: Apache-2.0
// HelloMVVM — エントリポイント。
//
// 構成:
//   PlayerVM.h          - ViewModel 定義 (Observable / ObservableArray / Command)
//   HelloMVVMApp.{h,cpp} - Application 派生クラス (ImGui で MVVM 各機能を順紹介)
//
// 動作: 1 ウィンドウ内を 4 セクションに分けて MVVM の機能を順番に紹介。
//   ① 5 分入門   — Observable<T>::Set/Get + imgui::Bind だけで動く最小例
//   ② バインダ    — TwoWayBinder / OneWayBinder / OneWayConvertBinder
//   ③ Derived     — HP/MaxHP から ratio を自動算出
//   ④ ObservableArray + Command — インベントリ追加削除 + 攻撃ボタン
//
// 注: ACS_RENDER_DILIGENT との関係はない。Imgui モジュールが有効ならビルド可。
//
// ACS_DEFINE_MAIN は HelloMVVMApp を main エントリに登録 (Application 派生 →
// `int WINAPI WinMain` / `int main` 両方の通常 main を裏で生成)。
#include "app/EntryPoint.h"
#include "HelloMVVMApp.h"

ACS_DEFINE_MAIN(hellomvvm::HelloMVVMApp)
