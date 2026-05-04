// MVVM モジュール本体は header-only (Observable / Binder / ViewModel)。
// このファイルは静的ライブラリとしてリンクするためのプレースホルダ。
// ImGui アダプタが有効な場合のみ ImguiBindings.cpp が追加される。
#include "mvvm/Observable.h"
#include "mvvm/Binder.h"
#include "mvvm/ViewModel.h"

namespace acs {
// 翻訳単位を 1 つ作るための明示インスタンシエーション
template class Observable<i32>;
template class Observable<u32>;
template class Observable<f32>;
template class Observable<f64>;
template class Observable<bool>;
} // namespace acs
