// SPDX-License-Identifier: Apache-2.0
// 2D スプライト描画ヘルパ実装
#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "memory/Memory.h"
#include "memory/Allocator.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

#include <cstring>
#include <cmath>

namespace acs {

namespace {

// HLSL: ピクセル座標 → NDC 変換（cb で screen サイズを受け取る）
const char* kSpriteHLSL = R"(
cbuffer Screen : register(b0) {
    float4 inv_screen;   // (1/w, 1/h, w, h)
    float4 view;         // (cam_x, cam_y, zoom, _)
};

struct VSIn {
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR;
};
struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR;
};

VSOut VSMain(VSIn v) {
    VSOut o;
    // ワールド座標にカメラ（中心 view.xy、ズーム view.z）を適用 → ピクセル → NDC
    float2 screen_half = float2(inv_screen.z, inv_screen.w) * 0.5;
    float2 sp = (v.pos - view.xy) * view.z + screen_half;
    float2 ndc;
    ndc.x = sp.x * inv_screen.x * 2.0 - 1.0;
    ndc.y = 1.0 - sp.y * inv_screen.y * 2.0;
    o.pos = float4(ndc, 0.0, 1.0);
    o.uv  = v.uv;
    o.col = v.col;
    return o;
}

Texture2D    atlas : register(t0);
// 命名規約: <texture>_sampler
SamplerState atlas_sampler : register(s0);

float4 PSMain(VSOut v) : SV_TARGET {
    return atlas.Sample(atlas_sampler, v.uv) * v.col;
}
)";

} // namespace

