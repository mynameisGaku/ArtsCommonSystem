# SPDX-License-Identifier: Apache-2.0
# Hello Showcase — PBR + IBL + ガラス屈折 (clear / frosted) + Post-process
# auto-orbit カメラの cinematic demo。HelloIbl から余計な toggle を全部捨てて
# 「画として綺麗な状態」だけを出す。
#
# 構成:
#   main.cpp            - ACS_DEFINE_MAIN(ShowcaseApp) のみ
#   ShowcaseTypes.h     - 配置定数 + Halton helper
#   ShowcaseApp.{h,cpp} - Application 派生クラス本体
add_executable(hello_showcase WIN32
    main.cpp
    ShowcaseApp.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    ShowcaseTypes.h
    ShowcaseApp.h
)
acs_apply_compiler_options(hello_showcase)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を
# "ShowcaseApp.h" のように local include で書けるようにする)
target_include_directories(hello_showcase PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_showcase PRIVATE ACS::Game)
# Solution Explorer 内で .cpp と .h を Source Files / Header Files にグループ化。
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
