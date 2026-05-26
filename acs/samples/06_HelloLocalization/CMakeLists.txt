## HelloLocalization — 多言語対応 (i18n) デモ。
##
## 構成:
##   main.cpp                       - ACS_DEFINE_MAIN(HelloLocalizationApp) のみ
##   Types.{h,cpp}                  - 埋め込み言語データ (kLangJa/En/Fr)
##   HelloLocalizationApp.{h,cpp}   - Application 派生クラス
add_executable(hello_localization WIN32
    main.cpp
    HelloLocalizationApp.cpp
    Types.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    HelloLocalizationApp.h
    Types.h
)
acs_apply_compiler_options(hello_localization)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を
# "HelloLocalizationApp.h" のように local include で書けるようにする)
target_include_directories(hello_localization PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_localization PRIVATE ACS::Game)
# Solution Explorer 内で .cpp と .h を Source Files / Header Files にグループ化。
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