Result<void> SpriteBatch::Init(IRhiDevice& device, EFormat rt_format, u32 max_sprites) noexcept {
    if (max_sprites == 0) max_sprites = 4096;
    _max_sprites = max_sprites;

    // === シェーダ ===
    ShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kSpriteHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "Sprite.VS";
    auto vs_r = CreateRhiShader(device, vs_d);
    if (vs_r.IsErr()) return Err<void>(vs_r.Error());
    _vs = Move(vs_r.Value());

    ShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kSpriteHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "Sprite.PS";
    auto ps_r = CreateRhiShader(device, ps_d);
    if (ps_r.IsErr()) return Err<void>(ps_r.Error());
    _ps = Move(ps_r.Value());

    // === 動的頂点バッファ（4 頂点 / sprite）===
    const usize vb_size = sizeof(Vertex) * 4 * max_sprites;
    BufferDesc vbd{};
    vbd.size = vb_size;
    vbd.usage = EBufferUsage::Vertex;
    vbd.cpu_writable = true;       // 自動で frame-cycled になる
    auto vb_r = CreateRhiBuffer(device, vbd);
    if (vb_r.IsErr()) return Err<void>(vb_r.Error());
    _vb = Move(vb_r.Value());

    // CPU 側のステージング配列（各フレームここに頂点を積んでから VB へコピー）
    _vertex_cpu = static_cast<Vertex*>(DefaultAllocator().Alloc(vb_size));
    if (!_vertex_cpu) return ACS_ERR(Memory, 250, "SpriteBatch: vertex stage alloc");

    // === インデックスバッファ（quad ごとに 6 indices、固定）===
    const u32 idx_count = max_sprites * 6;
    u16* idx_ptr = static_cast<u16*>(
        DefaultAllocator().Alloc(sizeof(u16) * idx_count));
    if (!idx_ptr) return ACS_ERR(Memory, 251, "SpriteBatch: index alloc");
    for (u32 i = 0; i < max_sprites; ++i) {
        const u16 base = static_cast<u16>(i * 4);
        idx_ptr[i*6 + 0] = base + 0;
        idx_ptr[i*6 + 1] = base + 1;
        idx_ptr[i*6 + 2] = base + 2;
        idx_ptr[i*6 + 3] = base + 0;
        idx_ptr[i*6 + 4] = base + 2;
        idx_ptr[i*6 + 5] = base + 3;
    }
    BufferDesc ibd{};
    ibd.size = sizeof(u16) * idx_count;
    ibd.usage = EBufferUsage::Index16;
    ibd.cpu_writable = true;
    ibd.initial_data = idx_ptr;
    auto ib_r = CreateRhiBuffer(device, ibd);
    DefaultAllocator().Free(idx_ptr);
    if (ib_r.IsErr()) return Err<void>(ib_r.Error());
    _ib = Move(ib_r.Value());

    // === 定数バッファ（screen size）===
    BufferDesc cbd{};
    cbd.size = 256;
    cbd.usage = EBufferUsage::Uniform;
    cbd.cpu_writable = true;
    auto cb_r = CreateRhiBuffer(device, cbd);
    if (cb_r.IsErr()) return Err<void>(cb_r.Error());
    _cb = Move(cb_r.Value());

    // === 1×1 白テクスチャ（DrawRect 用、矩形には常にこれを bind）===
    const u8 white_pixel[4] = { 255, 255, 255, 255 };
    TextureDesc td{};
    td.width = 1; td.height = 1;
    td.format = EFormat::R8G8B8A8_UNorm;
    td.initial_data = white_pixel;
    td.initial_data_size = 4;
    auto wt_r = CreateRhiTexture(device, td);
    if (wt_r.IsErr()) return Err<void>(wt_r.Error());
    _white = Move(wt_r.Value());

    // === パイプライン（α ブレンド有効、深度無し、カリング無し）===
    PipelineDesc pd{};
    pd.vs = _vs.Get();
    pd.ps = _ps.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = rt_format;
    pd.depth_format  = EFormat::Unknown;   // 2D は深度無し
    pd.depth_test    = false;
    pd.cull_mode     = ECullMode::None;
    pd.blend_mode    = EBlendMode::AlphaBlend;
    pd.cbuffer_slots = 1;
    pd.texture_slots = 1;
    pd.cbuffer_names[0] = "Screen";
    pd.texture_names[0] = "atlas";
    pd.static_sampler_count = 1;
    pd.static_samplers[0].filter    = ESamplerFilter::Linear;
    pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
    pd.vertex_stride = sizeof(Vertex);
    pd.layout[0] = { "POSITION", 0, EFormat::R32G32_Float,    0  };
    pd.layout[1] = { "TEXCOORD", 0, EFormat::R32G32_Float,    8  };
    pd.layout[2] = { "COLOR",    0, EFormat::R32G32B32A32_Float, 16 };
    pd.layout_count = 3;
    auto pl_r = CreateRhiPipeline(device, pd);
    if (pl_r.IsErr()) return Err<void>(pl_r.Error());
    _pipeline = Move(pl_r.Value());

    return Ok();
}

void SpriteBatch::Shutdown() noexcept {
    _pipeline.Reset();
    _white.Reset();
    _cb.Reset();
    _ib.Reset();
    _vb.Reset();
    _ps.Reset();
    _vs.Reset();
    if (_vertex_cpu) {
        DefaultAllocator().Free(_vertex_cpu);
        _vertex_cpu = nullptr;
    }
}

void SpriteBatch::Begin(IRhiCommandList& cl, u32 screen_w, u32 screen_h) noexcept {
    _cl = &cl;
    _screen_w = screen_w == 0 ? 1 : screen_w;
    _screen_h = screen_h == 0 ? 1 : screen_h;
    _sprite_count = 0;
    _flushed_count = 0;
    _current_tex = nullptr;

    // ビューを恒等（カメラ無し）に戻し、定数バッファを更新
    _view_x    = static_cast<f32>(_screen_w) * 0.5f;
    _view_y    = static_cast<f32>(_screen_h) * 0.5f;
    _view_zoom = 1.0f;
    WriteScreenCBuffer();

    // パイプラインと共通リソースを bind
    cl.SetPipeline(*_pipeline);
    cl.SetConstantBuffer(0, *_cb);
    cl.SetVertexBuffer(*_vb, sizeof(Vertex));
    cl.SetIndexBuffer(*_ib);
}

