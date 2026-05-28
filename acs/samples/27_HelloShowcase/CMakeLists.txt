# SPDX-License-Identifier: Apache-2.0
# Hello Showcase — PBR + IBL + ガラス屈折 (clear / frosted) + Post-process
# auto-orbit カメラの cinematic demo。HelloIbl から余計な toggle を全部捨てて
# 「画として綺麗な状態」だけを出す。
#
# 構成 (pass 系 helper に分割):
#   main.cpp                          - ACS_DEFINE_MAIN(ShowcaseApp) のみ
#   ShowcaseTypes.h                   - 配置定数 + Halton helper
#   ShowcaseAssets.{h,cpp}            - GPU resource bundle + 初期化 / 解放
#   PbrPass.{h,cpp}                   - Sky + 床 + opaque sphere + emissive orb
#   RefractionPass.{h,cpp}            - ガラス球 (clear / frosted)
#   MotionPass.{h,cpp}                - motion + normal G-buffer
#   SsrPass.{h,cpp}                   - Hi-Z + SSR + SSAO
#   BloomPass.{h,cpp}                 - PostProcess (Bloom + ACES + TAA)
#   HudPass.{h,cpp}                   - HUD overlay
#   ShowcaseApp.{h,cpp}               - Application 派生 (上記 pass を順に駆動)
add_executable(hello_showcase WIN32
    main.cpp
    ShowcaseApp.cpp
    ShowcaseAssets.cpp
    PbrPass.cpp
    RefractionPass.cpp
    MotionPass.cpp
    SsrPass.cpp
    BloomPass.cpp
    HudPass.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    ShowcaseTypes.h
    ShowcaseAssets.h
    PbrPass.h
    RefractionPass.h
    MotionPass.h
    SsrPass.h
    BloomPass.h
    HudPass.h
    ShowcaseApp.h
)
acs_apply_compiler_options(hello_showcase)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を
# "ShowcaseApp.h" のように local include で書けるようにする)
target_include_directories(hello_showcase PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_showcase PRIVATE ACS::Game)
