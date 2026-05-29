# HelloColliderViz3D - 3D メッシュ/凸包コライダーの wireframe 目視確認デモ (windowed)。
add_executable(hello_collider_viz3d WIN32
    main.cpp
    Viz3DApp.cpp
    Viz3DApp.h
)
acs_apply_compiler_options(hello_collider_viz3d)
target_include_directories(hello_collider_viz3d PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_collider_viz3d PRIVATE ACS::Game ACS::Collision)
