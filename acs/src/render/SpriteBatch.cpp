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
// 命名規約: <texture>m_Sampler
SamplerState atlas_sampler : register(s0);

float4 PSMain(VSOut v) : SV_TARGET {
    return atlas.Sample(atlas_sampler, v.uv) * v.col;
}
)";

} // namespace

TResult<void> FSpriteBatch::Init(IRhiDevice& device, EFormat rt_format, u32 max_sprites) noexcept {
    if (max_sprites == 0) max_sprites = 4096;
    m_MaxSprites = max_sprites;
    m_Device   = &device;     // ステンシル PSO の遅延生成で再利用
    m_RtFormat = rt_format;

    // === シェーダ ===
    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kSpriteHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "Sprite.VS";
    auto vs_r = CreateRhiShader(device, vs_d);
    if (vs_r.IsErr()) return Err<void>(vs_r.Error());
    m_Vs = Move(vs_r.Value());

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kSpriteHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "Sprite.PS";
    auto ps_r = CreateRhiShader(device, ps_d);
    if (ps_r.IsErr()) return Err<void>(ps_r.Error());
    m_Ps = Move(ps_r.Value());

    // === 動的頂点バッファ（4 頂点 / sprite）===
    const usize vb_size = sizeof(Vertex) * 4 * max_sprites;
    FBufferDesc vbd{};
    vbd.size = vb_size;
    vbd.usage = EBufferUsage::Vertex;
    vbd.cpu_writable = true;       // 自動で frame-cycled になる
    auto vb_r = CreateRhiBuffer(device, vbd);
    if (vb_r.IsErr()) return Err<void>(vb_r.Error());
    m_Vb = Move(vb_r.Value());

    // CPU 側のステージング配列（各フレームここに頂点を積んでから VB へコピー）
    m_VertexCpu = static_cast<Vertex*>(DefaultAllocator().Alloc(vb_size));
    if (!m_VertexCpu) return ACS_ERR(Memory, 250, "FSpriteBatch: vertex stage alloc");

    // === インデックスバッファ（quad ごとに 6 indices、固定）===
    const u32 idx_count = max_sprites * 6;
    u16* idx_ptr = static_cast<u16*>(
        DefaultAllocator().Alloc(sizeof(u16) * idx_count));
    if (!idx_ptr) return ACS_ERR(Memory, 251, "FSpriteBatch: index alloc");
    for (u32 i = 0; i < max_sprites; ++i) {
        const u16 base = static_cast<u16>(i * 4);
        idx_ptr[i*6 + 0] = base + 0;
        idx_ptr[i*6 + 1] = base + 1;
        idx_ptr[i*6 + 2] = base + 2;
        idx_ptr[i*6 + 3] = base + 0;
        idx_ptr[i*6 + 4] = base + 2;
        idx_ptr[i*6 + 5] = base + 3;
    }
    FBufferDesc ibd{};
    ibd.size = sizeof(u16) * idx_count;
    ibd.usage = EBufferUsage::Index16;
    ibd.cpu_writable = true;
    ibd.initial_data = idx_ptr;
    auto ib_r = CreateRhiBuffer(device, ibd);
    DefaultAllocator().Free(idx_ptr);
    if (ib_r.IsErr()) return Err<void>(ib_r.Error());
    m_Ib = Move(ib_r.Value());

    // === 定数バッファ（screen size + view）のリング ===
    // 1 個だとフレーム内の複数 view (world/HUD/反射) が同一アドレスを上書きし合い、
    // 先に積んだ draw が後の view を読んでしまう。view 切替ごとに別スロットを使う。
    for (u32 i = 0; i < kViewRing; ++i) {
        FBufferDesc cbd{};
        cbd.size = 256;
        cbd.usage = EBufferUsage::Uniform;
        cbd.cpu_writable = true;
        auto cb_r = CreateRhiBuffer(device, cbd);
        if (cb_r.IsErr()) return Err<void>(cb_r.Error());
        m_Cb[i] = Move(cb_r.Value());
    }
    m_CbCur = 0;

    // === 1×1 白テクスチャ（DrawRect 用、矩形には常にこれを bind）===
    const u8 white_pixel[4] = { 255, 255, 255, 255 };
    FTextureDesc td{};
    td.width = 1; td.height = 1;
    td.format = EFormat::R8G8B8A8_UNorm;
    td.initial_data = white_pixel;
    td.initial_data_size = 4;
    auto wt_r = CreateRhiTexture(device, td);
    if (wt_r.IsErr()) return Err<void>(wt_r.Error());
    m_White = Move(wt_r.Value());

    // === パイプライン（α ブレンド有効、深度無し、カリング無し）===
    FPipelineDesc pd{};
    FillCommonPipelineDesc(pd);
    pd.depth_format  = EFormat::Unknown;   // 2D は深度無し
    pd.depth_test    = false;
    auto pl_r = CreateRhiPipeline(device, pd);
    if (pl_r.IsErr()) return Err<void>(pl_r.Error());
    m_Pipeline = Move(pl_r.Value());

    return Ok();
}

