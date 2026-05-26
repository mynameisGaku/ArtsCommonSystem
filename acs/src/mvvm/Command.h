// SPDX-License-Identifier: Apache-2.0
// FCommand — VM のアクション (ボタン等) を FViewModel 側に置くためのヘルパ
//
// 使い方:
//   class PlayerVM : public FViewModel {
//   public:
//       FObservable<f32> hp { 100.0f };
//
//       FCommand attack {
//           [](void* user){ static_cast<PlayerVM*>(user)->hp.Set(0); },
//           this
//       };
//
//       // can_execute を FObservable<bool> で外から差し替えるなら 3 引数版:
//       FCommand revive {
//           [](void* u){ static_cast<PlayerVM*>(u)->hp.Set(100); },
//           this,
//           &is_dead   // この FObservable<bool> が true のときだけ実行可能
//       };
//
//       FObservable<bool> is_dead { false };
//   };
//
// 設計:
//   ・can_execute はオプション (常時実行可能がデフォルト)
//   ・can_execute は FObservable<bool>* を取る (生存期間は呼び出し元責任)
//   ・Execute は can_execute=false なら no-op
//   ・ImGui アダプタの BindCommand(label, cmd) でボタン化 + 自動 grayout
#pragma once

#include "foundation/Types.h"
#include "mvvm/Observable.h"

namespace acs {

class FCommand {
public:
    using ExecFn = void (*)(void* user);

    // 常時実行可能版
    FCommand(ExecFn fn, void* user) noexcept
        : _fn(fn), _user(user), _can_execute(nullptr) {}

    // 条件付き実行版 (can_execute が false の間は Execute() が no-op)
    FCommand(ExecFn fn, void* user, FObservable<bool>* can_execute) noexcept
        : _fn(fn), _user(user), _can_execute(can_execute) {}

    // 実行 (条件不成立なら何もしない)
    void Execute() noexcept {
        if (!_fn) return;
        if (_can_execute && !_can_execute->Get()) return;
        _fn(_user);
    }

    // 現在実行可能か (UI の grayout 判定に使う)
    bool CanExecute() const noexcept {
        return _fn && (!_can_execute || _can_execute->Get());
    }

    // 内部から無効化したいとき (VM の dtor で呼ぶと吊り回避)
    void Invalidate() noexcept { _fn = nullptr; _user = nullptr; }

    // can_execute FObservable のポインタ (BindCommand が監視するため)
    FObservable<bool>* CanExecuteSource() const noexcept { return _can_execute; }

private:
    ExecFn            _fn          = nullptr;
    void*             _user        = nullptr;
    FObservable<bool>* _can_execute = nullptr;
};

} // namespace acs
