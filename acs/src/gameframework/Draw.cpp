// SPDX-License-Identifier: Apache-2.0
// GameFramework — 即時描画の実装 (現在の描画パスへ直接積む)
#include "gameframework/Draw.h"

#include "gameframework/RenderContext.h"
#include "render/SpriteBatch.h"
#include "render/IRhiTexture.h"

#include <cmath>   // std::cos / std::sin / std::atan2 / std::sqrt

namespace acs::game {

namespace {

/** 現在の描画パス (AScene::OnRender が publish、pass 外は nullptr)。main スレッド専用。 */
FRenderContext* g_draw_context = nullptr;

/** 円周分割数の上限 (1 円あたりの三角形数を抑える)。 */
constexpr u32 kMaxCircleSegments = 128u;

/** 描ける状態なら sprite batch を返す (パス外・batch 未配線なら nullptr)。 */
CSpriteBatch* ActiveBatch() noexcept {
    if (g_draw_context == nullptr) return nullptr;
    if (!g_draw_context->HasSprites()) return nullptr;
    return &g_draw_context->Sprites();
}

/** 分割数を [3, kMaxCircleSegments] へ丸める。 */
u32 ClampSegments(u32 segments) noexcept {
    if (segments < 3u) return 3u;
    return segments > kMaxCircleSegments ? kMaxCircleSegments : segments;
}

} // namespace

void _SetDrawContext(FRenderContext* context) noexcept {
    g_draw_context = context;
}

FRenderContext* _CurrentDrawContext() noexcept {
    return g_draw_context;
}

bool IsDrawing() noexcept {
    return ActiveBatch() != nullptr;
}

u32 DrawWidth() noexcept {
    return g_draw_context != nullptr ? g_draw_context->Width() : 0u;
}

u32 DrawHeight() noexcept {
    return g_draw_context != nullptr ? g_draw_context->Height() : 0u;
}

void DrawRect(f32 x, f32 y, f32 w, f32 h, FVec4 color) noexcept {
    if (CSpriteBatch* batch = ActiveBatch()) batch->DrawRect(x, y, w, h, color);
}

void DrawRectOutline(f32 x, f32 y, f32 w, f32 h, FVec4 color,
                     f32 thickness) noexcept {
    CSpriteBatch* batch = ActiveBatch();
    if (batch == nullptr) return;
    if (!(thickness > 0.0f)) return;
    // 角を二重に塗らないよう、上下は全幅、左右は上下の内側だけを埋める。
    const f32 side_h = h - thickness * 2.0f;
    batch->DrawRect(x, y, w, thickness, color);
    batch->DrawRect(x, y + h - thickness, w, thickness, color);
    if (side_h > 0.0f) {
        batch->DrawRect(x, y + thickness, thickness, side_h, color);
        batch->DrawRect(x + w - thickness, y + thickness, thickness, side_h, color);
    }
}

void DrawRectRotated(f32 cx, f32 cy, f32 w, f32 h, f32 radians,
                     FVec4 color) noexcept {
    if (CSpriteBatch* batch = ActiveBatch())
        batch->DrawRectRotated(cx, cy, w, h, radians, color);
}

void DrawCircle(f32 cx, f32 cy, f32 radius, FVec4 color, u32 segments) noexcept {
    CSpriteBatch* batch = ActiveBatch();
    if (batch == nullptr || !(radius > 0.0f)) return;
    const u32 count = ClampSegments(segments);
    const f32 step  = 6.28318531f / static_cast<f32>(count);
    f32 px = cx + radius;
    f32 py = cy;
    for (u32 i = 1; i <= count; ++i) {
        const f32 angle = step * static_cast<f32>(i);
        const f32 nx = cx + radius * std::cos(angle);
        const f32 ny = cy + radius * std::sin(angle);
        batch->DrawTriangle(cx, cy, px, py, nx, ny, color);
        px = nx;
        py = ny;
    }
}

void DrawCircleOutline(f32 cx, f32 cy, f32 radius, FVec4 color,
                       f32 thickness, u32 segments) noexcept {
    CSpriteBatch* batch = ActiveBatch();
    if (batch == nullptr || !(radius > 0.0f) || !(thickness > 0.0f)) return;
    const u32 count = ClampSegments(segments);
    const f32 step  = 6.28318531f / static_cast<f32>(count);
    f32 px = cx + radius;
    f32 py = cy;
    for (u32 i = 1; i <= count; ++i) {
        const f32 angle = step * static_cast<f32>(i);
        const f32 nx = cx + radius * std::cos(angle);
        const f32 ny = cy + radius * std::sin(angle);
        DrawLine(px, py, nx, ny, color, thickness);
        px = nx;
        py = ny;
    }
}

void DrawLine(f32 x0, f32 y0, f32 x1, f32 y1, FVec4 color,
              f32 thickness) noexcept {
    CSpriteBatch* batch = ActiveBatch();
    if (batch == nullptr || !(thickness > 0.0f)) return;
    const f32 dx     = x1 - x0;
    const f32 dy     = y1 - y0;
    const f32 length = std::sqrt(dx * dx + dy * dy);
    if (!(length > 0.0f)) return;
    // 中点を回転中心にした «長さ×太さ» の矩形として描く。
    batch->DrawRectRotated((x0 + x1) * 0.5f, (y0 + y1) * 0.5f,
                           length, thickness, std::atan2(dy, dx), color);
}

void DrawTriangle(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2,
                  FVec4 color) noexcept {
    if (CSpriteBatch* batch = ActiveBatch())
        batch->DrawTriangle(x0, y0, x1, y1, x2, y2, color);
}

void DrawTexture(IRhiTexture& texture, f32 x, f32 y, FVec4 tint) noexcept {
    CSpriteBatch* batch = ActiveBatch();
    if (batch == nullptr) return;
    batch->Draw(texture, x, y,
                static_cast<f32>(texture.Width()),
                static_cast<f32>(texture.Height()), tint);
}

void DrawTexture(IRhiTexture& texture, f32 x, f32 y, f32 w, f32 h,
                 FVec4 tint) noexcept {
    if (CSpriteBatch* batch = ActiveBatch())
        batch->Draw(texture, x, y, w, h, tint);
}

void DrawTextureRotated(IRhiTexture& texture, f32 cx, f32 cy, f32 w, f32 h,
                        f32 radians, FVec4 tint) noexcept {
    if (CSpriteBatch* batch = ActiveBatch())
        batch->DrawRotated(texture, cx, cy, w, h, radians,
                           0.0f, 0.0f, 1.0f, 1.0f, tint);
}

void DrawTextureSub(IRhiTexture& texture, f32 x, f32 y, f32 w, f32 h,
                    f32 u0, f32 v0, f32 u1, f32 v1, FVec4 tint) noexcept {
    if (CSpriteBatch* batch = ActiveBatch())
        batch->DrawSub(texture, x, y, w, h, u0, v0, u1, v1, tint);
}

void DrawString(f32 x, f32 y, const char* utf8_text, FVec4 color) noexcept {
    CSpriteBatch* batch = ActiveBatch();
    if (batch == nullptr || utf8_text == nullptr) return;
    if (!g_draw_context->HasFont()) return;
    batch->DrawString(g_draw_context->GetFont(), utf8_text, x, y, color);
}

} // namespace acs::game
