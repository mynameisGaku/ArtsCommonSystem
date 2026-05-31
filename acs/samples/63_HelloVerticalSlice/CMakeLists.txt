# HelloVerticalSlice — title→play→pause→game over→save の縦スライス。
# FScene2D + FUiLayer (クリック可能メニュー) + 自前 atlas + tilemap + 設定保存を
# 1 本に統合。座標は ACS 2D 正規規約 = Y-down (左上原点・+Y 下)。
add_executable(hello_vertical_slice WIN32
    main.cpp
)
acs_apply_compiler_options(hello_vertical_slice)
target_link_libraries(hello_vertical_slice PRIVATE ACS::GameFramework)
