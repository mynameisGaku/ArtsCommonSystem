// SPDX-License-Identifier: Apache-2.0
// アセット基底クラス
//
// 全アセット型は Asset を継承する。Type() で実行時の型 ID を取得できる。
// テクスチャ・メッシュ・音声・スクリプトなどがすべてこの基底を共有する。
#pragma once

#include "foundation/Types.h"
#include "asset/AssetId.h"

namespace acs {

// アセット型 ID（コンパイル時に文字列から生成、衝突しない前提）
using AssetType = u32;

// ロード状態
enum class EAssetState : u8 {
    Unloaded = 0,   // 未ロード
    Loading  = 1,   // ロード中（非同期）
    Ready    = 2,   // 利用可能
    Failed   = 3,   // ロード失敗
};

// 全アセット共通の基底
class Asset {
public:
    virtual ~Asset() noexcept = default;

    // 派生クラスごとの型 ID
    virtual AssetType Type() const noexcept = 0;

    FAssetId   Id()    const noexcept { return m_Id; }
    EAssetState State() const noexcept { return _state; }

    // レジストリから設定される（一般ユーザは触らない）
    void SetId(FAssetId id) noexcept       { m_Id = id; }
    void SetState(EAssetState s) noexcept  { _state = s; }

private:
    FAssetId    m_Id    = kInvalidAssetId;
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
