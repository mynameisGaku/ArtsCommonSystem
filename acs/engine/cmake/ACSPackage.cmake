# ACS::Easy / ACS::Game で作ったゲームの配布パッケージング。
#
# 「ZIP 1 個を相手に渡せば動く」が目標。依存 DLL (Diligent / d3dcompiler 等) と
# アセットと exe を CPack ZIP に詰める。
#
# 使い方 (利用者の CMakeLists.txt):
#   add_executable(my_game main.cpp)
#   target_link_libraries(my_game PRIVATE ACS::Easy)
#   acs_package_game(my_game ASSETS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/assets)
#
# パッケージ生成:
#   cmake -B build -S .
#   cmake --build build --config Release
#   cmake --build build --target package    # → build/<project>-<ver>-win64.zip
#
# 生成物の内部構造:
#   my_game-0.1.0-win64.zip
#       my_game/
#           my_game.exe
#           <runtime DLLs>           # CMake 3.21+ TARGET_RUNTIME_DLLS で自動収集
#           my_game.pdb              # (RelWithDebInfo / Debug 時のみ)
#           assets/
#               <user assets>
#
# 注意:
#   ・install(TARGETS) を使うので「ZIP の中身」と「`cmake --install` する場合の
#     インストール先」が共通の DESTINATION ツリーになる。CMAKE_INSTALL_PREFIX で
#     ローカルインストール先も変更可。
#   ・ASSETS_DIR が存在しないとき (空 / 未指定) は黙ってスキップ。
#   ・win64 ZIP を生成する。CPACK_GENERATOR の上書きで他形式も選択できる。

include_guard(GLOBAL)

# Per-target install + DLL コピー + asset コピー + CPack 登録ヘルパ。
#   target            : 対象の実行ファイルターゲット名 (add_executable の名前)
#   ASSETS_DIR <dir>  : (任意) 同梱するアセット dir。ZIP 内では `<target>/assets/` に展開
#   DEST_PREFIX <name>: (任意) ZIP 内のサブディレクトリ名 (既定 = target 名)
function(acs_package_game target)
    cmake_parse_arguments(ACS_P "" "ASSETS_DIR;DEST_PREFIX" "" ${ARGN})
    if(NOT ACS_P_DEST_PREFIX)
        set(ACS_P_DEST_PREFIX "${target}")
    endif()

    # 1. exe 本体
    install(TARGETS ${target}
            RUNTIME DESTINATION "${ACS_P_DEST_PREFIX}"
            COMPONENT ACSGameRuntime)

    # 2. 依存 DLL (CMake 3.21+ の TARGET_RUNTIME_DLLS で実行時 DLL を自動収集)。
    #    Diligent / d3dcompiler.dll など PRIVATE link 経由のものも含まれる。
    install(FILES "$<TARGET_RUNTIME_DLLS:${target}>"
            DESTINATION "${ACS_P_DEST_PREFIX}"
            COMPONENT ACSGameRuntime
            OPTIONAL)

    # 3. PDB を任意で同梱 (Release では生成されないので OPTIONAL)。
    install(FILES "$<TARGET_PDB_FILE:${target}>"
            DESTINATION "${ACS_P_DEST_PREFIX}"
            COMPONENT ACSGameRuntime
            OPTIONAL)

    # 4. アセットを ZIP 内 <target>/assets/ に同梱
    if(ACS_P_ASSETS_DIR AND IS_DIRECTORY "${ACS_P_ASSETS_DIR}")
        install(DIRECTORY "${ACS_P_ASSETS_DIR}/"
                DESTINATION "${ACS_P_DEST_PREFIX}/assets"
                COMPONENT ACSGameRuntime)
    endif()

    # 5. このターゲットを CPack 対象として登録 (acs_enable_packaging が
    #    プロジェクト直下で呼ばれていなくても install rule は機能する)。
    set_property(GLOBAL APPEND PROPERTY _ACS_PACKAGED_GAMES "${target}")
endfunction()


