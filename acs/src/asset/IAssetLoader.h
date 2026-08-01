// SPDX-License-Identifier: Apache-2.0
// アセットローダのインターフェイス
//
// 各アセット型 (AImageAsset, AMeshAsset, AAudioAsset など) ごとにローダを実装し、
// CAssetRegistry に登録する。レジストリは拡張子から適切なローダを呼び出す。
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "memory/SharedPtr.h"
#include "container/Array.h"
#include "container/StringView.h"
#include "asset/Asset.h"

namespace acs {

/**
 * アセットローダのインターフェイス。
 *
 * @details
 * 各アセット型 (AImageAsset, AMeshAsset, AAudioAsset など) ごとに実装し、CAssetRegistry に登録する。
 * レジストリは Extension() でパスの拡張子からローダを選び、LoadFromBytes() を呼ぶ。
 * 非同期ロードはレジストリ側が FThreadPool で LoadFromBytes() を呼ぶ形で実現するため、
 * 実装は同期ロードだけを考えればよい。
 */
class IAssetLoader {
public:
    /** 派生ローダを正しく破棄するための仮想デストラクタ。 */
    virtual ~IAssetLoader() noexcept = default;

    /**
     * このローダが生成するアセット型 ID を返す。
     *
     * @return 担当アセット型の AssetType。
     */
    virtual AssetType TypeId() const noexcept = 0;

    /**
     * このローダが担当する拡張子を返す ("dds", "png", "mesh" など)。
     *
     * @details レジストリがこの文字列でパスの拡張子とマッチングする ("*" は全拡張子フォールバック)。
     * @return 小文字 ASCII の拡張子文字列。
     */
    virtual const char* Extension() const noexcept = 0;

    /**
     * バイト列からアセットを同期生成する。
     *
     * @param id 生成したアセットに割り当てる ID。
     * @param bytes ファイル全体のバイト列。
     * @return 成功なら生成したアセット、失敗ならエラー。
     */
    virtual TResult<TSharedPtr<AAsset>> LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept = 0;
};

} // namespace acs
