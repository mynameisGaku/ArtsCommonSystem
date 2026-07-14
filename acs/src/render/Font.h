// SPDX-License-Identifier: Apache-2.0
// フォント（TTF を GPU テクスチャアトラスに焼いて、文字描画に使う）
//
// 使い方:
//   Font font;
//   font.LoadFromFile(*renderer.Device(), L"C:/Windows/Fonts/meiryo.ttc", 32.0f);
//
//   // 描画フレーム中
//   sb.Begin(*cl, sw, sh);
//   sb.DrawText(font, "ハロー、ワールド！", 100, 100, FVec4{1,1,1,1});
//   sb.End();
//
// 対応コードポイント (デフォルト):
//   - ASCII (U+0020 .. U+007E)
//   - Latin-1 補足 (U+00A0 .. U+00FF)
//   - 平仮名 (U+3040 .. U+309F)
//   - 片仮名 (U+30A0 .. U+30FF)
//   - 全角英数 (U+FF01 .. U+FF5E)
//   - 全角句読点・記号 (U+3000 .. U+303F)
//
// 漢字を使う場合は include_cjk=true を指定（CJK 統合漢字 U+4E00..U+9FAF）。
// アトラスサイズは自動で 2048×2048 に拡張される。
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "container/HashMap.h"
#include "render/IRhiDevice.h"
#include "render/IRhiTexture.h"

namespace acs {

/**
 * 1 グリフ分の情報 (アトラス UV + 描画オフセット)。
 *
 * @details
 * アトラス内の矩形 UV、描画矩形のピクセルサイズ、ペン位置からのオフセット、
 * および描画後にペンを進める量を保持する。
 */
struct GlyphInfo {
    /** アトラス内の矩形左上 U 座標 (0..1)。 */
    f32 u0 = 0;

    /** アトラス内の矩形左上 V 座標 (0..1)。 */
    f32 v0 = 0;

    /** アトラス内の矩形右下 U 座標 (0..1)。 */
    f32 u1 = 0;

    /** アトラス内の矩形右下 V 座標 (0..1)。 */
    f32 v1 = 0;

    /** 描画矩形の幅 (ピクセル)。 */
    f32 width  = 0;

    /** 描画矩形の高さ (ピクセル)。 */
    f32 height = 0;

    /** ペン位置から矩形左上までの X オフセット。 */
    f32 x_offset = 0;

    /** ペン位置から矩形左上までの Y オフセット。 */
    f32 y_offset = 0;

    /** 描画後にペンを進める量 (X 方向の advance)。 */
    f32 x_advance = 0;
};

/**
 * TTF を GPU テクスチャアトラスに焼いて文字描画に使うフォント。
 *
 * @details
 * LoadFromBytes / LoadFromFile で TTF/OTF/TTC からグリフアトラスを構築し、
 * GetGlyph で各コードポイントのアトラス UV・描画オフセットを引く。
 * GPU リソースを単独所有する non-copy 型。
 */
class Font {
public:
    /** 空状態で構築する (アトラスは Load* で構築)。 */
    Font() noexcept = default;

    /** 破棄する (GPU リソースは TUniquePtr が解放)。 */
    ~Font() noexcept = default;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    Font(const Font&)            = delete;

    /** コピー代入も禁止。 */
    Font& operator=(const Font&) = delete;

    /**
     * TTF/OTF/TTC バイト列からアトラスを構築する。
     *
     * @details
     * include_cjk=true で漢字 (CJK 統合 U+4E00..U+9FAF) も含めて焼く。その場合
     * アトラスは自動で 2048 にバンプされる。再ロードに失敗した場合は旧フォントを
     * 保持せず、アトラス・グリフ・全メトリクスを空状態へ戻す。
     * @param device テクスチャアトラス生成に使う RHI デバイス。
     * @param ttf_data TTF/OTF/TTC のバイト列先頭。
     * @param ttf_size ttf_data のバイト数。
     * @param pixel_size 焼き込むグリフのピクセルサイズ。
     * @param atlas_size アトラステクスチャの一辺サイズ (既定 1024)。
     * @param include_cjk true なら CJK 統合漢字も焼く (既定 false)。
     * @return 成功なら空の TResult、構築失敗ならエラー。
     */
    TResult<void> LoadFromBytes(IRhiDevice& device,
                               const u8* ttf_data,
                               usize ttf_size,
                               f32 pixel_size,
                               u32 atlas_size = 1024,
                               bool include_cjk = false) noexcept;