// vs/ps/layout/blend/sampler/slot など、全 SpriteBatch PSO 共通の部分を埋める。
// depth_format / depth_test / stencil は呼び出し側がパス種別に応じて設定する。
void FSpriteBatch::FillCommonPipelineDesc(FPipelineDesc& pd) const noexcept {
    pd.vs = m_Vs.Get();
    pd.ps = m_Ps.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = m_RtFormat;
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
}

// ステンシル 4 モードの PSO を遅延生成する。すべて DSVFormat=D24S8 で、深度テスト/
// 書込みは無効 (2D)。StencilEnable と比較/操作だけがモードごとに異なる。
bool FSpriteBatch::EnsureStencilPipelines() noexcept {
    if (m_StencilReady) return true;
    if (!m_Device) return false;

    struct ModeCfg { bool enable; ECompareFunc func; EStencilOp pass; };
    const ModeCfg cfg[4] = {
        { false, ECompareFunc::Always,   EStencilOp::Keep    },  // Off
        { true,  ECompareFunc::Always,   EStencilOp::Replace },  // WriteMask
        { true,  ECompareFunc::Equal,    EStencilOp::Keep    },  // KeepInside
        { true,  ECompareFunc::NotEqual, EStencilOp::Keep    },  // KeepOutside
    };
    for (u32 i = 0; i < 4; ++i) {
        FPipelineDesc pd{};
        FillCommonPipelineDesc(pd);
        pd.depth_format = EFormat::D24_UNorm_S8_UInt;  // stencil 付き DSV と整合させる
        pd.depth_test   = false;
        pd.depth_write  = false;
        pd.stencil.enable     = cfg[i].enable;
        pd.stencil.func       = cfg[i].func;
        pd.stencil.pass_op    = cfg[i].pass;
        pd.stencil.fail_op    = EStencilOp::Keep;
        pd.stencil.depth_fail_op = EStencilOp::Keep;
        auto r = CreateRhiPipeline(*m_Device, pd);
        if (r.IsErr()) {
            ACS_LOG_ERROR("FSpriteBatch: ステンシル PSO %u の生成に失敗", i);
            return false;
        }
        m_StencilPipe[i] = Move(r.Value());
    }
    m_StencilReady = true;
    return true;
}

void FSpriteBatch::SetStencilMode(EStencilMode mode, u8 ref) noexcept {
    if (!m_Cl) return;
    Flush();                                  // 直前のバッチを現モードで確定
    if (!EnsureStencilPipelines()) return;
    IRhiPipeline* pl = m_StencilPipe[static_cast<u32>(mode)].Get();
    if (!pl) return;
    m_Cl->SetPipeline(*pl);
    m_Cl->SetStencilRef(ref);
    // PSO (= root signature) 切替で root 引数が無効化されるので CBV を貼り直す
    // (現 view バッファを bind = view は変えない)。IA (VB/IB) は PSO 切替で無効化
    // されないが、念のため再 bind してパリティを保つ。
    BindViewBuffer();
    m_Cl->SetVertexBuffer(*m_Vb, sizeof(Vertex));
    m_Cl->SetIndexBuffer(*m_Ib);
    m_StencilMode = mode;
}

