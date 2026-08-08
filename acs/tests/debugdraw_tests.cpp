// SPDX-License-Identifier: Apache-2.0
#include "gameframework/DebugDraw.h"

#include "foundation/Limits.h"
#include "gameframework/RigidWorld2D.h"
#include "gameframework/RigidWorldDebug.h"
#include "memory/Memory.h"
#include "test/Expect.h"
#include "test/Test.h"

#include <cstddef>
#include <type_traits>

using namespace acs;
using namespace acs::game;

namespace {

/** DebugDrawのCPU確保を要求単位で失敗させるallocator。 */
class CDebugDrawFailAllocator final : public IAllocator {
public:
    /** 通常確保を委譲するallocatorを指定する。 */
    explicit CDebugDrawFailAllocator(IAllocator& backing) noexcept : m_Backing(&backing) {}

    /** 以後の確保を失敗させるかを設定する。 */
    void SetFailing(bool failing) noexcept
    {
        m_Failing = failing;
    }

    /** 確保要求数だけを0へ戻す。 */
    void ResetRequestCount() noexcept
    {
        m_RequestCount = 0u;
    }

    /** 現在までの確保要求数を返す。 */
    u32 RequestCount() const noexcept
    {
        return m_RequestCount;
    }

    void* Alloc(usize size, usize alignment, FSourceLoc location) noexcept override
    {
        ++m_RequestCount;
        return m_Failing ? nullptr : m_Backing->Alloc(size, alignment, location);
    }

    void Free(void* pointer) noexcept override
    {
        m_Backing->Free(pointer);
    }

private:
    /** 通常確保と全解放を委譲するallocator。 */
    IAllocator* m_Backing = nullptr;
    /** trueなら新しい確保を失敗させる。 */
    bool m_Failing = false;
    /** Alloc/Reallocから到達した確保要求数。 */
    u32 m_RequestCount = 0u;
};

/** scope終了時に既定allocatorを復元する。 */
class CDebugDrawAllocatorScope {
public:
    /** scope内の既定allocatorをreplacementへ切り替える。 */
    explicit CDebugDrawAllocatorScope(IAllocator& replacement) noexcept : m_Previous(&DefaultAllocator())
    {
        SetDefaultAllocator(&replacement);
    }

