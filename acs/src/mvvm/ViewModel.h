// ViewModel — MVVM の M-V-VM のうち中央の VM 基底クラス
//
// 使い方:
//   class PlayerViewModel : public ViewModel {
//   public:
//       Observable<f32>     hp     { 100.0f };
//       Observable<f32>     mana   { 50.0f };
//       Observable<i32>     level  { 1 };
//       Observable<bool>    invincible { false };
//   };
//
//   // モデル (実データ) からの反映:
//   void OnDamage(PlayerViewModel& vm, f32 dmg) {
//       vm.hp.Set(vm.hp.Get() - dmg);    // 自動的に View 側へ propagate
//   }
//
// 設計:
//   ・基底はあえて空 (RTTI なしでも識別不要)。命名と意図のみ提供。
//   ・派生クラスが Observable<T> をメンバに持って公開する
//   ・View 側は Subscribe / Bind で監視する
#pragma once

#include "mvvm/Observable.h"
#include "mvvm/Binder.h"

namespace acs {

class ViewModel {
public:
    ViewModel() noexcept = default;
    virtual ~ViewModel() noexcept = default;

    ViewModel(const ViewModel&) = delete;
    ViewModel& operator=(const ViewModel&) = delete;
};

} // namespace acs
