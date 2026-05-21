// SPDX-License-Identifier: Apache-2.0
// MVVM モジュール本体は header-only (Observable / Binder / ViewModel)。
// このファイルは静的ライブラリとしてリンクするためのプレースホルダ。
// ImGui アダプタが有効な場合のみ ImguiBindings.cpp が追加される。
#include "mvvm/Observable.h"
#include "mvvm/Binder.h"
#include "mvvm/ViewModel.h"
#include "mvvm/ObservableArray.h"
#include "mvvm/Command.h"
#include "math/Vec.h"
#include "container/String.h"

namespace acs {
// 翻訳単位を 1 つ作るための明示インスタンシエーション
template class Observable<i32>;
template class Observable<u32>;
template class Observable<f32>;
template class Observable<f64>;
template class Observable<bool>;
template class Observable<Vec2>;
template class Observable<Vec3>;
template class Observable<Vec4>;
template class Observable<String>;
} // namespace acs
