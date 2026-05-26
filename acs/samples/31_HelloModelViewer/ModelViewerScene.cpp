// SPDX-License-Identifier: Apache-2.0
// HelloModelViewer — ModelViewerScene 実装。
#include "ModelViewerScene.h"
#include "ViewerCubeAssets.h"

#include "render/Renderer.h"
#include "render/IRhiSwapchain.h"
#include "platform/Input.h"
#include "foundation/Log.h"
#include "math/Mat.h"
#include "math/Math.h"

#include <imgui.h>

using namespace acs;
using namespace acs::game;

namespace hellomv {

// ----------------------------------------------------------------------------
// OnEnter — theme/workspace/asset_browser/panel 群を順に初期化
// ----------------------------------------------------------------------------
void ModelViewerScene::OnEnter() noexcept {
    // editor らしいニュートラルグレー (背景は ImGui に隠れるが Quad 描画の余白
    // (= viewport の外側) のクリア色を編集向けに揃える)。
    GetGame().SetClearColor(0.15f, 0.15f, 0.18f);

    // ---- Theme: ImGui context 取得後に Init (= Dark preset) ----
    // ImGuiCtx::Init は ModelViewerApp::OnStart で済んでいるはず。
    _theme.Init();
    _theme.ApplyPreset(editor_core::EEditorThemePreset::Dark);

    // ---- Workspace 本体 ----
    _workspace.Init();

    // ---- AssetBrowser: assets/ ルートを起点に列挙 ----
    _asset_browser.Init(kAssetRoot);
    _asset_browser.Refresh();

    // ---- 4 modelview panel を workspace に登録 ----
    // EditorWorkspace::RegisterPanel は内部で `panel->OnInit(*this)` を呼ぶ
    // (EditorWorkspace.cpp §119)。したがって panel.OnInit を別途呼ぶ必要は無い
    // (= 二重呼出しを誘発する)。以降の MainLoop 内で TickAllPanels が
    // OnFrameBegin → DrawUI を順に回す。
    _workspace.RegisterPanel(&_viewer_panel);
    _workspace.RegisterPanel(&_info_panel);
    _workspace.RegisterPanel(&_material_panel);
    _workspace.RegisterPanel(&_animation_panel);

    // AssetBrowser 自体は EditorPanel 派生ではないので Workspace 登録はせず、
    // OnRender 内で直接 DrawUI() を呼ぶ。EditorPanel ラッパが Phase 21c で
    // 追加されたらここを `_workspace.RegisterPanel(&_asset_browser_panel)` に
    // 差し替え予定。

    // ---- 3D resource 初期化 (sample 17 と同形) ----
    IRhiDevice* dev = GetGame().GetRenderer().Device();
    if (!dev) {
        ACS_LOG_ERROR("[ModelViewer] Device() == null -> Quit");
        GetGame().Quit();
        return;
    }

    // VS / PS
    ShaderDesc vs_desc{};
    vs_desc.stage       = EShaderStage::Vertex;
    vs_desc.hlsl_source = kHLSL;
    vs_desc.entry_point = "VSMain";
    vs_desc.debug_name  = "ModelViewer.VS";
    if (auto r = CreateRhiShader(*dev, vs_desc); r.IsErr()) {
        ACS_LOG_ERROR("[ModelViewer] VS compile failed");
        GetGame().Quit(); return;
    } else { _vs = Move(r.Value()); }

    ShaderDesc ps_desc{};
    ps_desc.stage       = EShaderStage::Pixel;
    ps_desc.hlsl_source = kHLSL;
    ps_desc.entry_point = "PSMain";
    ps_desc.debug_name  = "ModelViewer.PS";
    if (auto r = CreateRhiShader(*dev, ps_desc); r.IsErr()) {
        ACS_LOG_ERROR("[ModelViewer] PS compile failed");
        GetGame().Quit(); return;
    } else { _ps = Move(r.Value()); }

    // VB / IB
    BufferDesc vb_desc{};
    vb_desc.size = sizeof(kCubeVertices);
    vb_desc.usage = EBufferUsage::Vertex;
    vb_desc.cpu_writable = true;
    vb_desc.initial_data = kCubeVertices;
    if (auto r = CreateRhiBuffer(*dev, vb_desc); r.IsErr()) {
        ACS_LOG_ERROR("[ModelViewer] VB create failed");
        GetGame().Quit(); return;
    } else { _vb = Move(r.Value()); }

    BufferDesc ib_desc{};
    ib_desc.size = sizeof(kCubeIndices);
    ib_desc.usage = EBufferUsage::Index16;
    ib_desc.cpu_writable = true;
    ib_desc.initial_data = kCubeIndices;
    if (auto r = CreateRhiBuffer(*dev, ib_desc); r.IsErr()) {
        ACS_LOG_ERROR("[ModelViewer] IB create failed");
        GetGame().Quit(); return;
    } else { _ib = Move(r.Value()); }

    // 定数バッファ (MVP 1 個分、256B アライン)。
    BufferDesc cb_desc{};
    cb_desc.size = 256;
    cb_desc.usage = EBufferUsage::Uniform;
    cb_desc.cpu_writable = true;
    if (auto r = CreateRhiBuffer(*dev, cb_desc); r.IsErr()) {
        ACS_LOG_ERROR("[ModelViewer] CB create failed");
        GetGame().Quit(); return;
    } else { _cb = Move(r.Value()); }

    // Pipeline
    PipelineDesc pd{};
    pd.vs = _vs.Get();
    pd.ps = _ps.Get();
    pd.topology         = EPrimitiveTopology::TriangleList;
    pd.rt_format        = GetGame().GetRenderer().ColorFormat();
    pd.depth_format     = GetGame().GetRenderer().DepthFormat();
    pd.depth_test       = true;
    pd.depth_write      = true;
    pd.cull_mode        = ECullMode::Back;
    pd.cbuffer_slots    = 1;
    pd.cbuffer_names[0] = "Frame";
    pd.vertex_stride    = sizeof(Vertex);
    pd.layout[0] = { "POSITION", 0, EFormat::R32G32B32_Float, 0 };
    pd.layout[1] = { "COLOR",    0, EFormat::R32G32B32_Float, sizeof(f32) * 3 };
    pd.layout_count = 2;
    if (auto r = CreateRhiPipeline(*dev, pd); r.IsErr()) {
        ACS_LOG_ERROR("[ModelViewer] Pipeline create failed");
        GetGame().Quit(); return;
    } else { _pipeline = Move(r.Value()); }

    // ---- default model load (= cube を表示中の current model として扱う stub) ----
    // 実 model loader (glTF) は Phase 21c。本 sample では cube を「常駐 default
    // model」として扱い、各 panel に "(default cube)" を伝える設計。
    ACS_LOG_INFO("[ModelViewer] default model loaded (stub): %s", kDefaultModelPath);

    ACS_LOG_INFO("[ModelViewer] entered (workspace + 4 panel + asset browser + theme + cube)");
}

// ----------------------------------------------------------------------------
// OnExit — 逆順 shutdown (panel → asset browser → workspace → theme は no-op)
// ----------------------------------------------------------------------------
void ModelViewerScene::OnExit() noexcept {
    // GPU リソースは Application::Quit() → WaitIdle が走った後、Scene デストラクタ
    // 前のここで明示解放。CmdList が次フレームを参照し続けないよう Pipeline /
    // Buffer / Shader をリリース。
    if (GetGame().GetRenderer().Device()) {
        GetGame().GetRenderer().Device()->WaitIdle();
    }
    _pipeline.Reset();
    _cb.Reset();
    _ib.Reset();
    _vb.Reset();
    _ps.Reset();
    _vs.Reset();

    // EditorWorkspace::Shutdown は登録済み全 panel に OnShutdown を 1 度ずつ
    // 呼んでから list を Clear する (EditorWorkspace.cpp §76)。よって個別
    // UnregisterPanel を呼ぶ必要は無い (= 二重 OnShutdown を避けるため)。
    _workspace.Shutdown();

    _asset_browser.Shutdown();
    // EditorTheme::Shutdown は存在しない API なので明示解放は不要 (Dtor で十分)。

    ACS_LOG_INFO("[ModelViewer] exited");
}

// ----------------------------------------------------------------------------
// OnUpdate — 非 ImGui 系のロジックだけ (animation tick / 入力 / カメラ MVP 計算)
// ----------------------------------------------------------------------------
// ※ ImGui 関連 (Workspace::TickAllPanels が呼ぶ DrawDockSpace / MenuBar / panel
//   DrawUI) はすべて OnRender 側へ。`ImGui::Begin` 等は NewFrame() と Render()
//   の間でしか呼べないため、ここで Workspace::TickAllPanels は呼ばない。
//   各 panel の `OnFrameBegin` は Workspace::TickAllPanels から OnRender で
//   呼ばれるので、ロジック系 hook も結果的に正しい順序で回る。
void ModelViewerScene::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) {
        GetGame().Quit();
        return;
    }

    // dt スパイク防御 (Application 側の大 dt で animation/回転が暴れない)。
    if (dt > 0.1f) dt = 0.1f;

    // ---- animation 時間を進める (ModelAnimationPanel の責務) ----
    // frame callback が pose を適用するのは Phase 21c。本 sample では Tick だけ
    // 呼んで、actual pose 適用は stub。Tick は ImGui を触らない契約のため
    // OnUpdate で OK。
    _animation_panel.Tick(dt);

    // ---- 3D cube の自転 (sample 17 と同形) ----
    _angle += dt * 0.8f;

    // ---- MVP 更新: ModelViewerPanel::Camera() の view/proj を使う ----
    // (= editor 上のマウスドラッグで orbit / dolly した姿勢がそのまま 3D 像に
    //  反映される。EditorCamera の Tick (smooth target) は OnRender 側の
    //  TickAllPanels → OnFrameBegin から呼ばれる想定。)
    editor_core::EditorCamera& cam = _viewer_panel.Camera();
    const Mat4 view = cam.ViewMatrix();
    const f32  aspect = static_cast<f32>(GetGame().GetRenderer().Swapchain()->Width()) /
                        static_cast<f32>(GetGame().GetRenderer().Swapchain()->Height());
    const Mat4 proj = cam.ProjectionMatrix(aspect, 0.1f, 100.0f);
    const Mat4 model = Mat4::RotationY(_angle);
    const Mat4 mvp = model * view * proj;
    _cb->Update(&mvp, sizeof(Mat4));
}