    /** scope開始前の既定allocatorを復元する。 */
    ~CDebugDrawAllocatorScope() noexcept
    {
        SetDefaultAllocator(m_Previous);
    }

private:
    /** scope開始前の既定allocator。 */
    IAllocator* m_Previous = nullptr;
};

/** 2次元vectorを成分単位で比較する。 */
bool Vec2Equals(FVec2 left, FVec2 right) noexcept
{
    return left.x == right.x && left.y == right.y;
}

/** RGBA色を成分単位で比較する。 */
bool ColorEquals(FVec4 left, FVec4 right) noexcept
{
    return left.x == right.x && left.y == right.y && left.z == right.z && left.w == right.w;
}

/** x座標が異なる線分を7本追加し、初回成長で得た8要素容量を1要素残す。 */
bool FillSevenLines(CDebugDraw& debug_draw, FVec4 color) noexcept
{
    debug_draw.Clear();
    for (u32 index = 0u; index < 7u; ++index) {
        const f32 x = static_cast<f32>(index);
        if (!debug_draw.TryDrawLine(FVec2{x, 0.0f}, FVec2{x, 1.0f}, color)) return false;
    }
    return debug_draw.LineCount() == 7u;
}

/** FillSevenLinesで追加した7本がすべて変更されていないかを返す。 */
bool SevenLinesAreUnchanged(const CDebugDraw& debug_draw, FVec4 color) noexcept
{
    if (debug_draw.LineCount() != 7u || !debug_draw.Lines()) return false;
    for (u32 index = 0u; index < 7u; ++index) {
        const f32 x = static_cast<f32>(index);
        const CDebugDraw::FLine& line = debug_draw.Lines()[index];
        const bool unchanged = Vec2Equals(line.a, FVec2{x, 0.0f}) && Vec2Equals(line.b, FVec2{x, 1.0f}) && ColorEquals(line.color, color);
        if (!unchanged) return false;
    }
    return true;
}

using FDrawLineSignature = void (CDebugDraw::*)(FVec2, FVec2, FVec4) noexcept;
using FDrawAabbSignature = void (CDebugDraw::*)(const FAabb2&, FVec4) noexcept;
using FDrawCircleSignature = void (CDebugDraw::*)(const FCircle&, FVec4, u32) noexcept;
using FDrawCrossSignature = void (CDebugDraw::*)(FVec2, f32, FVec4) noexcept;
using FDrawArrowSignature = void (CDebugDraw::*)(FVec2, FVec2, FVec4, f32) noexcept;
using FTryDrawLineSignature = bool (CDebugDraw::*)(FVec2, FVec2, FVec4) noexcept;
using FTryDrawAabbSignature = bool (CDebugDraw::*)(const FAabb2&, FVec4) noexcept;
using FTryDrawCircleSignature = bool (CDebugDraw::*)(const FCircle&, FVec4, u32) noexcept;
using FTryDrawCrossSignature = bool (CDebugDraw::*)(FVec2, f32, FVec4) noexcept;
using FTryDrawArrowSignature = bool (CDebugDraw::*)(FVec2, FVec2, FVec4, f32) noexcept;

static_assert(std::is_same_v<decltype(&CDebugDraw::DrawLine), FDrawLineSignature>);
static_assert(std::is_same_v<decltype(&CDebugDraw::DrawAabb), FDrawAabbSignature>);
static_assert(std::is_same_v<decltype(&CDebugDraw::DrawCircle), FDrawCircleSignature>);
static_assert(std::is_same_v<decltype(&CDebugDraw::DrawCross), FDrawCrossSignature>);
static_assert(std::is_same_v<decltype(&CDebugDraw::DrawArrow), FDrawArrowSignature>);
static_assert(std::is_same_v<decltype(&CDebugDraw::TryDrawLine), FTryDrawLineSignature>);
static_assert(std::is_same_v<decltype(&CDebugDraw::TryDrawAabb), FTryDrawAabbSignature>);
static_assert(std::is_same_v<decltype(&CDebugDraw::TryDrawCircle), FTryDrawCircleSignature>);
static_assert(std::is_same_v<decltype(&CDebugDraw::TryDrawCross), FTryDrawCrossSignature>);
static_assert(std::is_same_v<decltype(&CDebugDraw::TryDrawArrow), FTryDrawArrowSignature>);
static_assert(std::is_same_v<FDebugDraw, CDebugDraw>);
static_assert(sizeof(CDebugDraw) == 32u);
static_assert(alignof(CDebugDraw) == 8u);
static_assert(sizeof(CDebugDraw::FLine) == 32u);
static_assert(alignof(CDebugDraw::FLine) == 16u);
static_assert(offsetof(CDebugDraw::FLine, a) == 0u);
static_assert(offsetof(CDebugDraw::FLine, b) == 8u);
static_assert(offsetof(CDebugDraw::FLine, color) == 16u);
static_assert(std::is_aggregate_v<CDebugDraw::FLine>);
static_assert(std::is_standard_layout_v<CDebugDraw::FLine>);
static_assert(std::is_trivially_copyable_v<CDebugDraw::FLine>);

} // namespace

ACS_TEST(DebugDraw, ArrowEmitsThreeLines)
{
    CDebugDraw debug_draw;
    debug_draw.DrawArrow(FVec2{0.0f, 0.0f}, FVec2{1.0f, 0.0f}, FVec4{1.0f, 1.0f, 1.0f, 1.0f});
    EXPECT_EQ(debug_draw.LineCount(), 3u);
    const CDebugDraw::FLine* const arrow_lines = debug_draw.Lines();
    EXPECT_TRUE(arrow_lines != nullptr);
    if (arrow_lines) {
        const f32 cosine = Cos(0.4f);
        const f32 sine = Sin(0.4f);
        EXPECT_TRUE(Vec2Equals(arrow_lines[0].a, FVec2{0.0f, 0.0f}));
        EXPECT_TRUE(Vec2Equals(arrow_lines[0].b, FVec2{1.0f, 0.0f}));
        EXPECT_TRUE(Vec2Equals(arrow_lines[1].a, FVec2{1.0f, 0.0f}));
        EXPECT_TRUE(Vec2Equals(arrow_lines[1].b, FVec2{1.0f + -cosine * 0.2f, -sine * 0.2f}));
        EXPECT_TRUE(Vec2Equals(arrow_lines[2].a, FVec2{1.0f, 0.0f}));
        EXPECT_TRUE(Vec2Equals(arrow_lines[2].b, FVec2{1.0f + -cosine * 0.2f, sine * 0.2f}));
    }

    debug_draw.Clear();
    debug_draw.DrawArrow(FVec2{2.0f, 2.0f}, FVec2{2.0f, 2.0f}, FVec4{1.0f, 1.0f, 1.0f, 1.0f});
    EXPECT_EQ(debug_draw.LineCount(), 1u);
}

