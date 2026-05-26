// SPDX-License-Identifier: Apache-2.0
// フォント実装（stb_truetype による TTF パース + アトラス焼き込み）
#include "render/Font.h"
#include "platform/FileSystem.h"
#include "memory/Memory.h"
#include "memory/Allocator.h"
#include "container/Array.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

// stb_truetype は単一ヘッダ。実装は ImageAsset.cpp で STB_IMAGE_IMPLEMENTATION
// しているが、stb_truetype は別マクロなので衝突しない。
#define STB_TRUETYPE_IMPLEMENTATION
#define STB_RECT_PACK_IMPLEMENTATION
#include <stb_rect_pack.h>
#include <stb_truetype.h>

#include <cstring>

namespace acs {

namespace {

// 焼き込むコードポイント範囲（first, count）
struct CpRange {
    u32 first;
    u32 count;
};

// デフォルト範囲: ASCII / Latin-1 / かな / 全角句読点・英数
constexpr CpRange kDefaultRanges[] = {
    { 0x0020, 0x007F - 0x0020 },   // ASCII (95)
    { 0x00A0, 0x0100 - 0x00A0 },   // Latin-1 補足 (96)
    { 0x3000, 0x3040 - 0x3000 },   // CJK 記号・句読点 (64)
    { 0x3040, 0x30A0 - 0x3040 },   // 平仮名 (96)
    { 0x30A0, 0x3100 - 0x30A0 },   // 片仮名 (96)
    { 0xFF01, 0xFF5F - 0xFF01 },   // 全角英数記号 (94)
};
constexpr u32 kNumDefaultRanges = sizeof(kDefaultRanges) / sizeof(kDefaultRanges[0]);

// 追加範囲: CJK 統合漢字（include_cjk=true で焼く）
constexpr CpRange kCjkRange = { 0x4E00, 0x9FB0 - 0x4E00 };  // 約 20,400 字

} // namespace

u32 DecodeUtf8(const char** p) noexcept {
    if (!p || !*p) return 0;
    const u8* s = reinterpret_cast<const u8*>(*p);
    if (*s == 0) return 0;
    u32 c = 0;
    if ((*s & 0x80) == 0) {
        c = *s++;
    } else if ((*s & 0xE0) == 0xC0) {
        c  = static_cast<u32>(*s++ & 0x1F) << 6;
        if ((*s & 0xC0) != 0x80) { *p = reinterpret_cast<const char*>(s); return 0xFFFD; }
        c |= static_cast<u32>(*s++ & 0x3F);
    } else if ((*s & 0xF0) == 0xE0) {
        c  = static_cast<u32>(*s++ & 0x0F) << 12;
        if ((*s & 0xC0) != 0x80) { *p = reinterpret_cast<const char*>(s); return 0xFFFD; }
        c |= static_cast<u32>(*s++ & 0x3F) << 6;
        if ((*s & 0xC0) != 0x80) { *p = reinterpret_cast<const char*>(s); return 0xFFFD; }
        c |= static_cast<u32>(*s++ & 0x3F);
    } else if ((*s & 0xF8) == 0xF0) {
        c  = static_cast<u32>(*s++ & 0x07) << 18;
        if ((*s & 0xC0) != 0x80) { *p = reinterpret_cast<const char*>(s); return 0xFFFD; }
        c |= static_cast<u32>(*s++ & 0x3F) << 12;
        if ((*s & 0xC0) != 0x80) { *p = reinterpret_cast<const char*>(s); return 0xFFFD; }
        c |= static_cast<u32>(*s++ & 0x3F) << 6;
        if ((*s & 0xC0) != 0x80) { *p = reinterpret_cast<const char*>(s); return 0xFFFD; }
        c |= static_cast<u32>(*s++ & 0x3F);
    } else {
        ++s;  // skip bad byte
        c = 0xFFFD;
    }
    *p = reinterpret_cast<const char*>(s);
    return c;
}

TResult<void> FFont::LoadFromBytes(IRhiDevice& device, const u8* ttf_data, usize ttf_size,
                                 f32 pixel_size, u32 atlas_size, bool include_cjk) noexcept {
    if (!ttf_data || ttf_size == 0)
        return ACS_ERR(FAsset, 200, "FFont: empty TTF data");
    if (atlas_size == 0) atlas_size = 1024;
    // CJK を含むなら 2048 未満は自動で 2048 へ
    if (include_cjk && atlas_size < 2048) atlas_size = 2048;
    _pixel_size = pixel_size;
    _atlas_size = atlas_size;

    // stb_truetype で font 情報を取得
    stbtt_fontinfo info{};
    if (!stbtt_InitFont(&info, ttf_data, stbtt_GetFontOffsetForIndex(ttf_data, 0))) {
        return ACS_ERR(FAsset, 201, "FFont: stbtt_InitFont failed");
    }
    const f32 scale = stbtt_ScaleForPixelHeight(&info, pixel_size);
    int ascent = 0, descent = 0, line_gap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);
    _ascent   = ascent   * scale;
    _descent  = descent  * scale;
    _line_gap = line_gap * scale;

    // ===== アトラス用ピクセル（R8 single-channel）=====
    const usize atlas_bytes = static_cast<usize>(atlas_size) * atlas_size;
    u8* atlas_r8 = static_cast<u8*>(DefaultAllocator().Alloc(atlas_bytes));
    if (!atlas_r8) return ACS_ERR(Memory, 220, "FFont: atlas alloc");
    ::memset(atlas_r8, 0, atlas_bytes);

