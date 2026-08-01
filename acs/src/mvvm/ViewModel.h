// SPDX-License-Identifier: Apache-2.0
// CViewModel — MVVM の M-V-VM のうち中央の VM 基底クラス
//
// MVVM は **一般的な UI architecture pattern** (WPF / Xamarin / Vue / React 等で
// 共通) であり、UE5 の UMG CViewModel 専用の概念ではない。
// ACS の MVVM は次の 3 層で構成される:
//   Model     = ゲームロジック / アセット / ECS 内のデータ
//   View      = src/ui/ の FWidget tree (FLabel / FButton / FSlider 等)、または
//               src/imgui/ の ImGui (ad-hoc デバッグ用、本番 UI は src/ui/ 推奨)
//   CViewModel = この基底を継承して TObservable<T> プロパティを公開するクラス
//
// 使い方:
//   class FPlayerViewModel : public CViewModel {
//   public:
//       TObservable<f32>     hp     { 100.0f };
//       TObservable<f32>     mana   { 50.0f };
//       TObservable<i32>     level  { 1 };
//       TObservable<bool>    invincible { false };
//   };
//
//   // モデル (実データ) からの反映:
//   void OnDamage(FPlayerViewModel& vm, f32 dmg) {
//       vm.hp.Set(vm.hp.Get() - dmg);    // 自動的に View 側へ propagate
//   }
//
// 設計:
//   ・基底はあえて空 (RTTI なしでも識別不要)。命名と意図のみ提供。
//   ・派生クラスが TObservable<T> をメンバに持って公開する
//   ・View 側は Subscribe / Bind で監視する
#pragma once

#include "mvvm/Observable.h"
#include "mvvm/Binder.h"

namespace acs {

/**
 * MVVM の CViewModel 基底クラス。
 *
 * @details
 * 本体はあえて空で (RTTI 不要・識別子なし)、命名と意図のみを提供する。派生クラスが
 * TObservable<T> をメンバとして公開し、View 側は Subscribe / Bind でそれを監視する。
 * Model のデータ更新を TObservable.Set で反映すると View へ自動伝播する。
 */
class CViewModel {
public:
    /** 空の CViewModel を構築する。 */
    CViewModel() noexcept = default;

    /** 派生クラスを正しく破棄するための仮想デストラクタ。 */
    virtual ~CViewModel() noexcept = default;

    /** コピー禁止 (TObservable メンバの購読を単独所有するため)。 */
    CViewModel(const CViewModel&) = delete;

    /** コピー代入も禁止。 */
    CViewModel& operator=(const CViewModel&) = delete;
};

/** 移行期間中に旧名を受け付ける互換別名。 */
using FViewModel = CViewModel;

} // namespace acs
