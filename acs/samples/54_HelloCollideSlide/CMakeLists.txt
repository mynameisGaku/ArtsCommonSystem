# HelloCollideSlide - FPhysicsBody2D collide-and-slide の検証コンソール。
add_executable(hello_collide_slide main.cpp)
acs_apply_compiler_options(hello_collide_slide)
target_link_libraries(hello_collide_slide PRIVATE
    ACS::GameFramework ACS::Foundation ACS::Container ACS::Math)
