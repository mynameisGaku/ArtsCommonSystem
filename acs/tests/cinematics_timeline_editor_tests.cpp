// SPDX-License-Identifier: Apache-2.0
#include "test/Expect.h"
#include "test/Test.h"
#include "gameframework/CinematicsDirector.h"
#include "gameframework/tools/cinetimeline/CinematicsTimelineEditorPanel.h"
#include "gameframework/tools/cinetimeline/ACinematicsTimelineEditorPanel.h"

#include <imgui.h>

#include <limits>
#include <type_traits>

using namespace acs;
using namespace acs::game;
using namespace acs::game::cinetimeline;

namespace {
class CImGuiContextScope final {
public:
    CImGuiContextScope() noexcept : m_Previous(ImGui::GetCurrentContext())
    {
        if (m_Previous == nullptr)
        {
            m_Owned = ImGui::CreateContext();
        }
    }

    ~CImGuiContextScope() noexcept
    {
        if (m_Owned != nullptr)
        {
            ImGui::DestroyContext(m_Owned);
        }
        else
        {
            ImGui::SetCurrentContext(m_Previous);
        }
    }

private:
    ImGuiContext* m_Previous = nullptr;
    ImGuiContext* m_Owned = nullptr;
};

struct FColorBounds {
    bool found = false;
    ImVec2 minimum{};
    ImVec2 maximum{};
};

void IncludeColorVertex(FColorBounds& bounds, const ImDrawVert& vertex, ImU32 color) noexcept
{
    if (vertex.col != color) return;
    if (!bounds.found)
    {
        bounds.found = true;
        bounds.minimum = vertex.pos;
        bounds.maximum = vertex.pos;
        return;
    }
    bounds.minimum.x = vertex.pos.x < bounds.minimum.x ? vertex.pos.x : bounds.minimum.x;
    bounds.minimum.y = vertex.pos.y < bounds.minimum.y ? vertex.pos.y : bounds.minimum.y;
    bounds.maximum.x = vertex.pos.x > bounds.maximum.x ? vertex.pos.x : bounds.maximum.x;
    bounds.maximum.y = vertex.pos.y > bounds.maximum.y ? vertex.pos.y : bounds.maximum.y;
}
}

ACS_TEST(CinematicsTimelineEditor, PreservesCompatibilityAndRejectsNonFiniteEdits)
{
    static_assert(std::is_same_v<FCinematicsTimelineEditorPanel, ACinematicsTimelineEditorPanel>);
    static_assert(std::is_same_v<FEditorKeyframe, FCinematicTimelineKeyframe>);
    ACinematicsTimelineEditorPanel panel;
    CCinematicsDirector director;
    panel.Init();
    panel.SetCinematicsDirector(&director);
    panel.AddKeyframe(ETimelineKeyKind::CameraCut, 0.0f);
    EXPECT_EQ(director.KeyframeCount(), 1u);

    const f32 before_time = panel.CurrentTimeSec();
    const f32 before_duration = panel.DurationSec();
    const i32 before_selected = panel.SelectedKeyframeIndex();
    const f32 invalid_values[] = {
        std::numeric_limits<f32>::quiet_NaN(),
        std::numeric_limits<f32>::infinity(),
        -std::numeric_limits<f32>::infinity(),
    };
    for (const f32 invalid : invalid_values) {
        panel.SetCurrentTimeSec(invalid);
        panel.SetDurationSec(invalid);
        panel.AddKeyframe(ETimelineKeyKind::FadeColor, invalid);
        panel.Step(invalid);
    }
    EXPECT_EQ(panel.CurrentTimeSec(), before_time);
    EXPECT_EQ(panel.DurationSec(), before_duration);
    EXPECT_EQ(panel.SelectedKeyframeIndex(), before_selected);
    EXPECT_EQ(director.KeyframeCount(), 1u);
    panel.Step(0.1f);
    EXPECT_EQ(panel.CurrentTimeSec(), before_time);

    panel.SetDurationSec(-1.0f);
    EXPECT_EQ(panel.DurationSec(), ACinematicsTimelineEditorPanel::kMinDurationSec);
    panel.SetCurrentTimeSec(10.0f);
    EXPECT_EQ(panel.CurrentTimeSec(), ACinematicsTimelineEditorPanel::kMinDurationSec);
}

