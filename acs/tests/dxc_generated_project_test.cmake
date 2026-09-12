# SPDX-License-Identifier: Apache-2.0
# 実テンプレートをCMakeとして評価する。ACS本体は構築せず配置helperだけを実物へ接続する。
include("${ACS_CMAKE_DIR}/ACSDxc.cmake")
add_library(ACS::GameFramework INTERFACE IMPORTED)
add_library(ACS::AssetPack INTERFACE IMPORTED)
set(ACS_SOURCE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/engine-stub")
file(MAKE_DIRECTORY "${ACS_SOURCE_ROOT}/editor_abi")
file(WRITE "${ACS_SOURCE_ROOT}/editor_abi/GameReflectShim.cpp" "// 独立fixture用shim。製品のEditorABIは構築しない。\nextern \"C\" __declspec(dllexport) int ReflectFixture() { return 0; }\n")

# ユーザーsourceを独立してcompileするための最小設定。
function(acs_apply_compiler_options target)
    target_compile_options(${target} PRIVATE /utf-8)
endfunction()

add_subdirectory("${GENERATED_SOURCE_DIR}" project)
foreach(target IN ITEMS DxcFixture DxcFixture_reflect)
    get_target_property(registered ${target} ACS_DXC_RUNTIME_REGISTERED)
    if(ACS_RENDER_DX12_RAW AND NOT registered)
        message(SEND_ERROR "DXC_GENERATED_MISSING_RUNTIME: ${target}")
    elseif(NOT ACS_RENDER_DX12_RAW AND registered)
        message(SEND_ERROR "DXC_GENERATED_UNEXPECTED_RUNTIME: ${target}")
    endif()
endforeach()
