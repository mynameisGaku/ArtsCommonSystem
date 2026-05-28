# HelloSteamworks — ISteamworksBridge (実績 / stat / leaderboard / cloud) デモ。
#
# 構成:
#   main.cpp                  - ACS_DEFINE_MAIN(SteamworksDemoApp) のみ
#   SteamworksDemoApp.{h,cpp} - Application 派生クラス
#
# stub mode (default): SDK 不要でビルド可能。全 API が NotImplemented を返す。
# real mode: -DACS_BUILD_STEAMWORKS=ON -DACS_STEAMWORKS_SDK_DIR=<sdk> で
#            ACS::Steamworks をリンク + steam_api64.dll / steam_appid.txt を配備。
add_executable(hello_steamworks WIN32
    main.cpp
    SteamworksDemoApp.cpp
    SteamworksDemoApp.h
)
acs_apply_compiler_options(hello_steamworks)
target_include_directories(hello_steamworks PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# ISteamworksBridge (Stub) は GameFramework module にあるため明示 link が必要。
# GameFramework は Imgui に依存するので transitive で入る。
target_link_libraries(hello_steamworks PRIVATE ACS::Game ACS::GameFramework)

# real backend が有効なときだけ ACS::Steamworks をリンク + runtime 配備。
if(ACS_BUILD_STEAMWORKS)
    target_link_libraries(hello_steamworks PRIVATE ACS::Steamworks)
    acs_steamworks_runtime(hello_steamworks APPID 480)
endif()

source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
