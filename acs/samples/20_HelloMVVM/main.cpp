// SPDX-License-Identifier: Apache-2.0
// HelloMVVM — エントリポイント。
//
// 1 ウィンドウ内を 4 セクションに分けて MVVM の機能を順番に紹介する。
//   ① 5 分入門 — Observable<T>::Set/Get + imgui::Bind だけで動く最小例
//   ② バインダ — OneWayBinder / OneWayConvertBinder
//   ③ Derived  — HP/MaxHP から ratio を自動算出
//   ④ Array + Command — インベントリ追加削除 + 攻撃ボタン
//
// ACS_DEFINE_MAIN は WinMain / main の両エントリを裏で生成するため、
// プラットフォーム別の main 関数を書き分ける必要がない。
#include "app/EntryPoint.h"
#include "HelloMVVMApp.h"

ACS_DEFINE_MAIN(hellomvvm::HelloMVVMApp)
