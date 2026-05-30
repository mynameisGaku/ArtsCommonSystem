# HelloWaterTopDown - 見下ろし (Core Keeper 風) の水面デモ。
add_executable(hello_water_topdown WIN32
    WaterTopDownDemo.cpp
)
acs_apply_compiler_options(hello_water_topdown)
target_link_libraries(hello_water_topdown PRIVATE ACS::GameFramework)
