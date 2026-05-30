# HelloStencilMask - 任意形状の stencil マスクで子ツリーをクリップするデモ。
add_executable(hello_stencil_mask WIN32
    StencilMaskDemo.cpp
)
acs_apply_compiler_options(hello_stencil_mask)
target_link_libraries(hello_stencil_mask PRIVATE ACS::GameFramework)
