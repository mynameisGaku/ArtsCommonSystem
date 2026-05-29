# HelloColliderViz2D - スプライトコライダー (凸包 + 凹輪郭) の目視確認デモ (windowed)。
add_executable(hello_collider_viz2d WIN32
    main.cpp
    ColliderVizApp.cpp
    ColliderVizApp.h
)
acs_apply_compiler_options(hello_collider_viz2d)
target_include_directories(hello_collider_viz2d PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_collider_viz2d PRIVATE ACS::Game ACS::GameFramework ACS::Collision)
