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

// 1 グリフ分の情報（アトラス UV + 描画オフセット）
struct GlyphInfo {
    f32 u0 = 0, v0 = 0, u1 = 0, v1 = 0;       // アトラス内 UV (0..1)
    f32 width  = 0, height = 0;                // 描画矩形のサイズ（ピクセル）
    f32 x_offset = 0, y_offset = 0;            // ペン位置からの矩形左上オフセット
    f32 x_advance = 0;                          // 描画後にペンを進める量
};

class Font {
public:
    Font() noexcept = default;
    ~Font() noexcept = default;

    Font(const Font&)            = delete;
    Font& operator=(const Font&) = delete;

    // TTF/OTF/TTC バイト列からアトラスを構築
    // include_cjk=true で漢字 (CJK 統合 U+4E00..U+9FAF) も含めて焼く。
    // その場合アトラスは自動で 2048 にバンプされる。
    TResult<void> LoadFromBytes(IRhiDevice& device,
                               const u8* ttf_data,
                               usize ttf_size,
                               f32 pixel_size,
                               u32 atlas_size = 1024,
                               bool include_cjk = false) noexcept;

    // ファイルから直接ロード（FileSystem::ReadAllBytes 経由）
    TResult<void> LoadFromFile(IRhiDevice& device,
                              const wchar_t* path,
                              f32 pixel_size,
                              u32 atlas_size = 1024,
                              bool include_cjk = false) noexcept;

    void Shutdown() noexcept;

    IRhiTexture* AtlasTexture() const noexcept { return m_Atlas.Get(); }
    u32          AtlasSize()    const noexcept { return m_AtlasSize; }
    f32          PixelSize()    const noexcept { return m_PixelSize; }

    // フォント全体のメトリクス（ピクセル単位）
    f32 Ascent()     const noexcept { return m_Ascent; }
    f32 Descent()    const noexcept { return m_Descent; }    // 通常負値
    f32 LineGap()    const noexcept { return m_LineGap; }
    f32 LineHeight() const noexcept { return m_Ascent - m_Descent + m_LineGap; }

    // コードポイントのグリフ情報を取得（アトラス未収録なら false）
    bool GetGlyph(u32 codepoint, GlyphInfo& out) const noexcept;

    // 文字列の幅をピクセルで測る（改行は無視）
    f32 MeasureWidth(const char* utf8_text) const noexcept;

private:
    TUniquePtr<IRhiTexture>    m_Atlas;
    THashMap<u32, GlyphInfo>   m_Glyphs;
    u32                       m_AtlasSize = 0;
    f32                       m_PixelSize = 0;
    f32                       m_Ascent     = 0;
    f32                       m_Descent    = 0;
    f32                       m_LineGap   = 0;
};

// UTF-8 デコード: *p から 1 文字ぶん読み、コードポイントを返す。
// *p は次の文字位置に進む。終端 / 不正バイト時は 0 を返す。
u32 DecodeUtf8(const char** p) noexcept;

} // namespace acs
