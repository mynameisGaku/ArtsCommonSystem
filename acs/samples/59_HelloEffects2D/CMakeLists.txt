# HelloEffects2D - Package D デモ: シェーダー無しインタラクティブエフェクト集。
add_executable(hello_effects2d WIN32
    EffectsDemo.cpp
)
acs_apply_compiler_options(hello_effects2d)
target_link_libraries(hello_effects2d PRIVATE ACS::GameFramework)
