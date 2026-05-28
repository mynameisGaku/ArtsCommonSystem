# HelloSpriteCollider - スプライト alpha → 形状コライダーの検証コンソール。
add_executable(hello_sprite_collider main.cpp)
acs_apply_compiler_options(hello_sprite_collider)
target_link_libraries(hello_sprite_collider PRIVATE
    ACS::Collision ACS::Foundation ACS::Container ACS::Math)
