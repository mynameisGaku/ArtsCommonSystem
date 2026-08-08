// SPDX-License-Identifier: Apache-2.0
// CDebugDraw 実装。
#include "gameframework/DebugDraw.h"

#include "foundation/Limits.h"
#include "math/Math.h"

#include <cmath>

namespace acs::game {

namespace {

/** 2次元vectorの全成分が有限かを返す。 */
bool IsFinite(FVec2 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y);
}

/** RGBA色の全成分が有限かを返す。 */
bool IsFinite(FVec4 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
}

/** f64の計算結果を有限なf32へ変換する。 */
bool TryConvertF32(f64 value, f32& output) noexcept
{
    constexpr f64 maximum = static_cast<f64>(TNumLimits<f32>::Max());
    if (!std::isfinite(value) || value < -maximum || value > maximum) return false;
    output = static_cast<f32>(value);
    return std::isfinite(output);
}

/** f64の2成分を有限なFVec2へ変換する。 */
bool TryMakeVec2(f64 x, f64 y, FVec2& output) noexcept
{
    return TryConvertF32(x, output.x) && TryConvertF32(y, output.y);
}

} // namespace

bool CDebugDraw::TryAppendSpace(usize count, FLine*& output) noexcept
{
    output = nullptr;
    if (count == 0u) return true;

    /** u32の公開件数で正確に表現できる最大線分数。 */
    constexpr usize maximum_line_count = static_cast<usize>(TNumLimits<u32>::Max());
    const usize old_count = m_Lines.Num();
    if (old_count > maximum_line_count || count > maximum_line_count - old_count) return false;
    if (!m_Lines.TrySetNum(old_count + count)) return false;

    output = m_Lines.GetData() + old_count;
    return true;
}

bool CDebugDraw::TryDrawLine(FVec2 a, FVec2 b, FVec4 color) noexcept
{
    if (!IsFinite(a) || !IsFinite(b) || !IsFinite(color)) return false;

    FLine* output = nullptr;
    if (!TryAppendSpace(1u, output)) return false;
    output[0] = FLine{a, b, color};
    return true;
}

void CDebugDraw::DrawLine(FVec2 a, FVec2 b, FVec4 color) noexcept
{
    (void)TryDrawLine(a, b, color);
}

bool CDebugDraw::TryDrawAabb(const FAabb2& a, FVec4 color) noexcept
{
    if (!IsFinite(a.center) || !IsFinite(a.half_size) || !IsFinite(color)) return false;

    /** f32演算を行う前に範囲を検査する四隅。 */
    FVec2 minimum{};
    FVec2 maximum{};
    const bool minimum_valid = TryMakeVec2(static_cast<f64>(a.center.x) - a.half_size.x, static_cast<f64>(a.center.y) - a.half_size.y, minimum);
    const bool maximum_valid = TryMakeVec2(static_cast<f64>(a.center.x) + a.half_size.x, static_cast<f64>(a.center.y) + a.half_size.y, maximum);
    if (!minimum_valid || !maximum_valid) return false;

    const FVec2 top_left{minimum.x, minimum.y};
    const FVec2 top_right{maximum.x, minimum.y};
    const FVec2 bottom_right{maximum.x, maximum.y};
    const FVec2 bottom_left{minimum.x, maximum.y};

    FLine* output = nullptr;
    if (!TryAppendSpace(4u, output)) return false;
    output[0] = FLine{top_left, top_right, color};
    output[1] = FLine{top_right, bottom_right, color};
    output[2] = FLine{bottom_right, bottom_left, color};
    output[3] = FLine{bottom_left, top_left, color};
    return true;
}

void CDebugDraw::DrawAabb(const FAabb2& a, FVec4 color) noexcept
{
    (void)TryDrawAabb(a, color);
}

bool CDebugDraw::TryDrawCircle(const FCircle& circle, FVec4 color, u32 segments) noexcept
{
    if (!IsFinite(circle.center) || !std::isfinite(circle.radius) || !IsFinite(color)) return false;
    if (segments < 3u) segments = 3u;

    /** 全角度の中心加算がf32有限範囲に収まるかを先に検査する。 */
    constexpr f64 maximum = static_cast<f64>(TNumLimits<f32>::Max());
    const f64 radius_magnitude = std::fabs(static_cast<f64>(circle.radius));
    const bool x_in_range = std::fabs(static_cast<f64>(circle.center.x)) <= maximum - radius_magnitude;
    const bool y_in_range = std::fabs(static_cast<f64>(circle.center.y)) <= maximum - radius_magnitude;
    if (!x_in_range || !y_in_range) return false;

    FLine* output = nullptr;
    if (!TryAppendSpace(static_cast<usize>(segments), output)) return false;

    const f32 inverse_segments = 1.0f / static_cast<f32>(segments);
    const f32 angle_step = kTwoPi * inverse_segments;
    FVec2 previous{circle.center.x + circle.radius, circle.center.y};
    for (u32 index = 0u; index < segments; ++index) {
        const f32 angle = angle_step * static_cast<f32>(index + 1u);
        const FVec2 current{circle.center.x + circle.radius * Cos(angle), circle.center.y + circle.radius * Sin(angle)};
        output[index] = FLine{previous, current, color};
        previous = current;
    }
    return true;
}

