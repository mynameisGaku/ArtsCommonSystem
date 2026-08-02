# HelloScene2D - AScene2D starter foundation sample.
add_executable(hello_scene2d WIN32
    Scene2DStarter.cpp
)
acs_apply_compiler_options(hello_scene2d)
target_link_libraries(hello_scene2d PRIVATE ACS::GameFramework)
