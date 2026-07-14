// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "imgui/ImGuiContext.h"
#include "platform/Window.h"
#include "render/Renderer.h"

#include <imgui.h>

using namespace acs;

ACS_TEST(ImGuiLifetime, InvalidInitRollsBackAndPreservesExternalContext)
{
    ImGuiContext* const external_context = ImGui::CreateContext();
    EXPECT_TRUE(external_context != nullptr);

    FWindow window;
    FRenderer renderer;
    ImGuiCtx context;
    EXPECT_TRUE(context.Init(window, renderer).IsErr());
    EXPECT_TRUE(ImGui::GetCurrentContext() == external_context);

    // 失敗後に部分状態が残っていなければ、再試行も同じ契約で失敗できる。
    EXPECT_TRUE(context.Init(window, renderer).IsErr());
    context.Shutdown();
    context.Shutdown();
    EXPECT_TRUE(ImGui::GetCurrentContext() == external_context);

    ImGui::DestroyContext(external_context);
}
