# HelloMeshCollider - 3D メッシュ → 三角形 BVH コライダーの検証コンソール。
add_executable(hello_mesh_collider main.cpp)
acs_apply_compiler_options(hello_mesh_collider)
target_link_libraries(hello_mesh_collider PRIVATE
    ACS::Collision ACS::Asset ACS::Foundation ACS::Container ACS::Math ACS::Memory)
