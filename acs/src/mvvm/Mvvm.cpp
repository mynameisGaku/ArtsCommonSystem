// SPDX-License-Identifier: Apache-2.0
// MVVM モジュール本体は header-only (Observable / Binder / FViewModel)。
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

// header-only な MVVM に翻訳単位を 1 つ与えるため、主要な値型について
// Observable<T> を明示インスタンス化する。

/** i32 値の Observable。 */
template class Observable<i32>;

/** u32 値の Observable。 */
template class Observable<u32>;

/** f32 値の Observable。 */
template class Observable<f32>;

/** f64 値の Observable。 */
template class Observable<f64>;

/** bool 値の Observable。 */
template class Observable<bool>;

/** FVec2 値の Observable。 */
template class Observable<FVec2>;

/** FVec3 値の Observable。 */
template class Observable<FVec3>;

/** FVec4 値の Observable。 */
template class Observable<FVec4>;

/** FString 値の Observable。 */
template class Observable<FString>;

} // namespace acs
