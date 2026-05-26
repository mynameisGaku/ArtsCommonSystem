# 「Hello MVVM」サンプル: Observable / Binder / Derived / Command を ImGui で紹介
#
# 構成:
#   main.cpp             - ACS_DEFINE_MAIN(HelloMVVMApp) のみ
#   PlayerVM.h           - ViewModel 定義 (Observable / ObservableArray / Command)
#   HelloMVVMApp.{h,cpp} - Application 派生クラス (ImGui で MVVM 各機能を順紹介)
add_executable(hello_mvvm WIN32
    main.cpp
    HelloMVVMApp.cpp
    HelloMVVMApp.h
    PlayerVM.h
)
acs_apply_compiler_options(hello_mvvm)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を "HelloMVVMApp.h"
# のように local include で書けるようにする)
target_include_directories(hello_mvvm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_mvvm PRIVATE
    ACS::Game
    ACS::Event   # 標準セットに含まれない追加モジュール
    ACS::Imgui
    ACS::Mvvm
)
# Solution Explorer 内で .cpp と .h を Source Files / Header Files にグループ化。
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
