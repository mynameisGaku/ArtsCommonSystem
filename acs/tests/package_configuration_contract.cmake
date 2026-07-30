if(NOT DEFINED ACS_CPACK_CONFIG OR NOT EXISTS "${ACS_CPACK_CONFIG}")
    message(FATAL_ERROR "CPackConfig.cmake が見つかりません")
endif()
if(NOT DEFINED ACS_INSTALL_SCRIPT OR NOT EXISTS "${ACS_INSTALL_SCRIPT}")
    message(FATAL_ERROR "cmake_install.cmake が見つかりません")
endif()

include("${ACS_CPACK_CONFIG}")

if(NOT CPACK_COMPONENTS_ALL STREQUAL "ACSGameRuntime")
    message(FATAL_ERROR "package component が ACSGameRuntime に限定されていません")
endif()
if(NOT CPACK_INSTALL_CMAKE_PROJECTS MATCHES ";ACSGameRuntime;/")
    message(FATAL_ERROR "CPack install project が runtime component を指定していません")
endif()
if(NOT CPACK_ARCHIVE_COMPONENT_INSTALL)
    message(FATAL_ERROR "archive component install が無効です")
endif()
if(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OR CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY)
    message(FATAL_ERROR "ZIP に不要な最上位directoryが追加されます")
endif()

file(READ "${ACS_INSTALL_SCRIPT}" _acs_install_script)
if(_acs_install_script MATCHES "acs_(diligent_core|mimalloc)-build[/\\\\]cmake_install\\.cmake")
    message(FATAL_ERROR "private依存の install 規則が親projectへ漏れています")
endif()

set(_acs_required_licenses
    ACS-License.txt
    stb-License.txt
    cgltf-License.txt
    ufbx-License.txt
    dr_libs-License.txt
    mimalloc-License.txt)
if(ACS_EXPECT_IMGUI_LICENSE)
    list(APPEND _acs_required_licenses DearImGui-License.txt)
endif()
if(ACS_EXPECT_DILIGENT_LICENSES)
    list(APPEND _acs_required_licenses
        DiligentCore-License.txt
        xxHash-License.txt
        DXC-License.txt
        DXC-ThirdPartyNotices.txt
        GPUOpenShaderUtils-License.txt)
endif()
foreach(_acs_license ${_acs_required_licenses})
    if(NOT _acs_install_script MATCHES "RENAME \"${_acs_license}\"")
        message(FATAL_ERROR "runtime license が未登録です: ${_acs_license}")
    endif()
endforeach()

message(STATUS "package_configuration_contract=ok")
