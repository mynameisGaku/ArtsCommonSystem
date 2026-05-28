# HelloTelemetryFile - real file-backed telemetry smoke test.
add_executable(hello_telemetry_file
    main.cpp
    TelemetryFileDemoApp.cpp
    TelemetryFileDemoApp.h
)
acs_apply_compiler_options(hello_telemetry_file)
target_link_libraries(hello_telemetry_file PRIVATE ACS::Game ACS::GameFramework ACS::TelemetryFile)
