// SPDX-License-Identifier: Apache-2.0
// 汎用バイナリアセット（バイト列をそのまま保持する最小実装）
//
// 専用ローダがない拡張子のフォールバック用、またはテストデータ用に使う。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "asset/Asset.h"
#include "asset/IAssetLoader.h"
#include "container/Hash.h"

namespace acs {

/**
 * バイト列をそのまま保持する汎用バイナリアセット。
 *
 * @details 専用ローダがない拡張子のフォールバック用、またはテストデータ用に使う最小実装。
 */
class ABinaryAsset : public AAsset {
public:
    ACS_ASSET_TYPE("FBinaryAsset")

    /** 空のバイナリアセットを構築する。 */
    ABinaryAsset() noexcept = default;

    /**
     * バイト列を所有してバイナリアセットを構築する。
     *
     * @param bytes 保持するバイト列 (ムーブで所有を奪う)。
     */
    explicit ABinaryAsset(TArray<byte>&& bytes) noexcept : m_Bytes(Move(bytes)) {}

    /**
     * 保持しているバイト列への const 参照を返す。
     *
     * @return バイト列への const 参照。
     */
    const TArray<byte>& Bytes() const noexcept { return m_Bytes; }

    /**
     * 保持しているバイト列への可変参照を返す。
     *
     * @return バイト列への参照。
     */
    TArray<byte>&       Bytes()       noexcept { return m_Bytes; }

private:
    /** 保持する生バイト列。 */
    TArray<byte> m_Bytes;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FBinaryAsset = ABinaryAsset;

/**
 * 任意の拡張子を担当する拡張子フォールバックローダ。
 *
 * @details Extension() が "*" を返し、専用ローダが見つからないアセットを ABinaryAsset として読み込む。
 */
class CBinaryAssetLoader final : public IAssetLoader {
public:
    /**
     * 生成するアセット型 ID を返す。
     *
     * @return ABinaryAsset の型 ID。
     */
    AssetType   TypeId()    const noexcept override { return ABinaryAsset::StaticType(); }

    /**
     * 担当拡張子を返す。
     *
     * @return "*" (全拡張子のフォールバック)。
     */
    const char* Extension() const noexcept override { return "*"; }

    /**
     * バイト列をそのまま保持する ABinaryAsset を生成する。
     *
     * @param id 生成アセットに割り当てる ID。
     * @param bytes ファイル全体のバイト列。
     * @return 成功なら ABinaryAsset、失敗ならエラー。
     */
    TResult<TSharedPtr<AAsset>> LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept override;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FBinaryAssetLoader = CBinaryAssetLoader;

} // namespace acs
