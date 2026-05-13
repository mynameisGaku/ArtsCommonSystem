# 「Hello IBL」サンプル: Phase 31 — Image-Based Lighting
# まずは BRDF LUT 可視化のみ。Step 3 以降で環境 cubemap / irradiance / prefilter
# を追加し、PbrShader 経由で sphere grid を IBL 描画するよう拡張する。
add_executable(hello_ibl WIN32 main.cpp)
acs_apply_compiler_options(hello_ibl)
target_link_libraries(hello_ibl PRIVATE
    ACS::Foundation
    ACS::Threading
    ACS::Memory
    ACS::Container
    ACS::Math
    ACS::Platform
    ACS::Asset
    ACS::Ecs
    ACS::Render
    ACS::App
)
