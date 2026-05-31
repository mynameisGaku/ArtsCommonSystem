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
            /EHs-c-          # exceptions disabled (TResult<T,E> only)
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
    else()
        target_compile_definitions(${tgt} PUBLIC ACS_ASSERTS_ENABLED=0)
    endif()

    # VS Solution Explorer のフィルタを実ディスクのフォルダ構成と一致させる。
    # ターゲット定義 dir を基準に source_group(TREE) を張り、engine は src/<mod>/…、
    # 各サンプルは自身の dir 構成をそのまま反映する。各サンプル CMakeLists 側で
    # "Source Files"/"Header Files" の regex grouping を書く必要はない (撤去済み)。
    #
    # 一部のエディタ系サンプルは自分の dir 外 (src/gameframework/tools/*/…) の
    # ソースを直接 add_executable しているため、自 dir 内/外でルートを分ける:
    #   ・自 dir 内 → SOURCE_DIR 基準 (例: main.cpp が直下に出る)
    #   ・自 dir 外 → プロジェクト root 基準 (実際のパス階層 src/… に出る)
    get_target_property(_acs_tgt_srcdir ${tgt} SOURCE_DIR)
    get_target_property(_acs_tgt_srcs   ${tgt} SOURCES)
    if(_acs_tgt_srcs AND _acs_tgt_srcdir)
        set(_acs_inside "")
        set(_acs_outside "")
        foreach(_s ${_acs_tgt_srcs})
            if(IS_ABSOLUTE "${_s}")
                set(_abs "${_s}")
            else()
                set(_abs "${_acs_tgt_srcdir}/${_s}")
            endif()
            file(RELATIVE_PATH _rel "${_acs_tgt_srcdir}" "${_abs}")
            if(_rel MATCHES "^\\.\\.")
                list(APPEND _acs_outside "${_abs}")
            else()
                list(APPEND _acs_inside "${_abs}")
            endif()
        endforeach()
        if(_acs_inside)
            source_group(TREE "${_acs_tgt_srcdir}" FILES ${_acs_inside})
        endif()
        if(_acs_outside)
            # Files referenced from outside the target's own dir (e.g. editor
            # samples pulling in src/gameframework/tools/...). Anchor the VS
            # filter tree at the acs/ source root, NOT CMAKE_SOURCE_DIR — the
            # latter is acs/engine/ (where the build system lives) and those
            # files live one level up under acs/src, which source_group(TREE)
            # would reject as "not under the tree".
            source_group(TREE "${ACS_TREE_ROOT}" FILES ${_acs_outside})
        endif()
    endif()
endfunction()
