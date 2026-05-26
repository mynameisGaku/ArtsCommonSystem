# 「HelloUI」サンプル: ACS 純正 UI + MVVM (FTwoWayBinder)
#
# 構成:
#   main.cpp             - ACS_DEFINE_MAIN(HelloUIApp) のみ
#   PlayerVM.h           - Player FViewModel
#   HelloUIApp.{h,cpp}   - FApplication 派生クラス + FWidget tree 構築
add_executable(hello_ui WIN32
    main.cpp
    HelloUIApp.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    PlayerVM.h
    HelloUIApp.h
)
acs_apply_compiler_options(hello_ui)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を "HelloUIApp.h"
# のように local include で書けるようにする)
target_include_directories(hello_ui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_ui PRIVATE
    ACS::Game
    ACS::Event   # 標準セットに含まれない追加モジュール
    ACS::Mvvm
    ACS::Ui
)
# Solution Explorer 内で .cpp と .h を Source Files / Header Files にグループ化。
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
