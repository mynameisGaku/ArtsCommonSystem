# HelloTilemap - Package C デモ: タイルマップ描画 + フェード付きシーン遷移。
add_executable(hello_tilemap WIN32
    TilemapDemo.cpp
)
acs_apply_compiler_options(hello_tilemap)
target_link_libraries(hello_tilemap PRIVATE ACS::GameFramework)