ACS_TEST(CinematicsTimelineEditor, StepAndFiniteOverflowRemainAtomic)
{
    ACinematicsTimelineEditorPanel panel;
    panel.Init();
    panel.SetDurationSec(std::numeric_limits<f32>::max());
    panel.Step(0.1f);
    EXPECT_EQ(panel.CurrentTimeSec(), 0.0f);
    panel.Step(0.1f);
    EXPECT_EQ(panel.CurrentTimeSec(), 0.0f);

    const f32 step = std::numeric_limits<f32>::max() * 0.75f;
    panel.Play();
    panel.Step(step);
    const f32 before = panel.CurrentTimeSec();
    panel.Step(step);
    EXPECT_EQ(panel.CurrentTimeSec(), before);
    EXPECT_TRUE(panel.IsPlaying());

    ACinematicsTimelineEditorPanel bound;
    CCinematicsDirector director;
    bound.Init();
    bound.SetCinematicsDirector(&director);
    bound.Step(0.1f);
    EXPECT_EQ(bound.CurrentTimeSec(), 0.0f);
    bound.Step(0.1f);
    EXPECT_EQ(bound.CurrentTimeSec(), 0.0f);
    EXPECT_FALSE(director.IsPlaying());
    bound.SetDurationSec(std::numeric_limits<f32>::max());
    bound.Play();
    bound.Step(step);
    const f32 director_before = director.CurrentTime();
    bound.SetCurrentTimeSec(0.0f);
    bound.Step(step);
    EXPECT_EQ(bound.CurrentTimeSec(), 0.0f);
    EXPECT_EQ(director.CurrentTime(), director_before);
    EXPECT_TRUE(bound.IsPlaying());
}

