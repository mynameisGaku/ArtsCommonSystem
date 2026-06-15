add_executable(acs_unit_tests
    main.cpp
    foundation_tests.cpp
    cast_tests.cpp
    reflect_tests.cpp
    reflect_serialize_tests.cpp
    reflect_apply_tests.cpp
    spatialaudio2d_tests.cpp
    scene_serialize_tests.cpp
    component_factory_tests.cpp
    component_services_tests.cpp
    input_feed_tests.cpp
    rigidworld_tests.cpp
    rigidbody_component_tests.cpp
    debugdraw_tests.cpp
    tilemap_tests.cpp
    steering_tests.cpp
    camera_tests.cpp
    behaviortree_tests.cpp
    threading_tests.cpp
    memory_tests.cpp
    memory_system_tests.cpp
    smartptr_tests.cpp
    objectpool_tests.cpp
    container_tests.cpp
    math_tests.cpp
    ecs_tests.cpp
    event_tests.cpp
    jobgraph_tests.cpp
    mvvm_tests.cpp
    ui_tests.cpp
    uilayer_tests.cpp
    json_tests.cpp
    collider_tests.cpp
    crypto_tests.cpp
    render_tests.cpp
    vertexscatter_tests.cpp
    node3d_tests.cpp
    subsystem_tests.cpp
    objectref_runtime_tests.cpp
    prefab_spawn_tests.cpp
    camerarig_tests.cpp
    spritesortlist_tests.cpp
    editor_play_draw_tests.cpp
    material2d_tests.cpp
)
set_target_properties(acs_unit_tests PROPERTIES FOLDER "Engine/Tests")

acs_apply_compiler_options(acs_unit_tests)

# UI tests は Widget tree のレイアウト計算 / 入力ハンドリング部分を検証する。
# UiRenderer.cpp は Render に依存するため、Ui を link すると Render / Platform /
# Asset も transitive で必要。Render 経由で GPU 系のコードが入るが、tests は
# 描画は行わないので Renderer を実体化しなければ実行時影響無し。
# uilayer_tests は FUiLayer (gameframework) を検証するため GameFramework を link する
# (HandleInput に合成 Event を流す純 logic テストで、GPU/Scene は実体化しない)。
target_link_libraries(acs_unit_tests PRIVATE
    ACS::Foundation
    ACS::Threading
    ACS::Memory
    ACS::Container
    ACS::Math
    ACS::Ecs
    ACS::Event
    ACS::Mvvm
    ACS::Ui
    ACS::Render
    ACS::GameFramework
    ACS::Collision
    ACS::AssetPack
    ACS::Test
)

add_test(NAME ACS.UnitTests COMMAND acs_unit_tests)
