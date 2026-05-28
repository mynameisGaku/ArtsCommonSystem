# 「HelloParticles」サンプル: 2D パーティクルエンジン
#
# 構成:
#   main.cpp                  - ACS_DEFINE_MAIN(HelloParticlesApp) のみ
#   Types.h                   - パーティクルテクスチャサイズ + preset 名一覧
#   HelloParticlesApp.{h,cpp} - Application 派生クラス
#   ParticleScene.{h,cpp}     - ParticleSystem の管理 + 描画
add_executable(hello_particles WIN32
    main.cpp
    HelloParticlesApp.cpp
    ParticleScene.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    Types.h
    HelloParticlesApp.h
    ParticleScene.h
)
acs_apply_compiler_options(hello_particles)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を "Types.h"
# のように local include で書けるようにする)
target_include_directories(hello_particles PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_particles PRIVATE ACS::Game)
