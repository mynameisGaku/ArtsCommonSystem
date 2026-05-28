# HelloCrashReporter - real Windows DbgHelp minidump smoke test.
add_executable(hello_crash_reporter
    main.cpp
    CrashReporterDemoApp.cpp
    CrashReporterDemoApp.h
)
acs_apply_compiler_options(hello_crash_reporter)
target_link_libraries(hello_crash_reporter PRIVATE ACS::Game ACS::GameFramework ACS::CrashWin)

