# ACS Module System — UE-inspired modular build framework.
#
# A "module" is a directory under src/ that contains a Module.cmake file.
# Each Module.cmake calls acs_module() to declare itself, optionally
# acs_module_feature() for opt-in compile-time features.
#
# Top-level flow:
#   1. acs_discover_modules(src_root)  — scans src/*/Module.cmake
#   2. include(modules.cmake)          — user-editable enablement
#   3. acs_finalize_modules()          — validates deps, creates targets,
#                                        applies WITH_ACS_<MOD> defines and
#                                        per-feature WITH_ACS_<FEATURE> defines.
include_guard(GLOBAL)

# ---- Module declaration --------------------------------------------------
# Called by src/<mod>/Module.cmake:
#   acs_module(
#       NAME            Foundation
#       TYPE            Runtime              # Runtime / Editor / Test / etc.
#       SOURCES         Assert.cpp Log.cpp ...
#       HEADERS         Types.h Compiler.h ...
#       PUBLIC_DEPS     Foundation
#       PRIVATE_DEPS
#       LINK_PUBLIC                          # external libs (e.g. dbghelp)
#       LINK_PRIVATE
#   )
function(acs_module)
    cmake_parse_arguments(M "" "NAME;TYPE"
        "SOURCES;HEADERS;PUBLIC_DEPS;PRIVATE_DEPS;LINK_PUBLIC;LINK_PRIVATE" ${ARGN})
    if(NOT M_NAME)
        message(FATAL_ERROR "acs_module: NAME is required")
    endif()
    if(NOT M_TYPE)
        set(M_TYPE "Runtime")
    endif()

    set_property(GLOBAL APPEND PROPERTY _ACS_AVAILABLE_MODULES "${M_NAME}")
    set_property(GLOBAL PROPERTY _ACS_MOD_${M_NAME}_TYPE          "${M_TYPE}")
    set_property(GLOBAL PROPERTY _ACS_MOD_${M_NAME}_SOURCES       "${M_SOURCES}")
    set_property(GLOBAL PROPERTY _ACS_MOD_${M_NAME}_HEADERS       "${M_HEADERS}")
    set_property(GLOBAL PROPERTY _ACS_MOD_${M_NAME}_PUBLIC_DEPS   "${M_PUBLIC_DEPS}")
    set_property(GLOBAL PROPERTY _ACS_MOD_${M_NAME}_PRIVATE_DEPS  "${M_PRIVATE_DEPS}")
    set_property(GLOBAL PROPERTY _ACS_MOD_${M_NAME}_LINK_PUBLIC   "${M_LINK_PUBLIC}")
    set_property(GLOBAL PROPERTY _ACS_MOD_${M_NAME}_LINK_PRIVATE  "${M_LINK_PRIVATE}")
    set_property(GLOBAL PROPERTY _ACS_MOD_${M_NAME}_SOURCE_DIR    "${CMAKE_CURRENT_LIST_DIR}")
endfunction()

# ---- Per-module compile-time feature -------------------------------------
# Defines a CMake option ACS_<MOD>_<NAME> that drives a WITH_ACS_<DEFINE>
# preprocessor define on the module target.
function(acs_module_feature)
    cmake_parse_arguments(F "" "MODULE;NAME;DEFINE;DESCRIPTION;DEFAULT" "" ${ARGN})
    if(NOT F_DEFINE)
        set(F_DEFINE "ACS_${F_MODULE}_${F_NAME}")
    endif()
    if(NOT DEFINED F_DEFAULT)
        set(F_DEFAULT ON)
    endif()
    set(opt "ACS_${F_MODULE}_${F_NAME}")
    if(NOT DEFINED ${opt})
        option(${opt} "${F_DESCRIPTION}" ${F_DEFAULT})
    endif()
    set_property(GLOBAL APPEND PROPERTY _ACS_MOD_${F_MODULE}_FEATURES
                 "${F_NAME}|WITH_${F_DEFINE}|${opt}")
endfunction()

# ---- Discover modules under a root directory ------------------------------
function(acs_discover_modules src_root)
    file(GLOB child_dirs RELATIVE "${src_root}" "${src_root}/*")
    foreach(d ${child_dirs})
        set(mod_file "${src_root}/${d}/Module.cmake")
        if(EXISTS "${mod_file}")
            include("${mod_file}")
        endif()
    endforeach()
endfunction()

# ---- User-facing module enablement ---------------------------------------
# In modules.cmake the user writes:
#   acs_enable_module(Foundation REQUIRED)
#   acs_enable_module(Math FEATURES AVX2)
#   acs_enable_module(Render DISABLED_FEATURES BGFX_BACKEND)
function(acs_enable_module mod_name)
    cmake_parse_arguments(E "REQUIRED" "" "FEATURES;DISABLED_FEATURES" ${ARGN})
    get_property(avail GLOBAL PROPERTY _ACS_AVAILABLE_MODULES)
    list(FIND avail "${mod_name}" idx)
    if(idx EQUAL -1)
        if(E_REQUIRED)
            message(FATAL_ERROR "ACS: required module '${mod_name}' is not available\n"
                    "  Available modules: ${avail}")
        endif()
        message(WARNING "ACS: requested module '${mod_name}' not registered — skipping")
        return()
    endif()
    set_property(GLOBAL APPEND PROPERTY _ACS_ENABLED_MODULES "${mod_name}")
    foreach(feat ${E_FEATURES})
        set("ACS_${mod_name}_${feat}" ON CACHE BOOL "" FORCE)
    endforeach()
    foreach(feat ${E_DISABLED_FEATURES})
        set("ACS_${mod_name}_${feat}" OFF CACHE BOOL "" FORCE)
    endforeach()