void SpriteBatch::Draw(IRhiTexture& tex, f32 x, f32 y, f32 w, f32 h, Vec4 tint) noexcept {
    DrawSub(tex, x, y, w, h, 0, 0, 1, 1, tint);
}

void SpriteBatch::DrawSub(IRhiTexture& tex,
                          f32 x, f32 y, f32 w, f32 h,
                          f32 u0, f32 v0, f32 u1, f32 v1,
                          Vec4 tint) noexcept {
    if (!_cl) return;
    // テクスチャ切替でフラッシュ（同じテクスチャは累積）
    if (_current_tex && _current_tex != &tex) Flush();
    // 容量上限に達したら以降は無視（max_sprites を Init 時に増やす）
    if (_sprite_count >= _max_sprites) return;
    _current_tex = &tex;

    Vertex* v = _vertex_cpu + _sprite_count * 4;
    // 4 頂点を時計回りに積む: (x,y), (x+w,y), (x+w,y+h), (x,y+h)
    v[0] = { x,     y,     u0, v0, tint.x, tint.y, tint.z, tint.w };
    v[1] = { x+w,   y,     u1, v0, tint.x, tint.y, tint.z, tint.w };
    v[2] = { x+w,   y+h,   u1, v1, tint.x, tint.y, tint.z, tint.w };
    v[3] = { x,     y+h,   u0, v1, tint.x, tint.y, tint.z, tint.w };
    ++_sprite_count;
}

void SpriteBatch::DrawRect(f32 x, f32 y, f32 w, f32 h, Vec4 color) noexcept {
    DrawSub(*_white, x, y, w, h, 0, 0, 1, 1, color);
}

void SpriteBatch::DrawRotated(IRhiTexture& tex,
                              f32 cx, f32 cy, f32 w, f32 h, f32 radians,
                              f32 u0, f32 v0, f32 u1, f32 v1,
                              Vec4 tint) noexcept {
    if (!_cl) return;
    if (_current_tex && _current_tex != &tex) Flush();
    if (_sprite_count >= _max_sprites) return;
    _current_tex = &tex;

    const f32 s  = ::sinf(radians);
    const f32 c  = ::cosf(radians);
    const f32 hw = w * 0.5f;
    const f32 hh = h * 0.5f;
    // ローカル 4 隅 (TL, TR, BR, BL) を中心まわりに回転。DrawSub と同じ頂点順。
    const f32 lx[4] = { -hw,  hw,  hw, -hw };
    const f32 ly[4] = { -hh, -hh,  hh,  hh };
    const f32 uu[4] = {  u0,  u1,  u1,  u0 };
    const f32 vv[4] = {  v0,  v0,  v1,  v1 };

    Vertex* vtx = _vertex_cpu + _sprite_count * 4;
    for (int i = 0; i < 4; ++i) {
        const f32 px = cx + lx[i] * c - ly[i] * s;
        const f32 py = cy + lx[i] * s + ly[i] * c;
        vtx[i] = { px, py, uu[i], vv[i], tint.x, tint.y, tint.z, tint.w };
    }
    ++_sprite_count;
}

void SpriteBatch::DrawRectRotated(f32 cx, f32 cy, f32 w, f32 h, f32 radians,
                                  Vec4 color) noexcept {
    DrawRotated(*_white, cx, cy, w, h, radians, 0, 0, 1, 1, color);
}

void SpriteBatch::WriteScreenCBuffer() noexcept {
    const f32 sw = static_cast<f32>(_screen_w);
    const f32 sh = static_cast<f32>(_screen_h);
    f32 cb[8] = {
        1.0f / sw, 1.0f / sh, sw, sh,
        _view_x, _view_y, _view_zoom, 0.0f,
    };
    _cb->Update(cb, sizeof(cb));
}

void SpriteBatch::SetView(f32 cam_x, f32 cam_y, f32 zoom) noexcept {
    if (!_cl) return;
    Flush();   // 既存バッチを現在のビューで確定してから切り替える
    _view_x    = cam_x;
    _view_y    = cam_y;
    _view_zoom = (zoom > 0.0001f) ? zoom : 1.0f;
    WriteScreenCBuffer();
}

