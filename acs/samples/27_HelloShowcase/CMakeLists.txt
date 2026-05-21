# SPDX-License-Identifier: Apache-2.0
# Hello Showcase — PBR + IBL + ガラス屈折 (clear / frosted) + Post-process
# auto-orbit カメラの cinematic demo。HelloIbl から余計な toggle を全部捨てて
# 「画として綺麗な状態」だけを出す。
add_executable(hello_showcase WIN32 main.cpp)
acs_apply_compiler_options(hello_showcase)
target_link_libraries(hello_showcase PRIVATE ACS::Game)
