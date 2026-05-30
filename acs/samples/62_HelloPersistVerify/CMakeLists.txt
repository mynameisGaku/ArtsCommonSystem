# HelloPersistVerify - stub 撲滅 Batch 1 の永続化/シリアライズ本実装の round-trip 検証 (console)。
add_executable(hello_persist_verify main.cpp)
acs_apply_compiler_options(hello_persist_verify)
target_link_libraries(hello_persist_verify PRIVATE ACS::GameFramework)