void FSpriteBatch::Shutdown() noexcept {
    m_Pipeline.Reset();
    for (u32 i = 0; i < 4; ++i) m_StencilPipe[i].Reset();
    m_StencilReady = false;
    m_White.Reset();
    for (u32 i = 0; i < kViewRing; ++i) m_Cb[i].Reset();
    m_Ib.Reset();
    m_Vb.Reset();
    m_Ps.Reset();
    m_Vs.Reset();
    if (m_VertexCpu) {
        DefaultAllocator().Free(m_VertexCpu);
        m_VertexCpu = nullptr;
    }
}

void FSpriteBatch::Begin(IRhiCommandList& cl, u32 screen_w, u32 screen_h) noexcept {
    m_Cl = &cl;
    m_ScreenW = screen_w == 0 ? 1 : screen_w;
    m_ScreenH = screen_h == 0 ? 1 : screen_h;
    m_SpriteCount = 0;
    m_FlushedCount = 0;
    m_CurrentTex = nullptr;
    m_StencilMode = EStencilMode::Off;   // 既定パイプライン (m_Pipeline) で開始

    // ビューを恒等（カメラ無し）に戻し、フレッシュな view バッファへ書く。
    // (Begin は同一フレーム内で複数回呼ばれ得る — 反射の 2 パス等 — ので、
    //  リングを reset せず常に次スロットへ進めて前パスの draw と干渉させない。)
    m_ViewX    = static_cast<f32>(m_ScreenW) * 0.5f;
    m_ViewY    = static_cast<f32>(m_ScreenH) * 0.5f;
    m_ViewZoom = 1.0f;
    AdvanceViewBuffer();
    WriteScreenCBuffer();

    // パイプラインと共通リソースを bind
    cl.SetPipeline(*m_Pipeline);
    BindViewBuffer();
    cl.SetVertexBuffer(*m_Vb, sizeof(Vertex));
    cl.SetIndexBuffer(*m_Ib);
}

void FSpriteBatch::Draw(IRhiTexture& tex, f32 x, f32 y, f32 w, f32 h, FVec4 tint) noexcept {
    DrawSub(tex, x, y, w, h, 0, 0, 1, 1, tint);
}

void FSpriteBatch::DrawSub(IRhiTexture& tex,
                          f32 x, f32 y, f32 w, f32 h,
                          f32 u0, f32 v0, f32 u1, f32 v1,
                          FVec4 tint) noexcept {
    if (!m_Cl) return;
    // テクスチャ切替でフラッシュ（同じテクスチャは累積）
    if (m_CurrentTex && m_CurrentTex != &tex) Flush();
    // 容量上限に達したら以降は無視（max_sprites を Init 時に増やす）
    if (m_SpriteCount >= m_MaxSprites) return;
    m_CurrentTex = &tex;

    Vertex* v = m_VertexCpu + m_SpriteCount * 4;
    // 4 頂点を時計回りに積む: (x,y), (x+w,y), (x+w,y+h), (x,y+h)
    v[0] = { x,     y,     u0, v0, tint.x, tint.y, tint.z, tint.w };
    v[1] = { x+w,   y,     u1, v0, tint.x, tint.y, tint.z, tint.w };
    v[2] = { x+w,   y+h,   u1, v1, tint.x, tint.y, tint.z, tint.w };
    v[3] = { x,     y+h,   u0, v1, tint.x, tint.y, tint.z, tint.w };
    ++m_SpriteCount;
}

