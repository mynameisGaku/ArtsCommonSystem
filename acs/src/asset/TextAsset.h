// SPDX-License-Identifier: Apache-2.0
// テキストアセット（UTF-8 文字列を保持）
//
// 対応拡張子: txt / json / xml / yaml / yml / toml / ini / csv / md / log / hlsl /
//          glsl / vert / frag / lua / py（任意のテキストファイル）
#pragma once

#include "container/Array.h"
#include "asset/Asset.h"
#include "asset/IAssetLoader.h"
#include "container/Hash.h"

namespace acs {

/**
 * UTF-8 テキスト文字列を保持するアセット。
 *
 * @details 内部バッファは末尾に NUL を含み、CStr() で NUL 終端文字列として参照できる。
 */
class ATextAsset : public AAsset {
public:
    ACS_ASSET_TYPE("FTextAsset")

    /** 空のテキストアセットを構築する。 */
    ATextAsset() noexcept = default;

    /**
     * 文字列バッファを所有してテキストアセットを構築する。
     *
     * @param text 末尾に NUL を含む文字列バッファ (ムーブで所有を奪う)。
     */
    explicit ATextAsset(TArray<char>&& text) noexcept : m_Text(Move(text)) {}

    /**
     * NUL 終端された文字列ポインタを返す。
     *
     * @return 文字列の先頭 (空なら "")。
     */
    const char* CStr() const noexcept { return m_Text.IsEmpty() ? "" : m_Text.Data(); }

    /**
     * NUL を除いた文字列長を返す。
     *
     * @return バイト数 (空なら 0)。
     */
    usize       Size() const noexcept { return m_Text.IsEmpty() ? 0 : m_Text.Size() - 1; }

    /**
     * 末尾 NUL を含む生バッファへの const 参照を返す。
     *
     * @return 文字列バッファへの const 参照。
     */
    const TArray<char>& Raw() const noexcept { return m_Text; }

private:
    /** 文字列データ (末尾に NUL を含む)。 */
    TArray<char> m_Text;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FTextAsset = ATextAsset;

/** 任意のテキストファイルを ATextAsset に読み込むローダ。 */
class CTextAssetLoader final : public IAssetLoader {
public:
    /**
     * 生成するアセット型 ID を返す。
     *
     * @return ATextAsset の型 ID。
     */
    AssetType   TypeId()    const noexcept override { return ATextAsset::StaticType(); }

    /**
     * 担当する主要拡張子を返す。
     *
     * @return "txt" (json/xml/lua などはレジストリが別名で登録する)。
     */
    const char* Extension() const noexcept override { return "txt"; }

    /**
     * バイト列を NUL 終端文字列としてコピーし ATextAsset を生成する。
     *
     * @param id 生成アセットに割り当てる ID。
     * @param bytes テキストファイル全体のバイト列。
     * @return 成功なら ATextAsset、失敗ならエラー。
     */
    TResult<TSharedPtr<AAsset>> LoadFromBytes(FAssetId id, const TArray<byte>& bytes) noexcept override;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FTextAssetLoader = CTextAssetLoader;

} // namespace acs