# プロジェクト直下で 1 度呼ぶ。CPack の既定値を ACS 流に設定し、ZIP を生成。
# CPACK_GENERATOR 等を上書きしたい場合は本関数呼び出し前に set(CPACK_* ...) する。
# include(CPack) は本関数内で実行するので、利用者は呼ばなくてよい。
function(acs_enable_packaging)
    if(NOT EXISTS "${ACS_TREE_ROOT}/LICENSE")
        message(FATAL_ERROR "ACS: product license not found: ${ACS_TREE_ROOT}/LICENSE")
    endif()
    install(FILES "${ACS_TREE_ROOT}/LICENSE"
            DESTINATION "Licenses"
            RENAME "ACS-License.txt"
            COMPONENT ACSGameRuntime)

    if(NOT DEFINED CPACK_GENERATOR)
        set(CPACK_GENERATOR "ZIP")
    endif()
    if(NOT DEFINED CPACK_PACKAGE_FILE_NAME)
        set(CPACK_PACKAGE_FILE_NAME
            "${PROJECT_NAME}-${PROJECT_VERSION}-win64")
    endif()
    if(NOT DEFINED CPACK_PACKAGE_NAME)
        set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
    endif()
    if(NOT DEFINED CPACK_PACKAGE_VENDOR)
        set(CPACK_PACKAGE_VENDOR "ACS")
    endif()
    if(NOT DEFINED CPACK_PACKAGE_DESCRIPTION_SUMMARY)
        set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "ACS game package")
    endif()
    set(CPACK_VERBATIM_VARIABLES TRUE)
    # ZIP 内のトップディレクトリ名 (PACKAGE_FILE_NAME と同じ)
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "${CPACK_PACKAGE_FILE_NAME}")
    # トップに余計な階層を作らない (ZIP 展開後すぐ <target>/ が見える)
    set(CPACK_PACKAGE_RELOCATABLE TRUE)
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY FALSE)
    set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY FALSE)
    # 宣言したゲーム実行物だけを収録し、依存プロジェクトの install 規則を隔離する。
    set(CPACK_COMPONENTS_ALL ACSGameRuntime)
    set(CPACK_ARCHIVE_COMPONENT_INSTALL TRUE)
    set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
    set(CPACK_INSTALL_CMAKE_PROJECTS
        "${CMAKE_BINARY_DIR};${PROJECT_NAME};ACSGameRuntime;/")

    # PARENT_SCOPE で渡さないと include(CPack) に届かない
    set(CPACK_GENERATOR             "${CPACK_GENERATOR}"             PARENT_SCOPE)
    set(CPACK_PACKAGE_FILE_NAME     "${CPACK_PACKAGE_FILE_NAME}"     PARENT_SCOPE)
    set(CPACK_PACKAGE_NAME          "${CPACK_PACKAGE_NAME}"          PARENT_SCOPE)
    set(CPACK_PACKAGE_VENDOR        "${CPACK_PACKAGE_VENDOR}"        PARENT_SCOPE)
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "${CPACK_PACKAGE_DESCRIPTION_SUMMARY}" PARENT_SCOPE)
    set(CPACK_VERBATIM_VARIABLES    TRUE                             PARENT_SCOPE)
    set(CPACK_PACKAGE_INSTALL_DIRECTORY
        "${CPACK_PACKAGE_INSTALL_DIRECTORY}" PARENT_SCOPE)
    set(CPACK_PACKAGE_RELOCATABLE   TRUE                             PARENT_SCOPE)
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY
        FALSE PARENT_SCOPE)
    set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY
        FALSE PARENT_SCOPE)
    set(CPACK_COMPONENTS_ALL        ACSGameRuntime                   PARENT_SCOPE)
    set(CPACK_ARCHIVE_COMPONENT_INSTALL
        TRUE PARENT_SCOPE)
    set(CPACK_COMPONENTS_GROUPING
        ALL_COMPONENTS_IN_ONE PARENT_SCOPE)
    set(CPACK_INSTALL_CMAKE_PROJECTS
        "${CPACK_INSTALL_CMAKE_PROJECTS}" PARENT_SCOPE)

    include(CPack)

    # どのターゲットがパッケージ対象か configure ログに出す (デバッグ補助)
    get_property(_pkg_targets GLOBAL PROPERTY _ACS_PACKAGED_GAMES)
    if(_pkg_targets)
        list(JOIN _pkg_targets ", " _pkg_csv)
        message(STATUS "ACS: 配布パッケージ対象ターゲット: ${_pkg_csv}")
        message(STATUS "       ZIP 生成: cmake --build <build> --target package")
    else()
        message(STATUS "ACS: 配布パッケージ対象ターゲットなし (acs_package_game を呼ぶと自動登録)")
    endif()
endfunction()
