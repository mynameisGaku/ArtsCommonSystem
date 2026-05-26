# 「HelloModel」サンプル: 標準ライティング + 手続きプリミティブ + 非同期ロード
#
# 構成:
#   main.cpp              - ACS_DEFINE_MAIN(HelloModelApp) のみ
#   Types.h               - カメラ初期値 / ライト / マテリアル色 etc. 定数
#   HelloModelApp.{h,cpp} - Application 派生クラス
#   ModelScene.{h,cpp}    - プリミティブ + カメラ + 描画
add_executable(hello_model WIN32
    main.cpp
    HelloModelApp.cpp
    ModelScene.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    Types.h
    HelloModelApp.h
    ModelScene.h
)
acs_apply_compiler_options(hello_model)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を "Types.h"
# のように local include で書けるようにする)
target_include_directories(hello_model PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_model PRIVATE ACS::Game)
# Solution Explorer 内で .cpp と .h を Source Files / Header Files にグループ化。
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
