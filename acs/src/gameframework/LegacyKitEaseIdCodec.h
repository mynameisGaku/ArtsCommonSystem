// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "gameframework/Easing.h"

namespace acs::game {

/**
 * 旧形式のイージング数値 ID と ACS の正規型を相互変換する。
 * 数式は保持せず、失敗時は出力引数を変更しない。
 */
class FLegacyKitEaseIdCodec final {
public:
    /** 旧形式で固定された有効 ID 数。 */
    static constexpr i32 kLegacyIdCount = 33;

    /** 状態を持たない変換型の生成を禁止する。 */
    FLegacyKitEaseIdCodec() = delete;

    /**
     * 旧形式の数値 ID を ACS の正規型へ変換する。
     *
     * @param legacy_id 0 以上 33 未満の旧形式 ID。
     * @param out_type 変換成功時に書き込む正規型。
     * @return 変換できた場合は true。不正値では false。
     */
    static bool TryDecode(i32 legacy_id, Easing::EEasingType& out_type) noexcept;

    /**
     * ACS の正規型を対応する旧形式の数値 ID へ変換する。
     *
     * @param type 変換する ACS の正規型。
     * @param out_legacy_id 変換成功時に書き込む旧形式 ID。
     * @return 旧形式に対応する型なら true。未対応型では false。
     */
    static bool TryEncode(Easing::EEasingType type, i32& out_legacy_id) noexcept;
};

} // namespace acs::game
