// SPDX-License-Identifier: Apache-2.0
// アセット基底クラス
//
// 全アセット型は Asset を継承する。Type() で実行時の型 ID を取得できる。
// テクスチャ・メッシュ・音声・スクリプトなどがすべてこの基底を共有する。
#pragma once

#include "foundation/Types.h"
#include "asset/AssetId.h"

namespace acs {

/** アセット型 ID (文字列ハッシュから生成、衝突しない前提)。 */
using AssetType = u32;

/** アセットのロード状態。 */
enum class EAssetState : u8 {
    /** 未ロード。 */
    Unloaded = 0,

    /** 非同期ロード中。 */
    Loading  = 1,

    /** ロード完了で利用可能。 */
    Ready    = 2,

    /** ロード失敗。 */
    Failed   = 3,
};

/**
 * 全アセット型が継承する共通基底クラス。
 *
 * @details
 * テクスチャ・メッシュ・音声・スクリプトなどがすべてこの基底を共有する。
 * Type() で実行時の型 ID を取得でき、Id()/State() でレジストリ管理用の
 * 識別子とロード状態を参照する。
 */
class Asset {
public:
    /** 派生クラスを正しく破棄するための仮想デストラクタ。 */
    virtual ~Asset() noexcept = default;

    /**
     * 派生クラスごとの実行時型 ID を返す。
     *
     * @return この派生型の AssetType。
     */
    virtual AssetType Type() const noexcept = 0;

    /**
     * アセット ID を返す。
     *
     * @return レジストリが割り当てた FAssetId (未設定なら kInvalidAssetId)。
     */
    FAssetId   Id()    const noexcept { return m_Id; }

    /**
     * 現在のロード状態を返す。
     *
     * @return EAssetState。
     */
    EAssetState State() const noexcept { return _state; }

    /**
     * アセット ID を設定する (レジストリ用、一般ユーザは触らない)。
     *
     * @param id 割り当てる FAssetId。
     */
    void SetId(FAssetId id) noexcept       { m_Id = id; }

    /**
     * ロード状態を設定する (レジストリ/ローダ用)。
     *
     * @param s 設定する EAssetState。
     */
    void SetState(EAssetState s) noexcept  { _state = s; }

private:
    /** レジストリが割り当てたアセット ID (default = 無効)。 */
    FAssetId    m_Id    = kInvalidAssetId;

    /** 現在のロード状態 (default = 未ロード)。 */
    EAssetState _state = EAssetState::Unloaded;
};

// 派生クラスで型 ID を簡単に宣言するためのヘルパマクロ
// 例:
//   class Texture : public Asset {
//   public:
//       ACS_ASSET_TYPE("Texture")
//       ...
//   };
#define ACS_ASSET_TYPE(name_literal)                                          \
    static ::acs::AssetType StaticType() noexcept {                           \
        static const ::acs::AssetType id =                                    \
            static_cast<::acs::AssetType>(::acs::HashBytes(name_literal,      \
                sizeof(name_literal) - 1));                                   \
        return id;                                                            \
    }                                                                         \
    ::acs::AssetType Type() const noexcept override { return StaticType(); }

} // namespace acs
