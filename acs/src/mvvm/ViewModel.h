// SPDX-License-Identifier: Apache-2.0
// FViewModel — MVVM の M-V-VM のうち中央の VM 基底クラス
//
// MVVM は **一般的な UI architecture pattern** (WPF / Xamarin / Vue / React 等で
// 共通) であり、UE5 の UMG FViewModel 専用の概念ではない。
// ACS の MVVM は次の 3 層で構成される:
//   Model     = ゲームロジック / アセット / ECS 内のデータ
//   View      = src/ui/ の Widget tree (Label / Button / Slider 等)、または
//               src/imgui/ の ImGui (ad-hoc デバッグ用、本番 UI は src/ui/ 推奨)
//   FViewModel = この基底を継承して Observable<T> プロパティを公開するクラス
//
// 使い方:
//   class PlayerViewModel : public FViewModel {
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

class FViewModel {
public:
    FViewModel() noexcept = default;
    virtual ~FViewModel() noexcept = default;

    FViewModel(const FViewModel&) = delete;
    FViewModel& operator=(const FViewModel&) = delete;
};

} // namespace acs
