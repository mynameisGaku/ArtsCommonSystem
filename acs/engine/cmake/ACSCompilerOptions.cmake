# Centralized compiler option helper for ACS targets.
function(_acs_classify_source_path target_root tree_root source_path result)
    cmake_path(SET _acs_class_target_root NORMALIZE "${target_root}")
    cmake_path(SET _acs_class_tree_root NORMALIZE "${tree_root}")
    cmake_path(SET _acs_class_source_path NORMALIZE "${source_path}")
    cmake_path(IS_PREFIX _acs_class_target_root
               "${_acs_class_source_path}" _acs_class_inside)
    cmake_path(IS_PREFIX _acs_class_tree_root
               "${_acs_class_source_path}" _acs_class_engine)
    if(_acs_class_inside)
        set(_acs_class Target)
    elseif(_acs_class_engine)
        set(_acs_class Engine)
    else()
        set(_acs_class External)
    endif()
    set(${result} "${_acs_class}" PARENT_SCOPE)
endfunction()

function(acs_apply_compiler_options tgt)
    # Release の ACS ターゲットだけへ IPO/LTCG を適用する。Debug と ASan は従来どおり
    # 高速な反復ビルドと診断可能性を保つ。
    if(ACS_RELEASE_IPO_ACTIVE)
        set_property(TARGET ${tgt}
            PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    endif()

    if(MSVC)
        # Diligent の static library は内部で例外と MSVC STL の例外型を使う。
        # 同じ最終 binary の ACS target だけ _HAS_EXCEPTIONS=0 にすると STL ABI が
        # 分裂して LTCG C4743 になるため、Diligent 構成では compiler ABI を揃える。
        # ACS source の throw/try/catch 禁止と TResult による失敗伝搬は維持する。
        if(ACS_RENDER_DILIGENT)
            set(_acs_exception_option /EHsc)
            set(_acs_exception_definition _HAS_EXCEPTIONS=1)
        else()
            set(_acs_exception_option /EHs-c-)
            set(_acs_exception_definition _HAS_EXCEPTIONS=0)
        endif()
        target_compile_options(${tgt} PRIVATE
            /W4
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
            /Zc:inline
            /utf-8
            ${_acs_exception_option}
            /GR-             # RTTI disabled
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
            ${_acs_exception_definition}
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
    # 固定レベルのログマクロを if constexpr で除去する。既定の 0 は従来互換で、
    # 配布ビルドでは 2 (Info) などへ上げて Trace/Debug の引数評価も消せる。
    target_compile_definitions(${tgt} PUBLIC
        ACS_COMPILED_LOG_MIN_SEVERITY=${ACS_COMPILED_LOG_MIN_SEVERITY})

    # VS Solution Explorer のフィルタを実ディスクのフォルダ構成と一致させる。
    # ターゲット定義 dir を基準に source_group(TREE) を張り、src/<mod>/… や
    # tool の実ディスク構成をそのまま反映する。各 CMakeLists 側で追加の
    # "Source Files"/"Header Files" grouping を書く必要はない。
    #
    # 外部プロジェクトは別 drive の ACS source も取り込むため、path prefix で
    # target 内・ACS 内・外部共有 source の三種類へ分類する。
    get_target_property(_acs_tgt_srcdir ${tgt} SOURCE_DIR)
    get_target_property(_acs_tgt_srcs   ${tgt} SOURCES)
    if(_acs_tgt_srcs AND _acs_tgt_srcdir)
        set(_acs_inside "")
        set(_acs_engine "")
        set(_acs_external "")
        foreach(_s ${_acs_tgt_srcs})
            if(IS_ABSOLUTE "${_s}")
                set(_abs "${_s}")
            else()
                set(_abs "${_acs_tgt_srcdir}/${_s}")
            endif()
            cmake_path(SET _abs NORMALIZE "${_abs}")
            _acs_classify_source_path("${_acs_tgt_srcdir}" "${ACS_TREE_ROOT}"
                                      "${_abs}" _acs_source_class)
            if(_acs_source_class STREQUAL Target)
                list(APPEND _acs_inside "${_abs}")
            elseif(_acs_source_class STREQUAL Engine)
                list(APPEND _acs_engine "${_abs}")
            else()
                list(APPEND _acs_external "${_abs}")
            endif()
        endforeach()
        if(_acs_inside)
            source_group(TREE "${_acs_tgt_srcdir}" FILES ${_acs_inside})
        endif()
        if(_acs_engine)
            # Files referenced from outside the target's own dir (e.g. editor
            # targets pulling in src/gameframework/tools/...). Anchor the VS
            # filter tree at the acs/ source root, NOT CMAKE_SOURCE_DIR — the
            # latter is acs/engine/ (where the build system lives) and those
            # files live one level up under acs/src, which source_group(TREE)
            # would reject as "not under the tree".
            source_group(TREE "${ACS_TREE_ROOT}" FILES ${_acs_engine})
        endif()
        if(_acs_external)
            # ACS 外の共有ソースは任意のドライブでも失敗しない固定 group にまとめる。
            source_group("External Sources" FILES ${_acs_external})
        endif()
    endif()
endfunction()