ACS_TEST(CinematicsTimelineEditor, DrawUiRetainsTimelineGeometry)
{
    CImGuiContextScope context;
    EXPECT_TRUE(ImGui::GetCurrentContext() != nullptr);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    EXPECT_TRUE(pixels != nullptr);
    EXPECT_TRUE(width > 0 && height > 0);
    ACinematicsTimelineEditorPanel panel;
    panel.Init();
    panel.AddKeyframe(ETimelineKeyKind::CameraCut, 0.0f);
    panel.AddKeyframe(ETimelineKeyKind::CameraCut, 1.0f);
    panel.AddKeyframe(ETimelineKeyKind::FadeColor, 2.0f);
    EXPECT_EQ(panel.SelectedKeyframeIndex(), 2);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(1000.0f, 600.0f), ImGuiCond_Always);
    panel.DrawUI();
    ImGui::Render();
    const ImDrawData* draw_data = ImGui::GetDrawData();
    EXPECT_TRUE(draw_data != nullptr);
    EXPECT_TRUE(draw_data->TotalVtxCount > 0);
    EXPECT_TRUE(draw_data->CmdListsCount > 0);
    FColorBounds canvas_bounds;
    FColorBounds ruler_bounds;
    FColorBounds even_row_bounds;
    FColorBounds odd_row_bounds;
    FColorBounds marker_bounds;
    FColorBounds selected_fill_bounds;
    FColorBounds cursor_bounds;
    FColorBounds selected_outline_bounds;
    FColorBounds nonselected_outline_bounds;
    i32 last_marker_vertex = -1;
    i32 last_cursor_vertex = -1;
    i32 vertex_index = 0;
    for (int list_index = 0; list_index < draw_data->CmdListsCount; ++list_index)
    {
        const ImDrawList* list = draw_data->CmdLists[list_index];
        for (const ImDrawVert& vertex : list->VtxBuffer)
        {
            const bool is_marker_color = vertex.col == IM_COL32(80, 200, 255, 255)
                || vertex.col == IM_COL32(255, 220, 100, 255)
                || vertex.col == IM_COL32(220, 100, 220, 255)
                || vertex.col == IM_COL32(100, 220, 120, 255)
                || vertex.col == IM_COL32(255, 150, 80, 255);
            if (is_marker_color) last_marker_vertex = vertex_index;
            if (vertex.col == IM_COL32(255, 80, 80, 220)) last_cursor_vertex = vertex_index;
            IncludeColorVertex(canvas_bounds, vertex, IM_COL32(30, 30, 35, 255));
            IncludeColorVertex(ruler_bounds, vertex, IM_COL32(40, 40, 48, 255));
            IncludeColorVertex(even_row_bounds, vertex, IM_COL32(32, 32, 38, 255));
            IncludeColorVertex(odd_row_bounds, vertex, IM_COL32(38, 38, 44, 255));
            IncludeColorVertex(marker_bounds, vertex, IM_COL32(80, 200, 255, 255));
            IncludeColorVertex(selected_fill_bounds, vertex, IM_COL32(255, 220, 100, 255));
            IncludeColorVertex(cursor_bounds, vertex, IM_COL32(255, 80, 80, 220));
            IncludeColorVertex(nonselected_outline_bounds, vertex, IM_COL32(10, 10, 10, 220));
            ++vertex_index;
        }
    }
    for (int list_index = 0; list_index < draw_data->CmdListsCount; ++list_index)
    {
        const ImDrawList* list = draw_data->CmdLists[list_index];
        for (const ImDrawVert& vertex : list->VtxBuffer)
        {
            const bool near_selected_fill = selected_fill_bounds.found
                && vertex.pos.x >= selected_fill_bounds.minimum.x - 3.0f
                && vertex.pos.x <= selected_fill_bounds.maximum.x + 3.0f
                && vertex.pos.y >= selected_fill_bounds.minimum.y - 3.0f
                && vertex.pos.y <= selected_fill_bounds.maximum.y + 3.0f;
            if (near_selected_fill)
            {
                IncludeColorVertex(selected_outline_bounds, vertex, IM_COL32(255, 255, 255, 255));
            }
        }
    }
    EXPECT_TRUE(canvas_bounds.found);
    EXPECT_TRUE(ruler_bounds.found);
    EXPECT_TRUE(even_row_bounds.found);
    EXPECT_TRUE(odd_row_bounds.found);
    EXPECT_TRUE(marker_bounds.found);
    EXPECT_TRUE(selected_fill_bounds.found);
    EXPECT_TRUE(cursor_bounds.found);
    EXPECT_TRUE(selected_outline_bounds.found);
    EXPECT_TRUE(nonselected_outline_bounds.found);
    EXPECT_TRUE(last_marker_vertex >= 0);
    EXPECT_TRUE(last_cursor_vertex > last_marker_vertex);
    EXPECT_NEAR(canvas_bounds.maximum.y - canvas_bounds.minimum.y, 22.0f + 2.0f + 5.0f * 28.0f + 4.0f, 0.1f);
    EXPECT_NEAR(ruler_bounds.maximum.y - ruler_bounds.minimum.y, 22.0f, 0.1f);
    EXPECT_NEAR(canvas_bounds.maximum.x - canvas_bounds.minimum.x, ruler_bounds.maximum.x - ruler_bounds.minimum.x, 0.1f);
    const f32 root_content_width = 1000.0f - 2.0f * ImGui::GetStyle().WindowPadding.x;
    const f32 expected_left_width = root_content_width - 260.0f - 8.0f;
    const f32 expected_canvas_width = expected_left_width - 4.0f;
    EXPECT_NEAR(canvas_bounds.maximum.x - canvas_bounds.minimum.x, expected_canvas_width, 0.1f);
    EXPECT_NEAR(even_row_bounds.maximum.y - even_row_bounds.minimum.y, 5.0f * 28.0f, 0.1f);
    EXPECT_NEAR(odd_row_bounds.maximum.y - odd_row_bounds.minimum.y, 3.0f * 28.0f, 0.1f);
    EXPECT_EQ(ACinematicsTimelineEditorPanel::kTrackCount, 5u);
    EXPECT_EQ(ACinematicsTimelineEditorPanel::kTrackRowHeightPx, 28.0f);
    EXPECT_EQ(ACinematicsTimelineEditorPanel::kMarkerWidthPx, 10.0f);

    // 固定したウィンドウ配置でStepボタン内の中心をクリックします。
    const ImVec2 step_click(190.0f, 38.0f);
    EXPECT_TRUE(step_click.x >= 150.0f && step_click.x <= 240.0f);
    EXPECT_TRUE(step_click.y >= 25.0f && step_click.y <= 55.0f);
    io.MousePos = step_click;
    // ボタン上へ移動した入力を1フレーム処理します。
    io.MouseDown[ImGuiMouseButton_Left] = false;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(1000.0f, 600.0f), ImGuiCond_Always);
    panel.DrawUI();
    ImGui::Render();
    // 押下入力を1フレーム処理します。
    io.MouseDown[ImGuiMouseButton_Left] = true;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(1000.0f, 600.0f), ImGuiCond_Always);
    panel.DrawUI();
    ImGui::Render();
    // 解放入力を1フレーム処理してボタン動作を確定します。
    io.MouseDown[ImGuiMouseButton_Left] = false;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(1000.0f, 600.0f), ImGuiCond_Always);
    panel.DrawUI();
    ImGui::Render();
    EXPECT_NEAR(panel.CurrentTimeSec(), 0.1f, 0.0001f);

    ACinematicsTimelineEditorPanel drag_panel;
    CCinematicsDirector drag_director;
    drag_panel.Init();
    drag_panel.AddKeyframe(ETimelineKeyKind::CameraCut, 1.0f);
    drag_panel.AddKeyframe(ETimelineKeyKind::FadeColor, 2.0f);
    drag_panel.SetCinematicsDirector(&drag_director);
    const ImVec2 marker_start(canvas_bounds.minimum.x + (canvas_bounds.maximum.x - canvas_bounds.minimum.x) * 0.1f, canvas_bounds.minimum.y + 22.0f + 2.0f + 14.0f);
    // キャンバス幅からドラッグ中の8秒位置を求めます。
    const f32 canvas_width = canvas_bounds.maximum.x - canvas_bounds.minimum.x;
    const ImVec2 drag_target(canvas_bounds.minimum.x + canvas_width * 0.8f, marker_start.y);
    // リリース時はキャンバス外を終端の10秒へクランプさせます。
    const ImVec2 release_target(canvas_bounds.maximum.x + 40.0f, marker_start.y);

    io.MousePos = marker_start;
    io.MouseDown[ImGuiMouseButton_Left] = false;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(1000.0f, 600.0f), ImGuiCond_Always);
    drag_panel.DrawUI();
    ImGui::Render();

    io.MouseDown[ImGuiMouseButton_Left] = true;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(1000.0f, 600.0f), ImGuiCond_Always);
    drag_panel.DrawUI();
    ImGui::Render();
    EXPECT_EQ(drag_director.TotalDuration(), 2.0f);

    io.MousePos = drag_target;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(1000.0f, 600.0f), ImGuiCond_Always);
    drag_panel.DrawUI();
    ImGui::Render();
    EXPECT_EQ(drag_director.TotalDuration(), 2.0f);

    io.MousePos = release_target;
    io.MouseDown[ImGuiMouseButton_Left] = false;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(1000.0f, 600.0f), ImGuiCond_Always);
    drag_panel.DrawUI();
    ImGui::Render();
    EXPECT_EQ(drag_panel.SelectedKeyframeIndex(), 1);
    EXPECT_NEAR(drag_panel.CurrentTimeSec(), 0.0f, 0.0001f);
    EXPECT_NEAR(drag_director.TotalDuration(), 10.0f, 0.0001f);
}
