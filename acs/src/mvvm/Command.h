// Command — VM のアクション (ボタン等) を ViewModel 側に置くためのヘルパ
//
// 使い方:
//   class PlayerVM : public ViewModel {
//   public:
//       Observable<f32> hp { 100.0f };
//
//       Command attack {
//           [](void* user){ static_cast<PlayerVM*>(user)->hp.Set(0); },
//           this
//       };
//
//       // can_execute を Observable<bool> で外から差し替えるなら 3 引数版:
//       Command revive {
//           [](void* u){ static_cast<PlayerVM*>(u)->hp.Set(100); },
//           this,
//           &is_dead   // この Observable<bool> が true のときだけ実行可能
//       };
//
//       Observable<bool> is_dead { false };
//   };
//
// 設計:
//   ・can_execute はオプション (常時実行可能がデフォルト)
//   ・can_execute は Observable<bool>* を取る (生存期間は呼び出し元責任)
//   ・Execute は can_execute=false なら no-op
//   ・ImGui アダプタの BindCommand(label, cmd) でボタン化 + 自動 grayout
#pragma once

#include "foundation/Types.h"
#include "mvvm/Observable.h"

namespace acs {

class Command {
public:
    using ExecFn = void (*)(void* user);

    // 常時実行可能版
    Command(ExecFn fn, void* user) noexcept
        : _fn(fn), _user(user), _can_execute(nullptr) {}

    // 条件付き実行版 (can_execute が false の間は Execute() が no-op)
    Command(ExecFn fn, void* user, Observable<bool>* can_execute) noexcept
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

    // can_execute Observable のポインタ (BindCommand が監視するため)
    Observable<bool>* CanExecuteSource() const noexcept { return _can_execute; }

private:
    ExecFn            _fn          = nullptr;
    void*             _user        = nullptr;
    Observable<bool>* _can_execute = nullptr;
};

} // namespace acs
