# ACS — サードパーティライブラリの取得 (FetchContent)
#
# 各 acs_third_party::<name> ターゲットは include パスのみを公開する
# INTERFACE ライブラリ。シングルヘッダ系のため、実装は ACS 側の .cpp
# 内で `#define <LIB>_IMPLEMENTATION` してインクルードする。
include_guard(GLOBAL)
include(FetchContent)

# 配布対象のlicenseを固有名で登録し、取得物の欠落をconfigure時に検出する。
function(_acs_install_runtime_license source_file output_name)
    if(NOT EXISTS "${source_file}")
        message(FATAL_ERROR "ACS: third-party license not found: ${source_file}")
    endif()
    install(FILES "${source_file}"
            DESTINATION "Licenses/ThirdParty"
            RENAME "${output_name}"
            COMPONENT ACSGameRuntime)
endfunction()

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
    _acs_install_runtime_license(
        "${acs_stb_SOURCE_DIR}/LICENSE" "stb-License.txt")
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
    _acs_install_runtime_license(
        "${acs_cgltf_SOURCE_DIR}/LICENSE" "cgltf-License.txt")
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
    _acs_install_runtime_license(
        "${acs_ufbx_SOURCE_DIR}/LICENSE" "ufbx-License.txt")
    # ufbx は ufbx.c + ufbx.h の 2 ファイル構成。.c をコンパイルして静的 lib にする。
    add_library(acs_third_party_ufbx STATIC "${acs_ufbx_SOURCE_DIR}/ufbx.c")
    target_include_directories(acs_third_party_ufbx PUBLIC "${acs_ufbx_SOURCE_DIR}")
    if(MSVC)
        # サードパーティ警告を抑制
        target_compile_options(acs_third_party_ufbx PRIVATE /W3 /wd4267 /wd4244 /wd4459 /wd4456 /wd4701 /wd4702)
        target_compile_definitions(acs_third_party_ufbx PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    add_library(acs_third_party::ufbx ALIAS acs_third_party_ufbx)
    set_target_properties(acs_third_party_ufbx PROPERTIES FOLDER "Engine/ThirdParty")
endfunction()

# ---- mimalloc -------------------------------------------------------------
# MemorySystem の仮想メモリヒープとして現行 v3 を静的リンクする。
# malloc/new のグローバル上書きは行わず、ACS の FAllocator 境界から明示利用する。
function(acs_third_party_mimalloc)
    if(TARGET acs_third_party::mimalloc)
        return()
    endif()

    set(MI_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_STATIC   ON  CACHE BOOL "" FORCE)
    set(MI_BUILD_OBJECT   OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_CXX      OFF CACHE BOOL "" FORCE)
    set(MI_OVERRIDE       OFF CACHE BOOL "" FORCE)
    set(MI_INSTALL_TOPLEVEL OFF CACHE BOOL "" FORCE)
    option(ACS_MIMALLOC_RE_ENGINE_PAGE_PROFILE
           "Use the measured 16 KiB small / 128 KiB medium page profile" ON)
    if(ACS_ENABLE_ADDRESS_SANITIZER)
        set(MI_TRACK_ASAN ON CACHE BOOL "" FORCE)
    else()
        set(MI_TRACK_ASAN OFF CACHE BOOL "" FORCE)
    endif()

    FetchContent_Declare(
        acs_mimalloc
        GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
        # v3.3.2 の実体へ固定し、タグの付け替えで依存物が変化しないようにする。
        GIT_TAG        30b2d9d89099bee08e9f67a1ffb3e12e7ba45227
        # CMake は commit hash と shallow clone の併用を保証しないため全履歴から取得する。
        GIT_SHALLOW    FALSE
    )
    FetchContent_MakeAvailable(acs_mimalloc)
    # mimalloc はリンク依存として構築し、上流のSDK install規則は公開しない。
    set_property(DIRECTORY "${acs_mimalloc_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
    _acs_install_runtime_license(
        "${acs_mimalloc_SOURCE_DIR}/LICENSE" "mimalloc-License.txt")

    if(NOT TARGET mimalloc-static)
        message(FATAL_ERROR "ACS: mimalloc-static target was not created")
    endif()

    if(CMAKE_CONFIGURATION_TYPES)
        # mimalloc v3.3.2 は multi-config generator でも configure 時の
        # CMAKE_BUILD_TYPE だけを見て MI_DEBUG / MI_CMAKE_BUILD_TYPE を全構成へ
        # 固定する。上流が追加した 2 定義だけを除去し、実際の構成へ追従させる。
        get_target_property(_acs_mimalloc_compile_definitions
                            mimalloc-static COMPILE_DEFINITIONS)
        if(_acs_mimalloc_compile_definitions)
            list(FILTER _acs_mimalloc_compile_definitions
                 EXCLUDE REGEX "^MI_DEBUG=")
            list(FILTER _acs_mimalloc_compile_definitions
                 EXCLUDE REGEX "^MI_CMAKE_BUILD_TYPE=")
            set_property(TARGET mimalloc-static PROPERTY COMPILE_DEFINITIONS
                         "${_acs_mimalloc_compile_definitions}")
        endif()
        target_compile_definitions(mimalloc-static PRIVATE
            "MI_DEBUG=$<IF:$<CONFIG:Debug>,2,0>"
            "MI_CMAKE_BUILD_TYPE=$<LOWER_CASE:$<CONFIG>>")
    endif()

    set_target_properties(mimalloc-static PROPERTIES FOLDER "Engine/ThirdParty")
    if(ACS_MIMALLOC_RE_ENGINE_PAGE_PROFILE)
        # mimalloc v3 では旧 segment 構造が arena slice へ置き換わった。
        # slice=16KiB にすると small=16KiB / medium=128KiB となり、RE ENGINE の
        # 実測プロファイルと同じ粒度になる。問題切り分けや再計測時は OFF に戻せる。
        target_compile_definitions(mimalloc-static PRIVATE MI_ARENA_SLICE_SHIFT=14)
    endif()
    if(MSVC)
        target_compile_options(mimalloc-static PRIVATE /W3)
    endif()

    add_library(acs_third_party_mimalloc INTERFACE)
    target_link_libraries(acs_third_party_mimalloc INTERFACE mimalloc-static)
    target_compile_definitions(acs_third_party_mimalloc INTERFACE
        ACS_MIMALLOC_RE_ENGINE_PAGE_PROFILE=$<BOOL:${ACS_MIMALLOC_RE_ENGINE_PAGE_PROFILE}>)
    add_library(acs_third_party::mimalloc ALIAS acs_third_party_mimalloc)
endfunction()

# ---- Steamworks SDK (Phase 26+ 実バックエンド) ----------------------------
# Valve の SDK は partner.steamgames.com (要パートナー登録) からのみ入手可能で
# 公開ミラー URL は存在しない。よって以下のいずれかで SDK を確保する:
#
#   方式 A (推奨): user が SDK を local download し、CMake var を渡す
#       cmake -DACS_STEAMWORKS_SDK_DIR=C:/path/to/steamworks_sdk_162
#
#   方式 B: ZIP URL を渡して FetchContent で取得 (CI 用、user の責任で URL 用意)
#       cmake -DACS_STEAMWORKS_SDK_URL=file:///C:/sdk.zip
#       (例: 自前 S3 / GitHub Release の private mirror。要 Valve への確認)
#
# 方式 A / B いずれかが ACS_BUILD_STEAMWORKS=ON 時に必須。両方未指定なら
# WARNING + 自動 OFF。
#
# 期待される SDK レイアウト (Steamworks SDK 1.62 確認):
#   <SDK>/public/steam/steam_api.h            ← C++ ヘッダ群
#   <SDK>/public/steam/*.h                     ← isteamuser, isteamuserstats, etc.
#   <SDK>/redistributable_bin/win64/steam_api64.lib  ← import lib
#   <SDK>/redistributable_bin/win64/steam_api64.dll  ← runtime DLL
# SDK root (= public/ と redistributable_bin/ を含む dir) を search_dir 以下から探す。
# ミラーによっては SDK が `sdk/` サブディレクトリに入っているので再帰探索する。
function(_acs_find_steamworks_root search_dir out_var)
    set(${out_var} "" PARENT_SCOPE)
    file(GLOB_RECURSE _hits "${search_dir}/*steam_api.h")
    foreach(_h ${_hits})
        get_filename_component(_steamdir  "${_h}" DIRECTORY)        # .../public/steam
        get_filename_component(_publicdir "${_steamdir}" DIRECTORY) # .../public
        get_filename_component(_root      "${_publicdir}" DIRECTORY) # .../ (SDK root)
        if(EXISTS "${_root}/redistributable_bin/win64/steam_api64.lib")
            set(${out_var} "${_root}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

function(acs_third_party_steamworks)
    if(TARGET acs_third_party::steamworks)
        return()
    endif()

    set(_sdk_dir "")
    if(DEFINED ACS_STEAMWORKS_SDK_DIR AND NOT "${ACS_STEAMWORKS_SDK_DIR}" STREQUAL "")
        # 方式 A: ローカル SDK ディレクトリ
        if(EXISTS "${ACS_STEAMWORKS_SDK_DIR}/public/steam/steam_api.h")
            set(_sdk_dir "${ACS_STEAMWORKS_SDK_DIR}")
            message(STATUS "ACS: Steamworks SDK (local) = ${_sdk_dir}")
        else()
            message(FATAL_ERROR
                "ACS_STEAMWORKS_SDK_DIR=${ACS_STEAMWORKS_SDK_DIR} に "
                "public/steam/steam_api.h が見つからない。Steamworks SDK の "
                "ZIP を展開した directory を指定すること。")
        endif()
    elseif(DEFINED ACS_STEAMWORKS_SDK_URL AND NOT "${ACS_STEAMWORKS_SDK_URL}" STREQUAL "")
        # 方式 B: ZIP URL から FetchContent
        FetchContent_Declare(
            acs_steamworks
            URL "${ACS_STEAMWORKS_SDK_URL}"
        )
        FetchContent_MakeAvailable(acs_steamworks)
        _acs_find_steamworks_root("${acs_steamworks_SOURCE_DIR}" _sdk_dir)
        if(_sdk_dir)
            message(STATUS "ACS: Steamworks SDK (URL fetched) = ${_sdk_dir}")
        else()
            message(FATAL_ERROR
                "ACS_STEAMWORKS_SDK_URL から取得した内容に "
                "public/steam/steam_api.h が見つからない。ZIP の中身を確認。")
        endif()
    else()
        # 方式 C (default): 公開 git ミラーから取得。
        # Valve 公式 SDK は partner gating されるが、CI 用に SDK を再配布する
        # 公開 repo がある。ACS_STEAMWORKS_SDK_GIT / _TAG で差し替え可能。
        if(NOT DEFINED ACS_STEAMWORKS_SDK_GIT OR "${ACS_STEAMWORKS_SDK_GIT}" STREQUAL "")
            set(ACS_STEAMWORKS_SDK_GIT "https://github.com/julianxhokaxhiu/SteamworksSDKCI.git")
        endif()
        if(NOT DEFINED ACS_STEAMWORKS_SDK_GIT_TAG OR "${ACS_STEAMWORKS_SDK_GIT_TAG}" STREQUAL "")
            set(ACS_STEAMWORKS_SDK_GIT_TAG "1.62")  # Steamworks SDK 1.62 (julianxhokaxhiu mirror tag)
        endif()
        message(STATUS "ACS: Steamworks SDK を git ミラーから取得: "
                       "${ACS_STEAMWORKS_SDK_GIT} @ ${ACS_STEAMWORKS_SDK_GIT_TAG}")
        FetchContent_Declare(
            acs_steamworks
            GIT_REPOSITORY "${ACS_STEAMWORKS_SDK_GIT}"
            GIT_TAG        "${ACS_STEAMWORKS_SDK_GIT_TAG}"
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(acs_steamworks)
        _acs_find_steamworks_root("${acs_steamworks_SOURCE_DIR}" _sdk_dir)
        if(NOT _sdk_dir)
            message(FATAL_ERROR
                "git ミラー (${ACS_STEAMWORKS_SDK_GIT}) から取得した内容に "
                "public/steam/steam_api.h が見つからない。レイアウトが想定と"
                "違う場合は -DACS_STEAMWORKS_SDK_DIR=<local SDK> で直接指定するか、"
                "別ミラーを -DACS_STEAMWORKS_SDK_GIT=... で指定すること。")
        endif()
        message(STATUS "ACS: Steamworks SDK (git mirror) = ${_sdk_dir}")
    endif()

    add_library(acs_third_party_steamworks INTERFACE)
    target_include_directories(acs_third_party_steamworks INTERFACE
        "${_sdk_dir}/public")
    # Win64 用 import lib (lib + DLL の両方が必要、runtime DLL は別途 install)
    target_link_libraries(acs_third_party_steamworks INTERFACE
        "${_sdk_dir}/redistributable_bin/win64/steam_api64.lib")
    # 配布時用に DLL のパスを GLOBAL property で保存 (acs_steamworks_runtime で使う)
    set_property(GLOBAL PROPERTY ACS_STEAMWORKS_DLL_PATH
        "${_sdk_dir}/redistributable_bin/win64/steam_api64.dll")
    add_library(acs_third_party::steamworks ALIAS acs_third_party_steamworks)
endfunction()

# ---- Steamworks runtime 配備ヘルパ ----------------------------------------
# acs_steamworks_runtime(<target> [APPID <id>])
#   <target> の出力ディレクトリに:
#     1) steam_api64.dll をコピー (post-build)
#     2) steam_appid.txt を生成 (APPID 未指定なら 480 = Spacewar)
#   かつ install ルールにも DLL を登録 (配布 ZIP に同梱)。
#
# ACS_BUILD_STEAMWORKS=OFF のときは no-op (= stub ビルドでは何もしない)。
#
# 使い方 (利用者の CMakeLists.txt):
#   add_executable(my_game main.cpp)
#   target_link_libraries(my_game PRIVATE ACS::Game ACS::Steamworks)
#   acs_steamworks_runtime(my_game APPID 480)
function(acs_steamworks_runtime target)
    if(NOT ACS_BUILD_STEAMWORKS)
        return()
    endif()
    cmake_parse_arguments(ACS_SW "" "APPID" "" ${ARGN})
    if(NOT ACS_SW_APPID)
        set(ACS_SW_APPID "480")  # Spacewar (dev default)
    endif()

    get_property(_dll GLOBAL PROPERTY ACS_STEAMWORKS_DLL_PATH)
    if(NOT _dll OR NOT EXISTS "${_dll}")
        message(WARNING "ACS: steam_api64.dll が見つからない (${_dll})。"
                        "acs_third_party_steamworks() を先に呼ぶこと。")
        return()
    endif()

    # 1) DLL を exe 出力 dir に post-build コピー
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_dll}" "$<TARGET_FILE_DIR:${target}>/steam_api64.dll"
        COMMENT "Copying steam_api64.dll next to ${target}"
        VERBATIM)

    # 2) steam_appid.txt を出力 dir に生成 (build dir、実行時に SteamAPI_Init が読む)
    file(GENERATE
        OUTPUT "$<TARGET_FILE_DIR:${target}>/steam_appid.txt"
        CONTENT "${ACS_SW_APPID}")

    # 3) install: 配布 ZIP に DLL + steam_appid.txt を同梱
    install(FILES "${_dll}" DESTINATION ".")
    # steam_appid.txt は configure 時に build dir に作って install
    set(_appid_file "${CMAKE_BINARY_DIR}/steam_appid_${target}.txt")
    file(WRITE "${_appid_file}" "${ACS_SW_APPID}")
    install(FILES "${_appid_file}" DESTINATION "." RENAME "steam_appid.txt")

    message(STATUS "ACS: Steamworks runtime 配備を ${target} に設定 (AppID=${ACS_SW_APPID})")
endfunction()

# ---- OpenXR SDK / Loader (XR backend) -------------------------------------
function(acs_third_party_openxr)
    if(TARGET acs_third_party::openxr)
        return()
    endif()

    if(NOT DEFINED ACS_OPENXR_SDK_GIT OR "${ACS_OPENXR_SDK_GIT}" STREQUAL "")
        set(ACS_OPENXR_SDK_GIT "https://github.com/KhronosGroup/OpenXR-SDK-Source.git")
    endif()
    if(NOT DEFINED ACS_OPENXR_SDK_GIT_TAG OR "${ACS_OPENXR_SDK_GIT_TAG}" STREQUAL "")
        set(ACS_OPENXR_SDK_GIT_TAG "release-1.1.60")
    endif()

    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
    set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        acs_openxr
        GIT_REPOSITORY "${ACS_OPENXR_SDK_GIT}"
        GIT_TAG        "${ACS_OPENXR_SDK_GIT_TAG}"
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(acs_openxr)

    add_library(acs_third_party_openxr INTERFACE)
    if(TARGET OpenXR::openxr_loader)
        target_link_libraries(acs_third_party_openxr INTERFACE OpenXR::openxr_loader)
    elseif(TARGET openxr_loader)
        target_link_libraries(acs_third_party_openxr INTERFACE openxr_loader)
        set_target_properties(openxr_loader PROPERTIES FOLDER "Engine/ThirdParty/OpenXR")
    else()
        message(FATAL_ERROR "OpenXR loader target was not created by OpenXR-SDK-Source")
    endif()
    add_library(acs_third_party::openxr ALIAS acs_third_party_openxr)
endfunction()

# ---- ONNX Runtime (ML inference backend) ----------------------------------
# Official CPU prebuilt package. This is a real runtime: headers + import lib
# are linked at build time, and onnxruntime.dll is copied beside executables by
# acs_onnxruntime_runtime().
function(_acs_find_onnxruntime_root search_dir out_var)
    set(${out_var} "" PARENT_SCOPE)
    file(GLOB_RECURSE _hits "${search_dir}/*onnxruntime_c_api.h")
    foreach(_h ${_hits})
        get_filename_component(_include_dir "${_h}" DIRECTORY)
        get_filename_component(_root "${_include_dir}" DIRECTORY)
        if(EXISTS "${_root}/lib/onnxruntime.lib" AND EXISTS "${_root}/lib/onnxruntime.dll")
            set(${out_var} "${_root}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

function(acs_third_party_onnxruntime)
    if(TARGET acs_third_party::onnxruntime)
        return()
    endif()

    set(_ort_root "")
    if(DEFINED ACS_ONNXRUNTIME_DIR AND NOT "${ACS_ONNXRUNTIME_DIR}" STREQUAL "")
        _acs_find_onnxruntime_root("${ACS_ONNXRUNTIME_DIR}" _ort_root)
        if(NOT _ort_root)
            message(FATAL_ERROR
                "ACS_ONNXRUNTIME_DIR=${ACS_ONNXRUNTIME_DIR} does not contain "
                "include/onnxruntime_c_api.h and lib/onnxruntime.{lib,dll}.")
        endif()
        message(STATUS "ACS: ONNX Runtime (local) = ${_ort_root}")
    else()
        if(NOT DEFINED ACS_ONNXRUNTIME_VERSION OR "${ACS_ONNXRUNTIME_VERSION}" STREQUAL "")
            set(ACS_ONNXRUNTIME_VERSION "1.23.0")
        endif()
        if(NOT DEFINED ACS_ONNXRUNTIME_URL OR "${ACS_ONNXRUNTIME_URL}" STREQUAL "")
            set(ACS_ONNXRUNTIME_URL
                "https://github.com/microsoft/onnxruntime/releases/download/v${ACS_ONNXRUNTIME_VERSION}/onnxruntime-win-x64-${ACS_ONNXRUNTIME_VERSION}.zip")
        endif()
        FetchContent_Declare(
            acs_onnxruntime
            URL "${ACS_ONNXRUNTIME_URL}"
        )
        FetchContent_MakeAvailable(acs_onnxruntime)
        _acs_find_onnxruntime_root("${acs_onnxruntime_SOURCE_DIR}" _ort_root)
        if(NOT _ort_root)
            message(FATAL_ERROR
                "ONNX Runtime package did not contain the expected Windows x64 layout. "
                "Set ACS_ONNXRUNTIME_DIR to an extracted official package.")
        endif()
        message(STATUS "ACS: ONNX Runtime (fetched) = ${_ort_root}")
    endif()

    add_library(acs_third_party_onnxruntime INTERFACE)
    target_include_directories(acs_third_party_onnxruntime INTERFACE "${_ort_root}/include")
    target_link_libraries(acs_third_party_onnxruntime INTERFACE "${_ort_root}/lib/onnxruntime.lib")
    set_property(GLOBAL PROPERTY ACS_ONNXRUNTIME_DLL_PATH "${_ort_root}/lib/onnxruntime.dll")
    add_library(acs_third_party::onnxruntime ALIAS acs_third_party_onnxruntime)
endfunction()

function(acs_onnxruntime_runtime target)
    if(NOT ACS_BUILD_ML_ONNX)
        return()
    endif()
    get_property(_dll GLOBAL PROPERTY ACS_ONNXRUNTIME_DLL_PATH)
    if(NOT _dll OR NOT EXISTS "${_dll}")
        message(WARNING "ACS: onnxruntime.dll not found (${_dll}). Call acs_third_party_onnxruntime() first.")
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_dll}" "$<TARGET_FILE_DIR:${target}>/onnxruntime.dll"
        COMMENT "Copying onnxruntime.dll next to ${target}"
        VERBATIM)
    install(FILES "${_dll}" DESTINATION ".")
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
    _acs_install_runtime_license(
        "${acs_drlibs_SOURCE_DIR}/LICENSE" "dr_libs-License.txt")
    add_library(acs_third_party_drlibs INTERFACE)
    target_include_directories(acs_third_party_drlibs INTERFACE "${acs_drlibs_SOURCE_DIR}")
    add_library(acs_third_party::drlibs ALIAS acs_third_party_drlibs)
endfunction()

# ---- Lua 5.4 (scripting backend) ------------------------------------------
# 公式 Lua repo (MIT、gating 無し) を取得し、core .c を自前で static lib 化する。
# lua.c (standalone interpreter main) と luac.c (compiler main) は除外。
# acs_third_party::lua として include path + static lib を公開。
function(acs_third_party_lua)
    if(TARGET acs_third_party::lua)
        return()
    endif()
    FetchContent_Declare(
        acs_lua
        GIT_REPOSITORY https://github.com/lua/lua.git
        GIT_TAG        v5.4.7
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(acs_lua)

    # Lua core ソース (lua.c / luac.c / onelua.c を除く全 .c)。
    # 明示列挙して将来の repo 構成変更に強くする (Lua 5.4 の固定セット)。
    set(_lua_src
        lapi.c lauxlib.c lbaselib.c lcode.c lcorolib.c lctype.c ldblib.c
        ldebug.c ldo.c ldump.c lfunc.c lgc.c linit.c liolib.c llex.c
        lmathlib.c lmem.c loadlib.c lobject.c lopcodes.c loslib.c lparser.c
        lstate.c lstring.c lstrlib.c ltable.c ltablib.c ltm.c lundump.c
        lutf8lib.c lvm.c lzio.c
    )
    list(TRANSFORM _lua_src PREPEND "${acs_lua_SOURCE_DIR}/")

    add_library(acs_third_party_lua STATIC ${_lua_src})
    target_include_directories(acs_third_party_lua PUBLIC "${acs_lua_SOURCE_DIR}")
    if(MSVC)
        # Lua は C89/C99 想定。MSVC の C4xxx 警告を抑制。
        target_compile_options(acs_third_party_lua PRIVATE /W3 /wd4297 /wd4310 /wd4334)
        target_compile_definitions(acs_third_party_lua PRIVATE _CRT_SECURE_NO_WARNINGS)
        # C++ ではなく C としてコンパイル (Lua は C extern "C" 前提)
        set_source_files_properties(${_lua_src} PROPERTIES LANGUAGE C)
    endif()
    set_target_properties(acs_third_party_lua PROPERTIES FOLDER "Engine/ThirdParty")
    add_library(acs_third_party::lua ALIAS acs_third_party_lua)
endfunction()

# ImGui は src/imgui/Module.cmake が独自に FetchContent するため、
# ここでは扱わない（重複ダウンロードを避ける）。

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
    if(MSVC)
        # 親の ACS ターゲットは例外を無効化しているが、Diligent 自身は上流の
        # 例外設定でビルドする。function スコープ内だけで /EHsc を復元する。
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc")
    endif()
    # 注意 (Phase 19a-fix-3): Diligent-Tools / DiligentFX は ACS が一切使って
    # おらず (`src/render/Module.cmake` は `acs_third_party::diligent_core`
    # しかリンクしない)、`MakeAvailable` で取得すると Diligent 内部の相対
    # include path (`../../../DiligentCore/...`) が FetchContent layout
    # (`_deps/acs_diligent_*-src/`) と合わず Build Solution が壊れる。
    # 不要なので Fetch しない。将来 PBR/IBL の Diligent 標準実装が欲しく
    # なったら、ここを再有効化 + relative include path の修正を別途行う。
    FetchContent_MakeAvailable(acs_diligent_core)
    # Diligent はリンク依存として構築し、上流の install 規則を ACS 配布から隔離する。
    set_property(DIRECTORY "${acs_diligent_core_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
    _acs_install_runtime_license(
        "${acs_diligent_core_SOURCE_DIR}/License.txt"
        "DiligentCore-License.txt")
    _acs_install_runtime_license(
        "${acs_diligent_core_SOURCE_DIR}/ThirdParty/xxHash/LICENSE"
        "xxHash-License.txt")
    _acs_install_runtime_license(
        "${acs_diligent_core_SOURCE_DIR}/ThirdParty/DirectXShaderCompiler/LICENSE.TXT"
        "DXC-License.txt")
    _acs_install_runtime_license(
        "${acs_diligent_core_SOURCE_DIR}/ThirdParty/DirectXShaderCompiler/ThirdPartyNotices.txt"
        "DXC-ThirdPartyNotices.txt")
    _acs_install_runtime_license(
        "${acs_diligent_core_SOURCE_DIR}/ThirdParty/GPUOpenShaderUtils/License.txt"
        "GPUOpenShaderUtils-License.txt")

    # Diligent の大量の target を Solution Explorer "third_party/Diligent" フォルダに集約。
    # 既知の主要 target を folder 移動する (TARGET 存在確認付き)。
    set(_diligent_targets
        Diligent-BuildSettings Diligent-Common Diligent-GraphicsAccessories
        Diligent-GraphicsEngine Diligent-GraphicsEngineD3D12-static
        Diligent-GraphicsEngineD3D12-shared Diligent-GraphicsEngineNextGenBase
        Diligent-GraphicsTools Diligent-Platforms Diligent-Primitives
        Diligent-PublicBuildSettings Diligent-ShaderTools Diligent-Win32Platform
        Diligent-BasicPlatform Diligent-HLSL2GLSLConverterLib Diligent-Archiver-static
        Diligent-Archiver-shared
        SPIRV xxHash glslang-default-resource-limits GenericCodeGen MachineIndependent
        OSDependent SPIRV-Tools-static SPIRV-Tools-opt glslang HLSL OGLCompiler
        spirv-cross-core spirv-cross-glsl spirv-cross-hlsl spirv-cross-msl
        spirv-cross-cpp spirv-cross-reflect spirv-cross-util volk_headers
    )
    foreach(_t ${_diligent_targets})
        if(TARGET ${_t})
            set_target_properties(${_t} PROPERTIES FOLDER "Engine/ThirdParty/Diligent")
        endif()
    endforeach()

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

    # Phase 19a-fix-3: diligent_tools / diligent_fx INTERFACE target は ACS が
    # 誰もリンクしておらず (上記コメント参照)、Fetch を止めた今、これらを
    # 公開すると linker error になる。declaring side を完全に削除。
endfunction()