// ----------------------------------------------------------------------------
// OnRender — 3D cube → ImGui (File menu → Workspace 全描画 → AssetBrowser → Theme)
// ----------------------------------------------------------------------------
void ModelViewerScene::OnRender(RenderContext& rc) noexcept {
    // ---- (1) 3D scene 描画 (cube only、sample 17 と同形) ----
    // 同じコマンドリスト上で ImGui より前に出すことで、ImGui ウィンドウが
    // viewport をオーバーレイで覆える (= フレームバッファ直書きの背景レイヤ)。
    IRhiCommandList& cl = rc.Cmd();
    if (_pipeline) {
        cl.SetPipeline(*_pipeline);
        cl.SetConstantBuffer(0, *_cb);
        cl.SetVertexBuffer(*_vb, sizeof(Vertex));
        cl.SetIndexBuffer(*_ib);
        cl.DrawIndexed(36);
    }

    // ---- (2) File メニュー (Workspace::DrawMenuBar の前に push する) ----
    // ImGui は同一フレーム内で BeginMainMenuBar を複数回呼んでも 1 個の bar に
    // マージするので、本 sample 専用の File メニューを Workspace の Window/
    // Layout メニューと並べて表示できる。
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Model...")) {
                // Phase 21b: 実ダイアログは未配線。callback hook だけ走らせる。
                ACS_LOG_INFO("[ModelViewer] Open Model... -> '%s' (stub, no-op)", kDefaultModelPath);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save Layout")) {
                _workspace.SaveLayout(kLayoutPath);
                ACS_LOG_INFO("[ModelViewer] Save Layout -> 'data/editor/modelviewer.acslayout'");
            }
            if (ImGui::MenuItem("Load Layout")) {
                _workspace.LoadLayout(kLayoutPath);
                ACS_LOG_INFO("[ModelViewer] Load Layout <- 'data/editor/modelviewer.acslayout'");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Theme...")) {
                // Theme Settings window を toggle。連打で開閉。
                _show_theme_settings = !_show_theme_settings;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Esc")) {
                GetGame().Quit();
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // ---- (3) Workspace 全描画 (1 行で OnFrameBegin → DockSpace → MenuBar →
    //         各 panel DrawUI を順に発火) ----
    // EditorWorkspace::TickAllPanels は ImGui 系の Draw 呼出を含むため OnRender
    // の中で呼ぶ。仕様 (EditorWorkspace.h §「メインループ」) は:
    //   1) 各 panel.OnFrameBegin(dt)
    //   2) DrawDockSpace()
    //   3) DrawMenuBar() — File メニューと同じ MainMenuBar に Window/Layout を push
    //   4) 各 panel.DrawUI()
    // よって本 sample 側で個別 DrawDockSpace / DrawMenuBar / panel.DrawUI を
    // 呼ぶ必要は無い。dt は RenderContext からは取れないので Game の DeltaTime()
    // を使う。
    _workspace.TickAllPanels(GetGame().DeltaTime());

    // ---- (4) AssetBrowser を直接描画 (EditorPanel 派生ではないため Workspace
    //         登録対象外) ----
    _asset_browser.DrawUI();

    // ---- (5) Theme Settings 窓 (File > Theme... で toggle) ----
    if (_show_theme_settings) {
        _theme.DrawThemeSettingsUI();
        // DrawThemeSettingsUI 内に Save/Load ボタンはあるが、固定パスで明示
        // 保存できるショートカットを別 window で出す (本 sample 専用のクイック
        // アクション)。
        if (ImGui::Begin("Theme Quick Actions")) {
            if (ImGui::Button("Save theme to file")) {
                _theme.SaveTheme(kThemePath);
                ACS_LOG_INFO("[ModelViewer] Theme saved -> 'data/editor/theme.acstheme'");
            }
            ImGui::SameLine();
            if (ImGui::Button("Load theme from file")) {
                _theme.LoadTheme(kThemePath);
                ACS_LOG_INFO("[ModelViewer] Theme loaded <- 'data/editor/theme.acstheme'");
            }
        }
        ImGui::End();
    }
}

} // namespace hellomv
