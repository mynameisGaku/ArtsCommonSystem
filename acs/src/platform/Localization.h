// SPDX-License-Identifier: Apache-2.0
// 多言語対応（i18n）
//
// 設計:
//   - 各言語は INI 形式の Storage（key=value、UTF-8）
//   - active = 現在表示中の言語、fallback = キーが無い時の代替
//   - Storage を直接利用するので、ファイル形式 / 内部構造を学び直す必要なし
//
// 使い方:
//   Localization L;
//   L.LoadActive(L"lang/ja.lang");
//   L.LoadFallback(L"lang/en.lang");
//
//   FRenderer 内:
//     sb.DrawText(font, L.Tr("greeting"), 100, 100, color);
//
//   ja.lang 内容例:
//     # コメント
//     greeting=ハロー、世界！
//     menu.start=ゲーム開始
//     menu.exit=終了
//
//   en.lang 内容例:
//     greeting=Hello, World!
//     menu.start=Start FGame
//     menu.exit=Quit
//
// 言語切替:
//   L.LoadActive(L"lang/de.lang");   // 別言語に切替（fallback はそのまま）
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "platform/Storage.h"

namespace acs {

/**
 * 多言語対応 (i18n) のための翻訳テーブル。
 *
 * @details
 * active (現在表示中) と fallback (代替) の 2 つの Storage を持ち、Tr で active → fallback →
 * キー自体の順に解決する。各言語は INI 形式 (key=value、UTF-8) の Storage として読み込む。
 * コピー禁止。
 */
class Localization {
public:
    /** 空の翻訳テーブルを構築する。 */
    Localization() noexcept = default;

    /** コピー禁止 (内部 Storage が move-only のため)。 */
    Localization(const Localization&)            = delete;

    /** コピー代入も禁止。 */
    Localization& operator=(const Localization&) = delete;

    /**
     * 現在表示中の言語をファイルから読み込む。
     *
     * @param path 言語ファイル (INI 形式) のパス。
     * @return Storage::Load の結果。
     */
    TResult<void> LoadActive  (const wchar_t* path) noexcept { return m_Active.Load(path); }

    /**
     * 代替 (fallback) 言語をファイルから読み込む。
     *
     * @param path 言語ファイル (INI 形式) のパス。
     * @return Storage::Load の結果。
     */
    TResult<void> LoadFallback(const wchar_t* path) noexcept { return m_Fallback.Load(path); }

    /**
     * 現在表示中の言語をバイト列から読み込む (埋め込み用)。
     *
     * @param data INI テキストの先頭。
     * @param size data のバイト数。
     * @return Storage::LoadFromBytes の結果。
     */
    TResult<void> LoadActiveBytes  (const u8* data, usize size) noexcept { return m_Active.LoadFromBytes(data, size); }

    /**
     * 代替 (fallback) 言語をバイト列から読み込む (埋め込み用)。
     *
     * @param data INI テキストの先頭。
     * @param size data のバイト数。
     * @return Storage::LoadFromBytes の結果。
     */
    TResult<void> LoadFallbackBytes(const u8* data, usize size) noexcept { return m_Fallback.LoadFromBytes(data, size); }

    /** active と fallback を入れ替える (言語切替の便利関数)。 */
    void Swap() noexcept;

    /** active・fallback の全エントリを削除する。 */
    void Clear() noexcept { m_Active.Clear(); m_Fallback.Clear(); }

    /**
     * キーに対応する翻訳文字列を取得する。
     *
     * @details active → fallback → キー自体 (最後の手段) の順で探す。
     * @param key 翻訳キー (nullptr なら空文字列)。
     * @return 見つかった訳文、無ければ key 自体。
     */
    const char* Tr(const char* key) const noexcept;

    /**
     * キーが active か fallback のどちらかに存在するかを返す。
     *
     * @param key 確認するキー (nullptr は false)。
     * @return いずれかに存在すれば true。
     */
    bool Has(const char* key) const noexcept;

    /**
     * active 言語の Storage への可変参照を返す (独自加工用)。
     *
     * @return active Storage への参照。
     */
    Storage&       Active()         noexcept { return m_Active; }

    /**
     * active 言語の Storage への const 参照を返す。
     *
     * @return active Storage への const 参照。
     */
    const Storage& Active()   const noexcept { return m_Active; }

    /**
     * fallback 言語の Storage への可変参照を返す (独自加工用)。
     *
     * @return fallback Storage への参照。
     */
    Storage&       Fallback()       noexcept { return m_Fallback; }

    /**
     * fallback 言語の Storage への const 参照を返す。
     *
     * @return fallback Storage への const 参照。
     */
    const Storage& Fallback() const noexcept { return m_Fallback; }

private:
    /** 現在表示中の言語の翻訳テーブル。 */
    Storage m_Active;

    /** キーが active に無いときに参照する代替言語の翻訳テーブル。 */
    Storage m_Fallback;
};

} // namespace acs