ACS_TEST(DebugDraw, PrimitiveWritesPreserveOrderAndColor)
{
    CDebugDraw debug_draw;
    const FVec4 color{1.0f, 0.5f, 0.25f, 1.0f};

    EXPECT_TRUE(debug_draw.TryDrawAabb(FAabb2{FVec2{2.0f, 3.0f}, FVec2{1.0f, 2.0f}}, color));
    EXPECT_EQ(debug_draw.LineCount(), 4u);
    const CDebugDraw::FLine* lines = debug_draw.Lines();
    EXPECT_TRUE(lines != nullptr);
    if (lines) {
        EXPECT_TRUE(Vec2Equals(lines[0].a, FVec2{1.0f, 1.0f}));
        EXPECT_TRUE(Vec2Equals(lines[0].b, FVec2{3.0f, 1.0f}));
        EXPECT_TRUE(Vec2Equals(lines[1].a, FVec2{3.0f, 1.0f}));
        EXPECT_TRUE(Vec2Equals(lines[1].b, FVec2{3.0f, 5.0f}));
        EXPECT_TRUE(Vec2Equals(lines[2].a, FVec2{3.0f, 5.0f}));
        EXPECT_TRUE(Vec2Equals(lines[2].b, FVec2{1.0f, 5.0f}));
        EXPECT_TRUE(Vec2Equals(lines[3].a, FVec2{1.0f, 5.0f}));
        EXPECT_TRUE(Vec2Equals(lines[3].b, FVec2{1.0f, 1.0f}));
        for (u32 index = 0u; index < 4u; ++index) EXPECT_TRUE(ColorEquals(lines[index].color, color));
    }

    debug_draw.Clear();
    EXPECT_TRUE(debug_draw.TryDrawCross(FVec2{4.0f, 5.0f}, 2.0f, color));
    EXPECT_EQ(debug_draw.LineCount(), 2u);
    lines = debug_draw.Lines();
    if (lines) {
        EXPECT_TRUE(Vec2Equals(lines[0].a, FVec2{2.0f, 5.0f}));
        EXPECT_TRUE(Vec2Equals(lines[0].b, FVec2{6.0f, 5.0f}));
        EXPECT_TRUE(Vec2Equals(lines[1].a, FVec2{4.0f, 3.0f}));
        EXPECT_TRUE(Vec2Equals(lines[1].b, FVec2{4.0f, 7.0f}));
    }

    debug_draw.Clear();
    EXPECT_TRUE(debug_draw.TryDrawCircle(FCircle{FVec2{0.0f, 0.0f}, 2.0f}, color, 0u));
    EXPECT_EQ(debug_draw.LineCount(), 3u);
    lines = debug_draw.Lines();
    if (lines) {
        EXPECT_TRUE(Vec2Equals(lines[0].a, FVec2{2.0f, 0.0f}));
        EXPECT_TRUE(Vec2Equals(lines[0].b, lines[1].a));
        EXPECT_TRUE(Vec2Equals(lines[1].b, lines[2].a));
    }

    debug_draw.Clear();
    EXPECT_TRUE(debug_draw.TryDrawCircle(FCircle{FVec2{0.0f, 0.0f}, 1.0f}, color, 4096u));
    EXPECT_EQ(debug_draw.LineCount(), 4096u);
}

