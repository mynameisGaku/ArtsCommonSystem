# HelloLocalMatchmaker - real local IMatchmaker smoke test.
add_executable(hello_local_matchmaker
    main.cpp
    LocalMatchmakerDemoApp.cpp
    LocalMatchmakerDemoApp.h
)
acs_apply_compiler_options(hello_local_matchmaker)
target_link_libraries(hello_local_matchmaker PRIVATE ACS::Game ACS::GameFramework ACS::LocalMatch)
