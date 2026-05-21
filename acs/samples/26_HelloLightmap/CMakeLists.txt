# 「Hello Lightmap」サンプル: Phase 33f-2 — 静的ライトマップ baker
# Cornell box シーンを CPU で 1-bounce ベイクし、PbrShader の lightmap slot
# (Phase 33f で追加した t8) 経由で表示する。
add_executable(hello_lightmap WIN32 main.cpp)
acs_apply_compiler_options(hello_lightmap)
target_link_libraries(hello_lightmap PRIVATE ACS::Game)