void FSpriteBatch::DrawRect(f32 x, f32 y, f32 w, f32 h, FVec4 color) noexcept {
    DrawSub(*m_White, x, y, w, h, 0, 0, 1, 1, color);
}

void FSpriteBatch::DrawRotated(IRhiTexture& tex,
                              f32 cx, f32 cy, f32 w, f32 h, f32 radians,
                              f32 u0, f32 v0, f32 u1, f32 v1,
                              FVec4 tint) noexcept {
    if (!m_Cl) return;
    if (m_CurrentTex && m_CurrentTex != &tex) Flush();
    if (m_SpriteCount >= m_MaxSprites) return;
    m_CurrentTex = &tex;

    const f32 s  = ::sinf(radians);
    const f32 c  = ::cosf(radians);
    const f32 hw = w * 0.5f;
    const f32 hh = h * 0.5f;
    // ローカル 4 隅 (TL, TR, BR, BL) を中心まわりに回転。DrawSub と同じ頂点順。
    const f32 lx[4] = { -hw,  hw,  hw, -hw };
    const f32 ly[4] = { -hh, -hh,  hh,  hh };
    const f32 uu[4] = {  u0,  u1,  u1,  u0 };
    const f32 vv[4] = {  v0,  v0,  v1,  v1 };

    Vertex* vtx = m_VertexCpu + m_SpriteCount * 4;
    for (int i = 0; i < 4; ++i) {
        const f32 px = cx + lx[i] * c - ly[i] * s;
        const f32 py = cy + lx[i] * s + ly[i] * c;
        vtx[i] = { px, py, uu[i], vv[i], tint.x, tint.y, tint.z, tint.w };
    }
    ++m_SpriteCount;
}

void FSpriteBatch::DrawRectRotated(f32 cx, f32 cy, f32 w, f32 h, f32 radians,
                                  FVec4 color) noexcept {
    DrawRotated(*m_White, cx, cy, w, h, radians, 0, 0, 1, 1, color);
}

void FSpriteBatch::WriteScreenCBuffer() noexcept {
    const f32 sw = static_cast<f32>(m_ScreenW);
    const f32 sh = static_cast<f32>(m_ScreenH);
    f32 cb[8] = {
        1.0f / sw, 1.0f / sh, sw, sh,
        m_ViewX, m_ViewY, m_ViewZoom, 0.0f,
    };
    m_Cb[m_CbCur]->Update(cb, sizeof(cb));
}

void FSpriteBatch::AdvanceViewBuffer() noexcept {
    m_CbCur = (m_CbCur + 1u) % kViewRing;
}

void FSpriteBatch::BindViewBuffer() noexcept {
    if (m_Cl) m_Cl->SetConstantBuffer(0, *m_Cb[m_CbCur]);
}

void FSpriteBatch::SetView(f32 cam_x, f32 cam_y, f32 zoom) noexcept {
    if (!m_Cl) return;
    Flush();   // 既存バッチを「現在の view バッファ」で確定してから切り替える
    // 別スロットへ進めて新 view を書き、root CBV を貼り直す。これで Flush 済みの
    // draw は旧スロット (旧 view) を、以降の draw は新スロット (新 view) を読む。
    AdvanceViewBuffer();
    m_ViewX    = cam_x;
    m_ViewY    = cam_y;
    m_ViewZoom = (zoom > 0.0001f) ? zoom : 1.0f;
    WriteScreenCBuffer();
    BindViewBuffer();
}

void FSpriteBatch::SetClipRect(i32 x, i32 y, i32 w, i32 h) noexcept {
    if (!m_Cl) return;
    Flush();   // クリップ変更前のバッチを確定
    FScissorRect sr{ x, y, x + w, y + h };
    m_Cl->SetScissor(sr);
}

void FSpriteBatch::ClearClipRect() noexcept {
    if (!m_Cl) return;
    Flush();
    FScissorRect sr{ 0, 0, static_cast<i32>(m_ScreenW), static_cast<i32>(m_ScreenH) };
    m_Cl->SetScissor(sr);
}

