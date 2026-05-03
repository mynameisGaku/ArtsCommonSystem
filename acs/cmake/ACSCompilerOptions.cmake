# Centralized compiler option helper for ACS targets.
function(acs_apply_compiler_options tgt)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE
            /W4
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
            /Zc:inline
            /utf-8
            /EHs-c-          # exceptions disabled (Result<T,E> only)
            /GR-             # RTTI disabled
            /D_HAS_EXCEPTIONS=0
            # 警告抑制（無害／設計意図によるもの・サードパーティ起因）
            /wd4324          # C4324: struct padded due to alignas
            /wd4201          # C4201: nonstandard extension nameless union/struct
            /wd4456          # C4456: declaration hides previous local declaration (stb_vorbis)
            /wd4457          # C4457: declaration hides function parameter
            /wd4458          # C4458: declaration hides class member
            /wd4459          # C4459: declaration hides global declaration
            /wd4505          # C4505: unreferenced local function (cgltf inline statics)
            /wd4533          # C4533: goto skips initialization (stb_vorbis)
            /wd4267          # C4267: size_t -> u32 narrowing (cgltf, stb_vorbis)
            /wd4244          # C4244: type narrowing (third-party)
            /wd4189          # C4189: local var initialized but unreferenced (cgltf)
            /wd4101          # C4101: unreferenced local variable (cgltf)
            /wd4702          # C4702: unreachable code (third-party)
            /wd4310          # C4310: cast truncates constant value (third-party)
            /wd4245          # C4245: signed/unsigned mismatch (cgltf)
            /wd4127          # C4127: conditional expression is constant (third-party)
        )
        target_compile_definitions(${tgt} PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            UNICODE _UNICODE
            _CRT_SECURE_NO_WARNINGS    # cgltf 等の fopen/strcpy で C4996 が出ないように
        )
    else()
        target_compile_options(${tgt} PRIVATE
            -Wall -Wextra -Wpedantic
            -fno-exceptions -fno-rtti
        )
    endif()

    if(ACS_ENABLE_ASSERTS)
        target_compile_definitions(${tgt} PUBLIC ACS_ASSERTS_ENABLED=1)
    endif()
endfunction()