ACS_TEST(DebugDraw, AllocationFailuresLeavePrimitiveAndCapacityUnchanged)
{
    IAllocator& backing = DefaultAllocator();
    CDebugDrawFailAllocator allocator(backing);
    CDebugDrawAllocatorScope allocator_scope(allocator);
    CDebugDraw debug_draw;
    const FVec4 color{0.25f, 0.5f, 0.75f, 1.0f};

    EXPECT_TRUE(FillSevenLines(debug_draw, color));
    const CDebugDraw::FLine* const storage = debug_draw.Lines();
    allocator.ResetRequestCount();
    allocator.SetFailing(true);
    EXPECT_FALSE(debug_draw.TryDrawAabb(FAabb2{FVec2{0.0f, 0.0f}, FVec2{1.0f, 1.0f}}, color));
    EXPECT_EQ(debug_draw.LineCount(), 7u);
    EXPECT_TRUE(debug_draw.Lines() == storage);
    EXPECT_TRUE(SevenLinesAreUnchanged(debug_draw, color));
    EXPECT_EQ(allocator.RequestCount(), 1u);
    debug_draw.DrawAabb(FAabb2{FVec2{0.0f, 0.0f}, FVec2{1.0f, 1.0f}}, color);
    EXPECT_EQ(debug_draw.LineCount(), 7u);
    EXPECT_TRUE(debug_draw.Lines() == storage);

    allocator.ResetRequestCount();
    EXPECT_TRUE(debug_draw.TryDrawLine(FVec2{8.0f, 0.0f}, FVec2{8.0f, 1.0f}, color));
    EXPECT_EQ(debug_draw.LineCount(), 8u);
    EXPECT_TRUE(debug_draw.Lines() == storage);
    EXPECT_EQ(allocator.RequestCount(), 0u);

    allocator.ResetRequestCount();
    EXPECT_FALSE(debug_draw.TryDrawLine(FVec2{9.0f, 0.0f}, FVec2{9.0f, 1.0f}, color));
    EXPECT_EQ(debug_draw.LineCount(), 8u);
    EXPECT_TRUE(debug_draw.Lines() == storage);
    EXPECT_EQ(allocator.RequestCount(), 1u);

    EXPECT_TRUE(FillSevenLines(debug_draw, color));
    allocator.ResetRequestCount();
    EXPECT_FALSE(debug_draw.TryDrawCircle(FCircle{FVec2{0.0f, 0.0f}, 1.0f}, color, 3u));
    EXPECT_EQ(debug_draw.LineCount(), 7u);
    EXPECT_TRUE(debug_draw.Lines() == storage);
    EXPECT_TRUE(SevenLinesAreUnchanged(debug_draw, color));
    EXPECT_EQ(allocator.RequestCount(), 1u);

    EXPECT_TRUE(FillSevenLines(debug_draw, color));
    allocator.ResetRequestCount();
    EXPECT_FALSE(debug_draw.TryDrawCross(FVec2{0.0f, 0.0f}, 1.0f, color));
    EXPECT_EQ(debug_draw.LineCount(), 7u);
    EXPECT_TRUE(debug_draw.Lines() == storage);
    EXPECT_TRUE(SevenLinesAreUnchanged(debug_draw, color));
    EXPECT_EQ(allocator.RequestCount(), 1u);

    EXPECT_TRUE(FillSevenLines(debug_draw, color));
    allocator.ResetRequestCount();
    EXPECT_FALSE(debug_draw.TryDrawArrow(FVec2{0.0f, 0.0f}, FVec2{1.0f, 0.0f}, color));
    EXPECT_EQ(debug_draw.LineCount(), 7u);
    EXPECT_TRUE(debug_draw.Lines() == storage);
    EXPECT_TRUE(SevenLinesAreUnchanged(debug_draw, color));
    EXPECT_EQ(allocator.RequestCount(), 1u);
    allocator.SetFailing(false);
}

