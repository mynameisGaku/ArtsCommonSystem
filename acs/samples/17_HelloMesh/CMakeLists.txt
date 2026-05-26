# 「HelloMesh」サンプル: 3D 回転キューブ + 深度テスト + 定数バッファ
#
# 構成:
#   main.cpp             - ACS_DEFINE_MAIN(HelloMeshApp) のみ
#   Types.h              - 頂点フォーマット + キューブ 24 頂点 / 36 indices
#   Shaders.h            - HLSL ソース (VS + PS + MVP cbuffer)
#   HelloMeshApp.{h,cpp} - FApplication 派生クラス
add_executable(hello_mesh WIN32
    main.cpp
    HelloMeshApp.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    Types.h
    Shaders.h
    HelloMeshApp.h
)
acs_apply_compiler_options(hello_mesh)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を "HelloMeshApp.h"
# のように local include で書けるようにする)
target_include_directories(hello_mesh PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_mesh PRIVATE ACS::Game)
# Solution Explorer 内で .cpp と .h を Source Files / Header Files にグループ化。
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
