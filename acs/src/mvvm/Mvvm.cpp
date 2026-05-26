// SPDX-License-Identifier: Apache-2.0
// MVVM モジュール本体は header-only (FObservable / Binder / FViewModel)。
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
template class FObservable<i32>;
template class FObservable<u32>;
template class FObservable<f32>;
template class FObservable<f64>;
template class FObservable<bool>;
template class FObservable<FVec2>;
template class FObservable<FVec3>;
template class FObservable<FVec4>;
template class FObservable<FString>;
} // namespace acs
