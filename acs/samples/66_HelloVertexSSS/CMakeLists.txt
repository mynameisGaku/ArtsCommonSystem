# 「Hello Vertex-Space SSS」サンプル: CVertexScatter (頂点空間サブサーフェス
# スキャタリング) で肌の内部散乱を表現する例。
#
# 構成:
#   main.cpp                  - ACS_DEFINE_MAIN(HelloVertexSssApp) のみ
#   HelloVertexSssApp.{h,cpp} - Application 派生 (自前シェーダ + 2 球 + 旋回光)
add_executable(hello_vertex_sss WIN32
    main.cpp
    HelloVertexSssApp.cpp
    HelloVertexSssApp.h
)
acs_apply_compiler_options(hello_vertex_sss)
target_include_directories(hello_vertex_sss PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_vertex_sss PRIVATE ACS::Game)
