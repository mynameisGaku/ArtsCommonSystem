# HelloOpenXR - real Khronos OpenXR loader smoke test.
add_executable(hello_openxr
    main.cpp
    OpenXrDemoApp.cpp
    OpenXrDemoApp.h
)
acs_apply_compiler_options(hello_openxr)
target_link_libraries(hello_openxr PRIVATE ACS::Game ACS::GameFramework ACS::OpenXr)