    /**
     * ファイルから直接ロードしてアトラスを構築する。
     *
     * @details FileSystem::ReadAllBytes でバイト列を読み込み LoadFromBytes に委譲する。
     * @param device テクスチャアトラス生成に使う RHI デバイス。
     * @param path 読み込む TTF/OTF/TTC のファイルパス。
     * @param pixel_size 焼き込むグリフのピクセルサイズ。
     * @param atlas_size アトラステクスチャの一辺サイズ (既定 1024)。
     * @param include_cjk true なら CJK 統合漢字も焼く (既定 false)。
     * @return 成功なら空の TResult、読み込み/構築失敗ならエラー。
     */
    TResult<void> LoadFromFile(IRhiDevice& device,
                              const wchar_t* path,
                              f32 pixel_size,
                              u32 atlas_size = 1024,
                              bool include_cjk = false) noexcept;

    /** 確保した GPU リソースとグリフ表を解放し、全メトリクスを 0 に戻す。 */
    void Shutdown() noexcept;

    /**
     * グリフアトラステクスチャを返す。
     *
     * @return アトラステクスチャ (未ロードなら nullptr)。
     */
    IRhiTexture* AtlasTexture() const noexcept { return m_Atlas.Get(); }

    /**
     * アトラステクスチャの一辺サイズを返す。
     *
     * @return アトラスの一辺ピクセル数。
     */
    u32          AtlasSize()    const noexcept { return m_AtlasSize; }

    /**
     * 焼き込んだグリフのピクセルサイズを返す。
     *
     * @return Load* で指定した pixel_size。
     */
    f32          PixelSize()    const noexcept { return m_PixelSize; }

    /**
     * ベースラインから上端までの高さ (アセント) を返す。
     *
     * @return アセント (ピクセル)。
     */
    f32 Ascent()     const noexcept { return m_Ascent; }

    /**
     * ベースラインから下端までの高さ (ディセント) を返す。
     *
     * @return ディセント (ピクセル、通常負値)。
     */
    f32 Descent()    const noexcept { return m_Descent; }

    /**
     * 行間に追加するギャップを返す。
     *
     * @return ラインギャップ (ピクセル)。
     */
    f32 LineGap()    const noexcept { return m_LineGap; }

    /**
     * 1 行分の高さ (アセント - ディセント + ラインギャップ) を返す。
     *
     * @return 行の高さ (ピクセル)。
     */
    f32 LineHeight() const noexcept { return m_Ascent - m_Descent + m_LineGap; }

    /**
     * コードポイントのグリフ情報を取得する。
     *
     * @param codepoint 取得する Unicode コードポイント。
     * @param out 見つかったグリフ情報の格納先。
     * @return アトラスに収録されていれば true、未収録なら false。
     */
    bool GetGlyph(u32 codepoint, GlyphInfo& out) const noexcept;

    /**
     * UTF-8 文字列の描画幅をピクセルで測る (改行は無視)。
     *
     * @param utf8_text 計測する UTF-8 文字列 (nullptr で 0)。
     * @return 各グリフの x_advance の総和 (ピクセル)。
     */
    f32 MeasureWidth(const char* utf8_text) const noexcept;

private:
    /** グリフアトラステクスチャ (所有権を持つ)。 */
    TUniquePtr<IRhiTexture>    m_Atlas;

    /** コードポイント → グリフ情報の対応表。 */
    THashMap<u32, GlyphInfo>   m_Glyphs;

    /** アトラステクスチャの一辺サイズ。 */
    u32                       m_AtlasSize = 0;

    /** 焼き込んだグリフのピクセルサイズ。 */
    f32                       m_PixelSize = 0;

    /** ベースラインから上端までの高さ (アセント)。 */
    f32                       m_Ascent     = 0;

    /** ベースラインから下端までの高さ (ディセント、通常負値)。 */
    f32                       m_Descent    = 0;

    /** 行間に追加するラインギャップ。 */
    f32                       m_LineGap   = 0;
};

/**
 * UTF-8 文字列から 1 文字をデコードしてコードポイントを返す。
 *
 * @details
 * *p から 1 文字ぶん読み、*p を次の文字位置に進める。終端 (NUL) または p や *p が
 * null のときは 0 を返す。不正な継続バイトを検出した場合は U+FFFD (置換文字) を返す。
 * @param p デコード位置を指すポインタへのポインタ (デコード後に進められる)。
 * @return デコードしたコードポイント。終端で 0、不正バイトで 0xFFFD。
 */
u32 DecodeUtf8(const char** p) noexcept;

} // namespace acs
