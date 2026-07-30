if(NOT DEFINED ACS_COMPILER_OPTIONS_FILE)
    message(FATAL_ERROR "ACS_COMPILER_OPTIONS_FILE が未指定です")
endif()

include("${ACS_COMPILER_OPTIONS_FILE}")

function(_acs_require_path_class target_root tree_root source_path expected)
    _acs_classify_source_path("${target_root}" "${tree_root}" "${source_path}"
                              _acs_actual)
    if(NOT _acs_actual STREQUAL expected)
        message(FATAL_ERROR
            "source path class mismatch: source=${source_path}, "
            "expected=${expected}, actual=${_acs_actual}")
    endif()
endfunction()

# 別 drive の外部プロジェクト・ACS・共有 source を決定的に分類する。
_acs_require_path_class(
    "C:/Project/Source" "W:/acs" "C:/Project/Source/Game.cpp" Target)
_acs_require_path_class(
    "C:/Project/Source" "W:/acs" "W:/acs/src/editor_abi/GameReflectShim.cpp" Engine)
_acs_require_path_class(
    "C:/Project/Source" "W:/acs" "D:/Shared/Generated.cpp" External)
_acs_require_path_class(
    "C:/Project/Source/../Source" "W:/acs/." "W:/acs/src/../src/render/Sky.cpp" Engine)

message(STATUS "compiler_options_path_classification=ok")
