# HelloSpriteAnim - Package A デモ: スプライトシートアニメ + HUD テキスト。
add_executable(hello_sprite_anim WIN32
    SpriteAnimDemo.cpp
)
acs_apply_compiler_options(hello_sprite_anim)
target_link_libraries(hello_sprite_anim PRIVATE ACS::GameFramework)
