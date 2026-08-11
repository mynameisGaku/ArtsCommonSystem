// SPDX-License-Identifier: Apache-2.0
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
