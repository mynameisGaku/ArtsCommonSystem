# 「HelloTextured」サンプル: テクスチャ + サンプラ + 3D 描画
#
# 構成:
#   main.cpp                 - ACS_DEFINE_MAIN(HelloTexturedApp) のみ
#   Types.h                  - 頂点フォーマット + キューブ 24 頂点 + UV + テクスチャサイズ
#   Shaders.h                - HLSL ソース (VS + PS + texture sampling)
#   TextureGen.{h,cpp}       - 手続き生成テクスチャ
#   HelloTexturedApp.{h,cpp} - Application 派生クラス
add_executable(hello_textured WIN32
    main.cpp
    HelloTexturedApp.cpp
    TextureGen.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    Types.h
    Shaders.h
    TextureGen.h
    HelloTexturedApp.h
)
acs_apply_compiler_options(hello_textured)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を "HelloTexturedApp.h"
# のように local include で書けるようにする)
target_include_directories(hello_textured PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_textured PRIVATE ACS::Game)
