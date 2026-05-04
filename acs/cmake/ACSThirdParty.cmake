# ACS — サードパーティライブラリの取得 (FetchContent)
#
# 各 acs_third_party::<name> ターゲットは include パスのみを公開する
# INTERFACE ライブラリ。シングルヘッダ系のため、実装は ACS 側の .cpp
# 内で `#define <LIB>_IMPLEMENTATION` してインクルードする。
include_guard(GLOBAL)
include(FetchContent)

# ---- stb (image / vorbis) -------------------------------------------------
function(acs_third_party_stb)
    if(TARGET acs_third_party::stb)
        return()
    endif()
    FetchContent_Declare(
        acs_stb
        GIT_REPOSITORY https://github.com/nothings/stb.git
        GIT_TAG        master
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(acs_stb)
    add_library(acs_third_party_stb INTERFACE)
    target_include_directories(acs_third_party_stb INTERFACE "${acs_stb_SOURCE_DIR}")
    add_library(acs_third_party::stb ALIAS acs_third_party_stb)
endfunction()

# ---- cgltf ----------------------------------------------------------------
function(acs_third_party_cgltf)
    if(TARGET acs_third_party::cgltf)
        return()
    endif()
    FetchContent_Declare(
        acs_cgltf
        GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
        GIT_TAG        v1.14
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(acs_cgltf)
    add_library(acs_third_party_cgltf INTERFACE)
    target_include_directories(acs_third_party_cgltf INTERFACE "${acs_cgltf_SOURCE_DIR}")
    add_library(acs_third_party::cgltf ALIAS acs_third_party_cgltf)
endfunction()

# ---- ufbx -----------------------------------------------------------------
function(acs_third_party_ufbx)
    if(TARGET acs_third_party::ufbx)
        return()
    endif()
    FetchContent_Declare(
        acs_ufbx
        GIT_REPOSITORY https://github.com/ufbx/ufbx.git
        GIT_TAG        v0.16.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(acs_ufbx)
    # ufbx は ufbx.c + ufbx.h の 2 ファイル構成。.c をコンパイルして静的 lib にする。
    add_library(acs_third_party_ufbx STATIC "${acs_ufbx_SOURCE_DIR}/ufbx.c")
    target_include_directories(acs_third_party_ufbx PUBLIC "${acs_ufbx_SOURCE_DIR}")
    if(MSVC)
        # サードパーティ警告を抑制
        target_compile_options(acs_third_party_ufbx PRIVATE /W3 /wd4267 /wd4244 /wd4459 /wd4456 /wd4701 /wd4702)
        target_compile_definitions(acs_third_party_ufbx PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    add_library(acs_third_party::ufbx ALIAS acs_third_party_ufbx)
endfunction()

# ---- dr_libs (wav / mp3 / flac) -------------------------------------------
function(acs_third_party_drlibs)
    if(TARGET acs_third_party::drlibs)
        return()
    endif()
    FetchContent_Declare(
        acs_drlibs
        GIT_REPOSITORY https://github.com/mackron/dr_libs.git
        GIT_TAG        master
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(acs_drlibs)
    add_library(acs_third_party_drlibs INTERFACE)
    target_include_directories(acs_third_party_drlibs INTERFACE "${acs_drlibs_SOURCE_DIR}")
    add_library(acs_third_party::drlibs ALIAS acs_third_party_drlibs)
endfunction()

# ImGui は src/imgui/Module.cmake が独自に FetchContent するため、
# ここでは扱わない（重複ダウンロードを避ける）。

# ---- miniaudio (single-header audio library, BSD-licensed) ----------------
# Win = WASAPI / DSound、Linux = ALSA / PulseAudio、macOS = CoreAudio を内部選択。
# acs_third_party::miniaudio は include パスのみ提供する INTERFACE。
# 実装は AudioEngine.cpp 内で MA_IMPLEMENTATION を define して inline する。
function(acs_third_party_miniaudio)
    if(TARGET acs_third_party::miniaudio)
        return()
    endif()
    FetchContent_Declare(
        acs_miniaudio
        GIT_REPOSITORY https://github.com/mackron/miniaudio.git
        GIT_TAG        master
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(acs_miniaudio)
    add_library(acs_third_party_miniaudio INTERFACE)
    target_include_directories(acs_third_party_miniaudio INTERFACE "${acs_miniaudio_SOURCE_DIR}")
    add_library(acs_third_party::miniaudio ALIAS acs_third_party_miniaudio)
endfunction()

# ---- Diligent Engine (DiligentCore + DiligentTools + DiligentFX) ----------
# acs_third_party::diligent_core / ::diligent_tools / ::diligent_fx を提供する。
# 現在は DX12 のみ有効化。Vulkan / Metal は後続フェーズで段階的に有効化する。
function(acs_third_party_diligent)
    if(TARGET acs_third_party::diligent_core)
        return()
    endif()

    # Diligent 内のデフォルトを ACS 向けに上書き（FetchContent 前に設定する必要あり）
    # ACS_DILIGENT_VULKAN=ON のときだけ Vulkan バックエンドも一緒にビルドする。
    if(WIN32)
        set(DILIGENT_NO_OPENGL    ON  CACHE BOOL "" FORCE)
        if(ACS_DILIGENT_VULKAN)
            set(DILIGENT_NO_VULKAN OFF CACHE BOOL "" FORCE)
        else()
            set(DILIGENT_NO_VULKAN ON  CACHE BOOL "" FORCE)
        endif()
        set(DILIGENT_NO_DIRECT3D11 ON CACHE BOOL "" FORCE)
        set(DILIGENT_NO_METAL     ON  CACHE BOOL "" FORCE)
        set(DILIGENT_NO_WEBGPU    ON  CACHE BOOL "" FORCE)
    endif()
    set(DILIGENT_BUILD_TOOLS_INCLUDE_DIRS    ON  CACHE BOOL "" FORCE)
    set(DILIGENT_BUILD_TOOLS_TESTS           OFF CACHE BOOL "" FORCE)
    set(DILIGENT_BUILD_FX_TESTS              OFF CACHE BOOL "" FORCE)
    set(DILIGENT_BUILD_CORE_TESTS            OFF CACHE BOOL "" FORCE)
    set(DILIGENT_BUILD_SAMPLES               OFF CACHE BOOL "" FORCE)
    set(DILIGENT_BUILD_DEMOS                 OFF CACHE BOOL "" FORCE)
    set(DILIGENT_BUILD_UNITY_PLUGIN          OFF CACHE BOOL "" FORCE)
    set(DILIGENT_INSTALL_CORE                OFF CACHE BOOL "" FORCE)
    set(DILIGENT_INSTALL_TOOLS               OFF CACHE BOOL "" FORCE)
    set(DILIGENT_INSTALL_FX                  OFF CACHE BOOL "" FORCE)
    set(DILIGENT_INSTALL_SAMPLES             OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS                    OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        acs_diligent_core
        GIT_REPOSITORY https://github.com/DiligentGraphics/DiligentCore.git
        GIT_TAG        v2.5.6
        GIT_SHALLOW    TRUE
    )
    FetchContent_Declare(
        acs_diligent_tools
        GIT_REPOSITORY https://github.com/DiligentGraphics/DiligentTools.git
        GIT_TAG        v2.5.6
        GIT_SHALLOW    TRUE
    )
    FetchContent_Declare(
        acs_diligent_fx
        GIT_REPOSITORY https://github.com/DiligentGraphics/DiligentFX.git
        GIT_TAG        v2.5.6
        GIT_SHALLOW    TRUE
    )
    # Core を先に MakeAvailable（Tools / FX が依存するため）
    FetchContent_MakeAvailable(acs_diligent_core)
    FetchContent_MakeAvailable(acs_diligent_tools)
    FetchContent_MakeAvailable(acs_diligent_fx)

    # ACS 側に公開する集約 INTERFACE ターゲット
    add_library(acs_third_party_diligent_core INTERFACE)
    target_link_libraries(acs_third_party_diligent_core INTERFACE
        Diligent-GraphicsEngine
        Diligent-GraphicsEngineD3D12-static
        Diligent-Common
        Diligent-GraphicsAccessories
        Diligent-GraphicsTools
        Diligent-ShaderTools
    )
    if(ACS_DILIGENT_VULKAN)
        target_link_libraries(acs_third_party_diligent_core INTERFACE
            Diligent-GraphicsEngineVk-static
        )
    endif()
    target_include_directories(acs_third_party_diligent_core INTERFACE
        "${acs_diligent_core_SOURCE_DIR}"
        "${acs_diligent_core_SOURCE_DIR}/Graphics/GraphicsEngine/interface"
        "${acs_diligent_core_SOURCE_DIR}/Graphics/GraphicsEngineD3D12/interface"
        "${acs_diligent_core_SOURCE_DIR}/Common/interface"
        "${acs_diligent_core_SOURCE_DIR}/Graphics/GraphicsAccessories/interface"
        "${acs_diligent_core_SOURCE_DIR}/Graphics/GraphicsTools/interface"
        "${acs_diligent_core_SOURCE_DIR}/Graphics/ShaderTools/interface"
        "${acs_diligent_core_SOURCE_DIR}/Platforms/Basic/interface"
        "${acs_diligent_core_SOURCE_DIR}/Platforms/Win32/interface"
        "${acs_diligent_core_SOURCE_DIR}/Platforms/interface"
        "${acs_diligent_core_SOURCE_DIR}/Primitives/interface"
    )
    add_library(acs_third_party::diligent_core ALIAS acs_third_party_diligent_core)

    add_library(acs_third_party_diligent_tools INTERFACE)
    target_link_libraries(acs_third_party_diligent_tools INTERFACE
        Diligent-TextureLoader
        Diligent-RenderStateNotation
        Diligent-AssetLoader
        acs_third_party_diligent_core
    )
    target_include_directories(acs_third_party_diligent_tools INTERFACE
        "${acs_diligent_tools_SOURCE_DIR}/TextureLoader/interface"
        "${acs_diligent_tools_SOURCE_DIR}/AssetLoader/interface"
        "${acs_diligent_tools_SOURCE_DIR}/RenderStateNotation/interface"
    )
    add_library(acs_third_party::diligent_tools ALIAS acs_third_party_diligent_tools)

    add_library(acs_third_party_diligent_fx INTERFACE)
    target_link_libraries(acs_third_party_diligent_fx INTERFACE
        DiligentFX
        acs_third_party_diligent_tools
    )
    target_include_directories(acs_third_party_diligent_fx INTERFACE
        "${acs_diligent_fx_SOURCE_DIR}/PostProcess/interface"
        "${acs_diligent_fx_SOURCE_DIR}/Components/interface"
        "${acs_diligent_fx_SOURCE_DIR}/PBR/interface"
        "${acs_diligent_fx_SOURCE_DIR}/Hydrogent/interface"
    )
    add_library(acs_third_party::diligent_fx ALIAS acs_third_party_diligent_fx)
endfunction()
