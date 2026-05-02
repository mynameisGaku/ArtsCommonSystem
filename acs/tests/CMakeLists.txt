add_executable(acs_unit_tests
    main.cpp
    foundation_tests.cpp
    threading_tests.cpp
    memory_tests.cpp
    memory_system_tests.cpp
    container_tests.cpp
    math_tests.cpp
    ecs_tests.cpp
)

acs_apply_compiler_options(acs_unit_tests)

target_link_libraries(acs_unit_tests PRIVATE
    ACS::Foundation
    ACS::Threading
    ACS::Memory
    ACS::Container
    ACS::Math
    ACS::Ecs
    ACS::Test
)

add_test(NAME ACS.UnitTests COMMAND acs_unit_tests)
