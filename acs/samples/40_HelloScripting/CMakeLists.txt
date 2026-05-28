# HelloScripting — IScriptVm (Lua 5.4 backend) のコンソールデモ。
#
# 構成:
#   main.cpp              - ScriptDemoApp::Run() を呼ぶだけのエントリ
#   ScriptDemoApp.{h,cpp} - VM Init → 4 デモ (CallFunction / native callback /
#                           globals / GC) → Shutdown
#
# stub mode (default): SDK 不要でビルド可能。全 API が NotImplemented。
# real mode: -DACS_BUILD_SCRIPTING=ON で Lua 5.4 を FetchContent 取得し
#            ACS::Scripting (FLuaVm) をリンク。window / renderer 不要のコンソール。
add_executable(hello_scripting
    main.cpp
    ScriptDemoApp.cpp
    ScriptDemoApp.h
)
acs_apply_compiler_options(hello_scripting)
target_include_directories(hello_scripting PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# IScriptVm / ScriptVmStub / GetVmStub は GameFramework module にある。
target_link_libraries(hello_scripting PRIVATE ACS::GameFramework)

# real backend が有効なときだけ ACS::Scripting をリンク (WITH_ACS_SCRIPTING=1 が
# transitive に付き、ソースの #if 分岐が real Lua を選ぶ)。
if(ACS_BUILD_SCRIPTING)
    target_link_libraries(hello_scripting PRIVATE ACS::Scripting)
endif()

source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
