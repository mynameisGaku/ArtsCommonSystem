# HelloSpritePhysics - スプライト凸包コライダー → FCollisionWorld2D 統合の検証コンソール。
add_executable(hello_sprite_physics main.cpp)
acs_apply_compiler_options(hello_sprite_physics)
target_link_libraries(hello_sprite_physics PRIVATE
    ACS::Collision ACS::GameFramework ACS::Foundation ACS::Container ACS::Math)
