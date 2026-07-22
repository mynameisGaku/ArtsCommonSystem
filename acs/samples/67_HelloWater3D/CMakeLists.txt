# Interactive 3D water: persistent ripples, normal mapping, Fresnel reflection,
# scene-color refraction, Beer-Lambert absorption, foam, HDR bloom, and ACES.
add_executable(hello_water_3d WIN32
    main.cpp
    HelloWater3DApp.cpp
    HelloWater3DApp.h
)
acs_apply_compiler_options(hello_water_3d)
target_include_directories(hello_water_3d PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_water_3d PRIVATE ACS::Game)