void FSpriteBatch::DrawTriangleVC(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2,
                                 FVec4 c0, FVec4 c1, FVec4 c2) noexcept {
    if (!m_Cl) return;
    if (m_CurrentTex && m_CurrentTex != m_White.Get()) Flush();
    if (m_SpriteCount >= m_MaxSprites) return;
    m_CurrentTex = m_White.Get();
    // 4 頂点目に 3 頂点目を重ねる。インデックス (0,2,3) の三角形は面積 0 に
    // 退化して描画されず、(0,1,2) の三角形だけが塗られる。各頂点に別の色を
    // 持たせると、シェーダが COLOR を補間してグラデーション三角形になる。
    Vertex* v = m_VertexCpu + m_SpriteCount * 4;
    v[0] = { x0, y0, 0, 0, c0.x, c0.y, c0.z, c0.w };
    v[1] = { x1, y1, 0, 0, c1.x, c1.y, c1.z, c1.w };
    v[2] = { x2, y2, 0, 0, c2.x, c2.y, c2.z, c2.w };
    v[3] = { x2, y2, 0, 0, c2.x, c2.y, c2.z, c2.w };
    ++m_SpriteCount;
}

void FSpriteBatch::DrawTriangle(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2,
                               FVec4 color) noexcept {
    DrawTriangleVC(x0, y0, x1, y1, x2, y2, color, color, color);
}

void FSpriteBatch::DrawTriangleSub(IRhiTexture& tex,
                                   f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2,
                                   f32 u0, f32 v0, f32 u1, f32 v1, f32 u2, f32 v2,
                                   FVec4 tint) noexcept {
    if (!m_Cl) return;
    if (m_CurrentTex && m_CurrentTex != &tex) Flush();
    if (m_SpriteCount >= m_MaxSprites) return;
    m_CurrentTex = &tex;
    // 任意テクスチャを per-vertex UV で貼る三角形 (水の反射でシーン RT をサンプル)。
    Vertex* v = m_VertexCpu + m_SpriteCount * 4;
    v[0] = { x0, y0, u0, v0, tint.x, tint.y, tint.z, tint.w };
    v[1] = { x1, y1, u1, v1, tint.x, tint.y, tint.z, tint.w };
    v[2] = { x2, y2, u2, v2, tint.x, tint.y, tint.z, tint.w };
    v[3] = { x2, y2, u2, v2, tint.x, tint.y, tint.z, tint.w };
    ++m_SpriteCount;
}

void FSpriteBatch::DrawString(const Font& font, const char* utf8_text,
                           f32 x, f32 y, FVec4 color) noexcept {
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

void FSpriteBatch::End() noexcept {
    Flush();
    m_Cl = nullptr;
}

void FSpriteBatch::Flush() noexcept {
    if (!m_Cl || !m_CurrentTex) return;
    if (m_SpriteCount <= m_FlushedCount) return;

    // 既にフラッシュ済みのスプライトの後ろに、新しい範囲だけを VB に部分書き込みする。
    // こうすることで、先行投入済みの DrawIndexed が参照する範囲を上書きしない。
    const u32   first_sprite = m_FlushedCount;
    const u32   count        = m_SpriteCount - m_FlushedCount;
    const usize byte_offset  = static_cast<usize>(first_sprite) * 4 * sizeof(Vertex);
    const usize byte_size    = static_cast<usize>(count) * 4 * sizeof(Vertex);
    m_Vb->Update(m_VertexCpu + first_sprite * 4, byte_size, byte_offset);

    m_Cl->SetTexture(0, *m_CurrentTex);
    m_Cl->DrawIndexed(count * 6, first_sprite * 6, 0);

    m_FlushedCount = m_SpriteCount;
    m_CurrentTex   = nullptr;
}

void FSpriteBatch::EnsurePipeline() noexcept {
    // 現状未使用。RT フォーマットが動的に変わる場合のリビルドフックの placeholder
}

} // namespace acs