void CDebugDraw::DrawCircle(const FCircle& circle, FVec4 color, u32 segments) noexcept
{
    (void)TryDrawCircle(circle, color, segments);
}

bool CDebugDraw::TryDrawCross(FVec2 position, f32 size, FVec4 color) noexcept
{
    if (!IsFinite(position) || !std::isfinite(size) || !IsFinite(color)) return false;

    FVec2 left{};
    FVec2 right{};
    FVec2 top{};
    FVec2 bottom{};
    const bool left_valid = TryMakeVec2(static_cast<f64>(position.x) - size, position.y, left);
    const bool right_valid = TryMakeVec2(static_cast<f64>(position.x) + size, position.y, right);
    const bool top_valid = TryMakeVec2(position.x, static_cast<f64>(position.y) - size, top);
    const bool bottom_valid = TryMakeVec2(position.x, static_cast<f64>(position.y) + size, bottom);
    if (!left_valid || !right_valid || !top_valid || !bottom_valid) return false;

    FLine* output = nullptr;
    if (!TryAppendSpace(2u, output)) return false;
    output[0] = FLine{left, right, color};
    output[1] = FLine{top, bottom, color};
    return true;
}

void CDebugDraw::DrawCross(FVec2 position, f32 size, FVec4 color) noexcept
{
    (void)TryDrawCross(position, size, color);
}

bool CDebugDraw::TryDrawArrow(FVec2 a, FVec2 b, FVec4 color, f32 head_len) noexcept
{
    if (!IsFinite(a) || !IsFinite(b) || !IsFinite(color) || !std::isfinite(head_len)) return false;

    /** f32差分と長さ二乗が表現範囲を超えないことを先に検査する。 */
    f32 delta_x = 0.0f;
    f32 delta_y = 0.0f;
    const bool delta_x_valid = TryConvertF32(static_cast<f64>(b.x) - a.x, delta_x);
    const bool delta_y_valid = TryConvertF32(static_cast<f64>(b.y) - a.y, delta_y);
    if (!delta_x_valid || !delta_y_valid) return false;
    const f64 length_squared_wide = static_cast<f64>(delta_x) * delta_x + static_cast<f64>(delta_y) * delta_y;
    if (!std::isfinite(length_squared_wide) || length_squared_wide > static_cast<f64>(TNumLimits<f32>::Max())) {
        return false;
    }
    const f32 length_squared = delta_x * delta_x + delta_y * delta_y;
    if (!std::isfinite(length_squared)) return false;
    const f32 length = std::sqrt(length_squared);

    /** 退化した矢印は従来どおり軸1本だけを追加する。 */
    if (length < 1.0e-6f) return TryDrawLine(a, b, color);

    const FVec2 direction{delta_x / length, delta_y / length};
    const f32 actual_head_length = head_len > 0.0f ? head_len : length * 0.2f;
    const FVec2 back{-direction.x, -direction.y};
    const f32 cosine = Cos(0.4f);
    const f32 sine = Sin(0.4f);
    const FVec2 left_direction{back.x * cosine - back.y * sine, back.x * sine + back.y * cosine};
    const FVec2 right_direction{back.x * cosine + back.y * sine, -back.x * sine + back.y * cosine};
    /** 従来のf32演算順を保ち、各積と加算が有限かを確定前に検査する矢じり端点。 */
    const f32 left_offset_x = left_direction.x * actual_head_length;
    const f32 left_offset_y = left_direction.y * actual_head_length;
    const f32 right_offset_x = right_direction.x * actual_head_length;
    const f32 right_offset_y = right_direction.y * actual_head_length;
    const bool offsets_finite = std::isfinite(left_offset_x) && std::isfinite(left_offset_y) && std::isfinite(right_offset_x) && std::isfinite(right_offset_y);
    if (!offsets_finite) return false;
    const FVec2 left_end{b.x + left_offset_x, b.y + left_offset_y};
    const FVec2 right_end{b.x + right_offset_x, b.y + right_offset_y};
    if (!IsFinite(left_end) || !IsFinite(right_end)) return false;

    FLine* output = nullptr;
    if (!TryAppendSpace(3u, output)) return false;
    output[0] = FLine{a, b, color};
    output[1] = FLine{b, left_end, color};
    output[2] = FLine{b, right_end, color};
    return true;
}

void CDebugDraw::DrawArrow(FVec2 a, FVec2 b, FVec4 color, f32 head_len) noexcept
{
    (void)TryDrawArrow(a, b, color, head_len);
}

} // namespace acs::game
