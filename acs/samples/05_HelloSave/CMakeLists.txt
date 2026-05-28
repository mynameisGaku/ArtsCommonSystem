## HelloSave — Storage で設定 / セーブデータの永続化。
##
## 構成:
##   main.cpp              - ACS_DEFINE_MAIN(HelloSaveApp) のみ
##   HelloSaveApp.{h,cpp}  - Application 派生クラス
add_executable(hello_save WIN32
    main.cpp
    HelloSaveApp.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    HelloSaveApp.h
)
acs_apply_compiler_options(hello_save)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を
# "HelloSaveApp.h" のように local include で書けるようにする)
target_include_directories(hello_save PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_save PRIVATE ACS::Game)
