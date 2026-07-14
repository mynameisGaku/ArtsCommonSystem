add_executable(acs_unit_tests
    main.cpp
    foundation_tests.cpp
    cast_tests.cpp
    reflect_tests.cpp
    reflect_serialize_tests.cpp
    reflect_apply_tests.cpp
    spatialaudio2d_tests.cpp
    audio_backend_tests.cpp
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
    virtual_memory_tests.cpp
    mimalloc_allocator_tests.cpp
    crt_debug_heap_diagnostics_tests.cpp
    smartptr_tests.cpp
    objectpool_tests.cpp
    allocator_ownership_tests.cpp
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
    diligent_memory_adapter_tests.cpp
    gpu_live_object_diagnostics_tests.cpp
    vertexscatter_tests.cpp
    node3d_tests.cpp
    subsystem_tests.cpp
    objectref_runtime_tests.cpp
    prefab_spawn_tests.cpp
    eventbus_tests.cpp
    spawn_subsystem_tests.cpp
    camerarig_tests.cpp
    spritesortlist_tests.cpp
    editor_play_draw_tests.cpp
    material2d_tests.cpp
    asset_registry_tests.cpp
    application_lifetime_tests.cpp
    win32_resource_tests.cpp
)
set_target_properties(acs_unit_tests PROPERTIES FOLDER "Engine/Tests")

acs_apply_compiler_options(acs_unit_tests)

# GameFramework の失敗注入は unit test を構成した build にだけ組み込む。
target_compile_definitions(acs_gameframework PRIVATE ACS_GAMEFRAMEWORK_TEST_HOOKS=1)
target_compile_definitions(acs_unit_tests PRIVATE ACS_GAMEFRAMEWORK_TEST_HOOKS=1)

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
    ACS::Network
    ACS::Collision
    ACS::AssetPack
    ACS::App
    ACS::Test
)

if(TARGET acs_third_party_diligent_core)
    # アダプタ契約テストだけが Diligent の IMemoryAllocator 定義を直接参照する。
    target_link_libraries(acs_unit_tests PRIVATE acs_third_party::diligent_core)

    add_executable(acs_diligent_memory_adapter_lifetime_tests diligent_memory_adapter_lifetime_tests.cpp)
    set_target_properties(acs_diligent_memory_adapter_lifetime_tests PROPERTIES FOLDER "Engine/Tests")
    acs_apply_compiler_options(acs_diligent_memory_adapter_lifetime_tests)
    target_link_libraries(acs_diligent_memory_adapter_lifetime_tests PRIVATE
        ACS::Foundation
        ACS::Memory
        ACS::Render
        acs_third_party::diligent_core
    )
    add_test(NAME ACS.DiligentMemoryAdapterLifetime COMMAND acs_diligent_memory_adapter_lifetime_tests)
    add_test(NAME ACS.DiligentMemoryAdapterGenerationChangeDuringFree
             COMMAND acs_diligent_memory_adapter_lifetime_tests --generation-change-during-free)
endif()

if(TARGET ACS::Imgui)
    target_sources(acs_unit_tests PRIVATE imgui_lifetime_tests.cpp)
    target_link_libraries(acs_unit_tests PRIVATE ACS::Imgui)
endif()

if(TARGET ACS::MlOnnx)
    target_sources(acs_unit_tests PRIVATE mlonnx_failure_tests.cpp)
    target_link_libraries(acs_unit_tests PRIVATE ACS::MlOnnx)
    acs_onnxruntime_runtime(acs_unit_tests)
endif()

add_test(NAME ACS.UnitTests COMMAND acs_unit_tests)
set_tests_properties(ACS.UnitTests PROPERTIES TIMEOUT 120)

if(TARGET ACS::EditorAbi)
    add_executable(acs_editor_abi_lifecycle_tests editor_abi_lifecycle_tests.cpp)
    set_target_properties(acs_editor_abi_lifecycle_tests PROPERTIES FOLDER "Engine/Tests")
    acs_apply_compiler_options(acs_editor_abi_lifecycle_tests)
    target_link_libraries(acs_editor_abi_lifecycle_tests PRIVATE ACS::EditorAbi)
    add_test(NAME ACS.EditorAbiLifecycle COMMAND acs_editor_abi_lifecycle_tests)
endif()
