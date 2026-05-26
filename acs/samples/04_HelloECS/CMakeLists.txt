## HelloECS — ECS / Event / Timer / 並列イテレーション デモ。
##
## 構成:
##   main.cpp             - ACS_DEFINE_MAIN(HelloECSApp) のみ
##   Types.{h,cpp}        - Position/Velocity/FColor POD + SpawnEvent + helper
##   HelloECSApp.{h,cpp}  - Application 派生クラス
add_executable(hello_ecs WIN32
    main.cpp
    HelloECSApp.cpp
    Types.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    HelloECSApp.h
    Types.h
)
acs_apply_compiler_options(hello_ecs)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を
# "HelloECSApp.h" のように local include で書けるようにする)
target_include_directories(hello_ecs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_ecs PRIVATE
    ACS::Game
    ACS::Event   # TimerManager / MessageBroker（標準セットに含まれない追加モジュール）
)
# Solution Explorer 内で .cpp と .h を Source Files / Header Files にグループ化。
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