ACS_TEST(DebugDraw, InvalidAndOverflowInputsDoNotAllocateOrChangeState)
{
    IAllocator& backing = DefaultAllocator();
    CDebugDrawFailAllocator allocator(backing);
    CDebugDrawAllocatorScope allocator_scope(allocator);
    CDebugDraw debug_draw;
    const FVec4 color{1.0f, 1.0f, 1.0f, 1.0f};
    const f32 infinity = TNumLimits<f32>::Infinity();
    const f32 maximum = TNumLimits<f32>::Max();

    EXPECT_TRUE(debug_draw.TryDrawLine(FVec2{0.0f, 0.0f}, FVec2{1.0f, 0.0f}, color));
    const CDebugDraw::FLine* const storage = debug_draw.Lines();
    allocator.ResetRequestCount();

    EXPECT_FALSE(debug_draw.TryDrawLine(FVec2{infinity, 0.0f}, FVec2{1.0f, 0.0f}, color));
    EXPECT_FALSE(debug_draw.TryDrawLine(FVec2{0.0f, 0.0f}, FVec2{1.0f, 0.0f}, FVec4{infinity, 1.0f, 1.0f, 1.0f}));
    EXPECT_FALSE(debug_draw.TryDrawAabb(FAabb2{FVec2{maximum, maximum}, FVec2{maximum, maximum}}, color));
    EXPECT_FALSE(debug_draw.TryDrawCircle(FCircle{FVec2{0.0f, 0.0f}, 1.0f}, color, 4097u));
    EXPECT_FALSE(debug_draw.TryDrawCircle(FCircle{FVec2{0.0f, 0.0f}, 1.0f}, color, TNumLimits<u32>::Max()));
    EXPECT_FALSE(debug_draw.TryDrawCircle(FCircle{FVec2{infinity, 0.0f}, 1.0f}, color, 8u));
    EXPECT_FALSE(debug_draw.TryDrawCross(FVec2{maximum, maximum}, maximum, color));
    EXPECT_FALSE(debug_draw.TryDrawArrow(FVec2{-maximum, 0.0f}, FVec2{maximum, 0.0f}, color));
    EXPECT_FALSE(debug_draw.TryDrawArrow(FVec2{0.0f, 0.0f}, FVec2{1.0f, 0.0f}, color, infinity));
    EXPECT_EQ(debug_draw.LineCount(), 1u);
    EXPECT_TRUE(debug_draw.Lines() == storage);
    EXPECT_TRUE(Vec2Equals(debug_draw.Lines()[0].a, FVec2{0.0f, 0.0f}));
    EXPECT_TRUE(Vec2Equals(debug_draw.Lines()[0].b, FVec2{1.0f, 0.0f}));
    EXPECT_TRUE(ColorEquals(debug_draw.Lines()[0].color, color));
    EXPECT_EQ(allocator.RequestCount(), 0u);
}

ACS_TEST(DebugDraw, RigidWorldVisualizationLineCounts)
{
    const FVec4 color{1.0f, 1.0f, 1.0f, 1.0f};
    const FVec4 velocity_color{1.0f, 0.0f, 0.0f, 1.0f};

    CRigidWorld2D static_world;
    static_world.AddStaticAabb(FVec2{5.0f, 0.0f}, FVec2{1.0f, 1.0f});
    static_world.AddCircle(FVec2{0.0f, 0.0f}, 1.0f, 0.0f);
    CDebugDraw static_draw;
    DebugDrawRigidWorld(static_world, static_draw, color, velocity_color);
    EXPECT_EQ(static_draw.LineCount(), 28u);

    CRigidWorld2D dynamic_world;
    const u32 dynamic_body = dynamic_world.AddCircle(FVec2{0.0f, 0.0f}, 1.0f, 1.0f);
    dynamic_world.SetVelocity(dynamic_body, FVec2{3.0f, 0.0f});
    CDebugDraw dynamic_draw;
    DebugDrawRigidWorld(dynamic_world, dynamic_draw, color, velocity_color);
    EXPECT_EQ(dynamic_draw.LineCount(), 27u);

    CRigidWorld2D removed_world;
    const u32 removed_body = removed_world.AddCircle(FVec2{0.0f, 0.0f}, 1.0f, 0.0f);
    removed_world.AddStaticAabb(FVec2{5.0f, 0.0f}, FVec2{1.0f, 1.0f});
    removed_world.RemoveBody(removed_body);
    CDebugDraw removed_draw;
    DebugDrawRigidWorld(removed_world, removed_draw, color, velocity_color);
    EXPECT_EQ(removed_draw.LineCount(), 4u);
}
