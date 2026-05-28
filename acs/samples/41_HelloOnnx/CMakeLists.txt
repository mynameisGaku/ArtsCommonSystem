# HelloOnnx - real ONNX Runtime smoke test.
add_executable(hello_onnx
    main.cpp
)
acs_apply_compiler_options(hello_onnx)
target_link_libraries(hello_onnx PRIVATE ACS::Game ACS::GameFramework ACS::MlOnnx)
acs_onnxruntime_runtime(hello_onnx)
