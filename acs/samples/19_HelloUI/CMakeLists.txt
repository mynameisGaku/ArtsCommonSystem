# 「HelloUI」サンプル: ACS 純正 UI + MVVM (TwoWayBinder)
#
# 構成:
#   main.cpp             - ACS_DEFINE_MAIN(HelloUIApp) のみ
#   PlayerVM.h           - Player ViewModel
#   HelloUIApp.{h,cpp}   - Application 派生クラス + Widget tree 構築
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
