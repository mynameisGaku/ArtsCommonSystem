# HelloConvexHull - 3D 凸包生成 + 凸コライダー化の検証コンソール。
add_executable(hello_convex_hull main.cpp)
acs_apply_compiler_options(hello_convex_hull)
target_link_libraries(hello_convex_hull PRIVATE
    ACS::Collision ACS::Asset ACS::Foundation ACS::Container ACS::Math ACS::Memory)
