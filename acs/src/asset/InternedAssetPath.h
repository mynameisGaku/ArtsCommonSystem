// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "asset/AssetId.h"
#include "container/Array.h"
#include "foundation/Types.h"

namespace acs {

/**
 * 非同期ロード間で共有する不変アセットパス。
 *
 * @details 生成後に文字列を変更せず、共有参照が残る間はパスの寿命を保証する。
 */
class FInternedAssetPath {
public:
    /** 指定アロケータで空のパスを構築する。 */
    FInternedAssetPath(FAllocator& Allocator, FAssetId Id) noexcept;

    /** パス文字列を一度だけ設定する。 */
    bool TryInitialize(const wchar_t* Path, usize Length) noexcept;

    /** NUL 終端されたパスを返す。 */
    const wchar_t* Path() const noexcept;

    /** NUL を除く文字数を返す。 */
    usize Length() const noexcept;

    /** パスから計算済みのアセット ID を返す。 */
    FAssetId Id() const noexcept;

    /** 指定文字列と完全一致するかを返す。 */
    bool Equals(const wchar_t* Path, usize Length) const noexcept;

private:
    /** NUL 終端を含む所有文字列。 */
    TArray<wchar_t> m_Units;

    /** パスから計算したアセット ID。 */
    FAssetId m_Id{};
};

} // namespace acs
