# 「HelloSprite」サンプル: 2D スプライト + 矩形 + α ブレンド
add_executable(hello_sprite WIN32
    main.cpp
    HelloSpriteApp.cpp
    HelloSpriteApp.h
    Types.h
)
acs_apply_compiler_options(hello_sprite)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を
# "HelloSpriteApp.h" のように local include で書けるようにする)
target_include_directories(hello_sprite PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_sprite PRIVATE ACS::Game)
