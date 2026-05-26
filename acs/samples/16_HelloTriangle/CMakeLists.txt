# 「HelloTriangle」サンプル: 3 角形を描画する最小例 (低レベル RHI トラックの 1 本目)
#
# 構成:
#   main.cpp                 - ACS_DEFINE_MAIN(HelloTriangleApp) のみ
#   Types.h                  - 頂点フォーマット + 三角形 3 頂点
#   Shaders.h                - HLSL ソース (VS + PS)
#   HelloTriangleApp.{h,cpp} - Application 派生クラス
add_executable(hello_triangle WIN32
    main.cpp
    HelloTriangleApp.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    Types.h
    Shaders.h
    HelloTriangleApp.h
)
acs_apply_compiler_options(hello_triangle)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を "HelloTriangleApp.h"
# のように local include で書けるようにする)
target_include_directories(hello_triangle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_triangle PRIVATE ACS::Game)
# Solution Explorer 内で .cpp と .h を Source Files / Header Files にグループ化。
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