void SpriteBatch::SetClipRect(i32 x, i32 y, i32 w, i32 h) noexcept {
    if (!_cl) return;
    Flush();   // クリップ変更前のバッチを確定
    ScissorRect sr{ x, y, x + w, y + h };
    _cl->SetScissor(sr);
}

void SpriteBatch::ClearClipRect() noexcept {
    if (!_cl) return;
    Flush();
    ScissorRect sr{ 0, 0, static_cast<i32>(_screen_w), static_cast<i32>(_screen_h) };
    _cl->SetScissor(sr);
}

void SpriteBatch::DrawTriangle(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2,
                               Vec4 color) noexcept {
    if (!_cl) return;
    if (_current_tex && _current_tex != _white.Get()) Flush();
    if (_sprite_count >= _max_sprites) return;
    _current_tex = _white.Get();
    // 4 頂点目に 3 頂点目を重ねる。インデックス (0,2,3) の三角形は面積 0 に
    // 退化して描画されず、(0,1,2) の三角形だけが塗られる。
    Vertex* v = _vertex_cpu + _sprite_count * 4;
    v[0] = { x0, y0, 0, 0, color.x, color.y, color.z, color.w };
    v[1] = { x1, y1, 0, 0, color.x, color.y, color.z, color.w };
    v[2] = { x2, y2, 0, 0, color.x, color.y, color.z, color.w };
    v[3] = { x2, y2, 0, 0, color.x, color.y, color.z, color.w };
    ++_sprite_count;
}

void SpriteBatch::DrawString(const Font& font, const char* utf8_text,
                           f32 x, f32 y, Vec4 color) noexcept {
    if (!utf8_text || !font.AtlasTexture()) return;
    IRhiTexture* atlas = font.AtlasTexture();

    // (x, y) は行の左上。ベースラインは y + ascent。
    f32 pen_x    = x;
    f32 baseline = y + font.Ascent();

    const char* p = utf8_text;
    while (true) {
        u32 cp = DecodeUtf8(&p);
        if (cp == 0) break;
        if (cp == '\n') {
            pen_x    = x;
            baseline += font.LineHeight();
            continue;
        }
        GlyphInfo g{};
        if (!font.GetGlyph(cp, g)) continue;
        // packedchar の x_offset/y_offset はベースライン基準
        const f32 qx = pen_x    + g.x_offset;
        const f32 qy = baseline + g.y_offset;
        DrawSub(*atlas, qx, qy, g.width, g.height,
                g.u0, g.v0, g.u1, g.v1, color);
        pen_x += g.x_advance;
    }
}

void SpriteBatch::End() noexcept {
    Flush();
    _cl = nullptr;
}

void SpriteBatch::Flush() noexcept {
    if (!_cl || !_current_tex) return;
    if (_sprite_count <= _flushed_count) return;

    // 既にフラッシュ済みのスプライトの後ろに、新しい範囲だけを VB に部分書き込みする。
    // こうすることで、先行投入済みの DrawIndexed が参照する範囲を上書きしない。
    const u32   first_sprite = _flushed_count;
    const u32   count        = _sprite_count - _flushed_count;
    const usize byte_offset  = static_cast<usize>(first_sprite) * 4 * sizeof(Vertex);
    const usize byte_size    = static_cast<usize>(count) * 4 * sizeof(Vertex);
    _vb->Update(_vertex_cpu + first_sprite * 4, byte_size, byte_offset);

    _cl->SetTexture(0, *_current_tex);
    _cl->DrawIndexed(count * 6, first_sprite * 6, 0);

    _flushed_count = _sprite_count;
    _current_tex   = nullptr;
}

void SpriteBatch::EnsurePipeline() noexcept {
    // 現状未使用。RT フォーマットが動的に変わる場合のリビルドフックの placeholder
}

} // namespace acs