endfunction()

# ---- Resolve, validate, and create targets --------------------------------
function(acs_finalize_modules)
    get_property(enabled  GLOBAL PROPERTY _ACS_ENABLED_MODULES)
    get_property(avail    GLOBAL PROPERTY _ACS_AVAILABLE_MODULES)

    if(NOT enabled)
        message(FATAL_ERROR "ACS: no modules enabled — edit modules.cmake")
    endif()

    # Verify dependency closure is enabled.
    foreach(mod ${enabled})
        get_property(pd GLOBAL PROPERTY _ACS_MOD_${mod}_PUBLIC_DEPS)
        get_property(rd GLOBAL PROPERTY _ACS_MOD_${mod}_PRIVATE_DEPS)
        foreach(dep ${pd} ${rd})
            list(FIND enabled "${dep}" found)
            if(found EQUAL -1)
                message(FATAL_ERROR
                    "ACS: module '${mod}' depends on '${dep}' but '${dep}' is not enabled.")
            endif()
        endforeach()
    endforeach()

    foreach(mod ${enabled})
        get_property(srcdir GLOBAL PROPERTY _ACS_MOD_${mod}_SOURCE_DIR)
        get_property(srcs   GLOBAL PROPERTY _ACS_MOD_${mod}_SOURCES)
        get_property(hdrs   GLOBAL PROPERTY _ACS_MOD_${mod}_HEADERS)
        get_property(pd     GLOBAL PROPERTY _ACS_MOD_${mod}_PUBLIC_DEPS)
        get_property(rd     GLOBAL PROPERTY _ACS_MOD_${mod}_PRIVATE_DEPS)
        get_property(lp     GLOBAL PROPERTY _ACS_MOD_${mod}_LINK_PUBLIC)
        get_property(lr     GLOBAL PROPERTY _ACS_MOD_${mod}_LINK_PRIVATE)
        get_property(feats  GLOBAL PROPERTY _ACS_MOD_${mod}_FEATURES)

        string(TOLOWER "acs_${mod}" tgt)
        set(src_paths "")
        foreach(s ${srcs})
            list(APPEND src_paths "${srcdir}/${s}")
        endforeach()
        set(hdr_paths "")
        foreach(h ${hdrs})
            list(APPEND hdr_paths "${srcdir}/${h}")
        endforeach()

        add_library(${tgt} STATIC ${src_paths} ${hdr_paths})
        add_library(ACS::${mod} ALIAS ${tgt})

        # Solution Explorer (Visual Studio) でエンジンモジュールを 1 フォルダに集約。
        set_target_properties(${tgt} PROPERTIES FOLDER "engine")

        target_include_directories(${tgt} PUBLIC ${ACS_SOURCE_ROOT})
        acs_apply_compiler_options(${tgt})

        # Module-presence define.
        string(TOUPPER "${mod}" MOD_UPPER)
        target_compile_definitions(${tgt} PUBLIC "WITH_ACS_${MOD_UPPER}=1")

        # Feature defines (1 if enabled, 0 if disabled — header guards see both states).
        foreach(feat ${feats})
            string(REPLACE "|" ";" parts "${feat}")
            list(GET parts 1 fdef)
            list(GET parts 2 fopt)
            if(${fopt})
                target_compile_definitions(${tgt} PUBLIC "${fdef}=1")
            else()
                target_compile_definitions(${tgt} PUBLIC "${fdef}=0")
            endif()
        endforeach()

        foreach(d ${pd})
            target_link_libraries(${tgt} PUBLIC "ACS::${d}")
        endforeach()
        foreach(d ${rd})
            target_link_libraries(${tgt} PRIVATE "ACS::${d}")
        endforeach()
        foreach(l ${lp})
            target_link_libraries(${tgt} PUBLIC ${l})
        endforeach()
        foreach(l ${lr})
            target_link_libraries(${tgt} PRIVATE ${l})
        endforeach()
    endforeach()

    message(STATUS "ACS: enabled modules — ${enabled}")
    foreach(mod ${enabled})
        get_property(feats GLOBAL PROPERTY _ACS_MOD_${mod}_FEATURES)
        foreach(feat ${feats})
            string(REPLACE "|" ";" parts "${feat}")
            list(GET parts 0 fname)
            list(GET parts 1 fdef)
            list(GET parts 2 fopt)
            if(${fopt})
                message(STATUS "  ${mod}::${fname} = ON  (-D${fdef}=1)")
            else()
                message(STATUS "  ${mod}::${fname} = OFF (-D${fdef}=0)")
            endif()
        endforeach()
    endforeach()
endfunction()