    // ===== stbtt_pack で複数コードポイント範囲を一気に焼く =====
    stbtt_pack_context spc{};
    if (!stbtt_PackBegin(&spc, atlas_r8,
                          static_cast<int>(atlas_size), static_cast<int>(atlas_size),
                          0 /* stride = w */, 1 /* padding */, nullptr)) {
        DefaultAllocator().Free(atlas_r8);
        return ACS_ERR(FAsset, 202, "FFont: stbtt_PackBegin failed");
    }
    stbtt_PackSetOversampling(&spc, 1, 1);

    // 範囲リストを動的構築（デフォルト + 任意で CJK）
    TArray<CpRange> active_ranges;
    active_ranges.Reserve(kNumDefaultRanges + 1);
    for (u32 i = 0; i < kNumDefaultRanges; ++i) {
        active_ranges.PushBack(kDefaultRanges[i]);
    }
    if (include_cjk) {
        active_ranges.PushBack(kCjkRange);
    }
    const u32 num_active = static_cast<u32>(active_ranges.Size());

    // 各範囲ぶん stbtt_packedchar を確保
    TArray<TArray<stbtt_packedchar>> packed_per_range;
    packed_per_range.Resize(num_active);
    TArray<stbtt_pack_range> ranges;
    ranges.Resize(num_active);
    for (u32 i = 0; i < num_active; ++i) {
        packed_per_range[i].Resize(active_ranges[i].count);
        stbtt_pack_range& pr = ranges[i];
        pr = stbtt_pack_range{};
        pr.font_size                       = pixel_size;
        pr.first_unicode_codepoint_in_range = static_cast<int>(active_ranges[i].first);
        pr.array_of_unicode_codepoints    = nullptr;
        pr.num_chars                       = static_cast<int>(active_ranges[i].count);
        pr.chardata_for_range              = packed_per_range[i].Data();
    }
    // PackFontRanges は失敗しても部分的に成功している場合があるので結果は無視
    (void)stbtt_PackFontRanges(&spc, ttf_data, 0, ranges.Data(), static_cast<int>(num_active));
    stbtt_PackEnd(&spc);

    // ===== R8 → RGBA8 変換（アルファのみ、RGB は白）=====
    const usize rgba_bytes = atlas_bytes * 4;
    u8* atlas_rgba = static_cast<u8*>(DefaultAllocator().Alloc(rgba_bytes));
    if (!atlas_rgba) {
        DefaultAllocator().Free(atlas_r8);
        return ACS_ERR(Memory, 221, "FFont: atlas RGBA alloc");
    }
    for (usize i = 0; i < atlas_bytes; ++i) {
        atlas_rgba[i*4 + 0] = 255;
        atlas_rgba[i*4 + 1] = 255;
        atlas_rgba[i*4 + 2] = 255;
        atlas_rgba[i*4 + 3] = atlas_r8[i];
    }
    DefaultAllocator().Free(atlas_r8);

    // ===== GPU テクスチャ作成 =====
    FTextureDesc td{};
    td.width = atlas_size; td.height = atlas_size;
    td.format = EFormat::R8G8B8A8_UNorm;
    td.initial_data = atlas_rgba;
    td.initial_data_size = rgba_bytes;
    auto tr = CreateRhiTexture(device, td);
    DefaultAllocator().Free(atlas_rgba);
    if (tr.IsErr()) return Err<void>(tr.Error());
    _atlas = Move(tr.Value());

    // ===== グリフマップ構築 =====
    const f32 inv_size = 1.0f / static_cast<f32>(atlas_size);
    for (u32 ri = 0; ri < num_active; ++ri) {
        const stbtt_packedchar* pcs = packed_per_range[ri].Data();
        for (u32 i = 0; i < active_ranges[ri].count; ++i) {
            const stbtt_packedchar& pc = pcs[i];
            // 焼き込み失敗グリフ (xadvance=0 等) はスキップ
            if (pc.xadvance == 0 && pc.x0 == pc.x1) continue;
            FGlyphInfo g{};
            g.u0 = pc.x0 * inv_size;
            g.v0 = pc.y0 * inv_size;
            g.u1 = pc.x1 * inv_size;
            g.v1 = pc.y1 * inv_size;
            g.width   = static_cast<f32>(pc.x1 - pc.x0);
            g.height  = static_cast<f32>(pc.y1 - pc.y0);
            g.x_offset = pc.xoff;
            g.y_offset = pc.yoff;
            g.x_advance = pc.xadvance;
            const u32 cp = active_ranges[ri].first + i;
            _glyphs.Insert(cp, g);
        }
    }

    return Ok();
}

TResult<void> FFont::LoadFromFile(IRhiDevice& device, const wchar_t* path,
                                f32 pixel_size, u32 atlas_size, bool include_cjk) noexcept {
    auto bytes_r = FFileSystem::ReadAllBytes(path);
    if (bytes_r.IsErr()) return Err<void>(bytes_r.Error());
    const TArray<byte>& bytes = bytes_r.Value();
    return LoadFromBytes(device, reinterpret_cast<const u8*>(bytes.Data()),
                         bytes.Size(), pixel_size, atlas_size, include_cjk);
}

void FFont::Shutdown() noexcept {
    _atlas.Reset();
    _glyphs.Clear();
    _atlas_size = 0;
    _pixel_size = 0;
}

bool FFont::GetGlyph(u32 codepoint, FGlyphInfo& out) const noexcept {
    const FGlyphInfo* hit = _glyphs.Find(codepoint);
    if (!hit) return false;
    out = *hit;
    return true;
}

f32 FFont::MeasureWidth(const char* utf8_text) const noexcept {
    if (!utf8_text) return 0.0f;
    f32 w = 0.0f;
    const char* p = utf8_text;
    while (true) {
        u32 cp = DecodeUtf8(&p);
        if (cp == 0) break;
        if (cp == '\n') continue;
        FGlyphInfo g{};
        if (GetGlyph(cp, g)) w += g.x_advance;
    }
    return w;
}

} // namespace acs
