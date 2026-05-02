# Centralized compiler option helper for ACS targets.
function(acs_apply_compiler_options tgt)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE
            /W4 /WX
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
            /Zc:inline
            /utf-8
            /EHs-c-          # exceptions disabled (Result<T,E> only)
            /GR-             # RTTI disabled
            /D_HAS_EXCEPTIONS=0
        )
        target_compile_definitions(${tgt} PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            UNICODE _UNICODE
        )
    else()
        target_compile_options(${tgt} PRIVATE
            -Wall -Wextra -Wpedantic -Werror
            -fno-exceptions -fno-rtti
        )
    endif()

    if(ACS_ENABLE_ASSERTS)
        target_compile_definitions(${tgt} PUBLIC ACS_ASSERTS_ENABLED=1)
    endif()
endfunction()
