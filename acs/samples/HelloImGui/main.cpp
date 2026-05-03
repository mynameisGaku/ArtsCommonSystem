// ACS の Hello ImGui サンプル
//
// 動作:
//   ・ウィンドウを開いて ImGui のデモウィンドウを表示
//   ・自前ウィンドウで FPS 表示 + スライダーで背景色変更
//   ・Esc で終了

#include "app/Application.h"
#include "app/EntryPoint.h"
#include "platform/Input.h"
#include "imgui/ImGuiContext.h"

#include <imgui.h>

using namespace acs;

class HelloImGui : public Application {
public:
    void OnStart() noexcept override {
        // ImGui を初期化（Window と Renderer に紐付け）
        auto r = _imgui.Init(GetWindow(), GetRenderer());
        if (r.IsErr()) {
            Quit();
            return;
        }
    }

    void OnUpdate(f32 /*dt*/) noexcept override {
        if (Input::IsKeyPressed(Key::Escape)) Quit();
    }

    void OnRender() noexcept override {
        // ImGui の新フレーム開始（OnRender は BeginFrame の後に呼ばれる）
        _imgui.NewFrame();

        // デモウィンドウ（ImGui の機能網羅サンプル）
        if (_show_demo) ImGui::ShowDemoWindow(&_show_demo);

        // 自前のコントロールウィンドウ
        ImGui::Begin("ACS Sample");
        ImGui::Text("FPS: %.1f", FPS());
        ImGui::Text("Frames: %llu", static_cast<unsigned long long>(FrameCount()));
        ImGui::Checkbox("Show ImGui demo", &_show_demo);
        ImGui::SliderFloat("R", &_r, 0.0f, 1.0f);
        ImGui::SliderFloat("G", &_g, 0.0f, 1.0f);
        ImGui::SliderFloat("B", &_b, 0.0f, 1.0f);
        if (ImGui::Button("Quit")) Quit();
        ImGui::End();

        // ImGui の描画コマンドをコマンドリストに発行
        _imgui.Render();
    }

    void OnShutdown() noexcept override {
        _imgui.Shutdown();
    }

    void OnEvent(const Event& e) noexcept override {
        _imgui.OnEvent(e);
    }

private:
    ImGuiCtx _imgui;
    bool     _show_demo = true;
    f32      _r = 0.1f;
    f32      _g = 0.12f;
    f32      _b = 0.16f;
};

ACS_DEFINE_MAIN(HelloImGui)
