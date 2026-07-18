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
    linear_allocator_lifecycle_tests.cpp
    memory_system_tests.cpp
    tlsf_safety_tests.cpp
    virtual_memory_tests.cpp
    mimalloc_allocator_tests.cpp
    sharded_tlsf_lifecycle_tests.cpp
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
    provider_thread_safety_tests.cpp
    objectref_runtime_tests.cpp
    prefab_spawn_tests.cpp
    eventbus_tests.cpp
    spawn_subsystem_tests.cpp
    camerarig_tests.cpp
    spritesortlist_tests.cpp
    editor_play_draw_tests.cpp
    material2d_tests.cpp
    asset_registry_tests.cpp
    save_archive_tests.cpp
    content_moderator_tests.cpp
    director_reentrancy_tests.cpp
    rollback_tests.cpp
    rollback_session_tests.cpp
    projectile_reentrancy_tests.cpp
    camerastack_tests.cpp
    collisionworld2d_tests.cpp
    particleeffect_tests.cpp
    assetpack_lifetime_tests.cpp
    application_lifetime_tests.cpp
    win32_resource_tests.cpp
)
set_target_properties(acs_unit_tests PROPERTIES FOLDER "Engine/Tests")

acs_apply_compiler_options(acs_unit_tests)

# LinearAllocator の Reset 競合を他モジュールのリンク状態から独立して反復できる専用ターゲット。
add_executable(acs_linear_allocator_lifecycle_tests
    main.cpp
    linear_allocator_lifecycle_tests.cpp
)
set_target_properties(acs_linear_allocator_lifecycle_tests PROPERTIES FOLDER "Engine/Tests")
acs_apply_compiler_options(acs_linear_allocator_lifecycle_tests)
target_link_libraries(acs_linear_allocator_lifecycle_tests PRIVATE
    ACS::Foundation
    ACS::Threading
    ACS::Memory
    ACS::Test
)
add_test(NAME ACS.LinearAllocatorLifecycle COMMAND acs_linear_allocator_lifecycle_tests)
set_tests_properties(ACS.LinearAllocatorLifecycle PROPERTIES TIMEOUT 30)

# AudioEngine の MTA cookie と Shutdown 競合を実デバイスなしで検証する。
target_compile_definitions(acs_audio PRIVATE ACS_AUDIO_TEST_HOOKS=1)
add_executable(acs_audio_engine_lifetime_tests audio_engine_lifetime_tests.cpp)
set_target_properties(acs_audio_engine_lifetime_tests PROPERTIES FOLDER "Engine/Tests")
acs_apply_compiler_options(acs_audio_engine_lifetime_tests)
target_compile_definitions(acs_audio_engine_lifetime_tests PRIVATE ACS_AUDIO_TEST_HOOKS=1)
target_link_libraries(acs_audio_engine_lifetime_tests PRIVATE
    ACS::Foundation
    ACS::Threading
    ACS::Audio
)
add_test(NAME ACS.AudioEngineLifetime COMMAND acs_audio_engine_lifetime_tests)
set_tests_properties(ACS.AudioEngineLifetime PROPERTIES TIMEOUT 30)

# GameFramework の XAudio2 backend を基盤依存だけで隔離して寿命競合を検証する。
add_executable(acs_xaudio2_backend_lifetime_tests
    xaudio2_backend_lifetime_tests.cpp
    "${ACS_SOURCE_ROOT}/gameframework/audio_backend/XAudio2Backend.cpp"
)
set_target_properties(acs_xaudio2_backend_lifetime_tests PROPERTIES FOLDER "Engine/Tests")
acs_apply_compiler_options(acs_xaudio2_backend_lifetime_tests)
target_compile_definitions(acs_xaudio2_backend_lifetime_tests PRIVATE
    ACS_XAUDIO2_BACKEND_TEST_HOOKS=1
)
target_link_libraries(acs_xaudio2_backend_lifetime_tests PRIVATE
    ACS::Foundation
    ACS::Threading
    ACS::Memory
    ACS::Container
    xaudio2
    ole32
)
add_test(NAME ACS.XAudio2BackendLifetime COMMAND acs_xaudio2_backend_lifetime_tests)
set_tests_properties(ACS.XAudio2BackendLifetime PROPERTIES TIMEOUT 30)

# メモリライフサイクル競合を他モジュールのリンク状態から独立して反復できる専用ターゲット。
add_executable(acs_sharded_tlsf_lifecycle_tests
    main.cpp
    sharded_tlsf_lifecycle_tests.cpp
)
set_target_properties(acs_sharded_tlsf_lifecycle_tests PROPERTIES FOLDER "Engine/Tests")
acs_apply_compiler_options(acs_sharded_tlsf_lifecycle_tests)
target_link_libraries(acs_sharded_tlsf_lifecycle_tests PRIVATE
    ACS::Foundation
    ACS::Threading
    ACS::Memory
    ACS::Test
)
add_test(NAME ACS.ShardedTlsfLifecycle COMMAND acs_sharded_tlsf_lifecycle_tests)
set_tests_properties(ACS.ShardedTlsfLifecycle PROPERTIES TIMEOUT 30)

# mimalloc の所有権 CAS とライフサイクル競合を単独で反復する専用ターゲット。
add_executable(acs_mimalloc_allocator_tests
    main.cpp
    mimalloc_allocator_tests.cpp
)
set_target_properties(acs_mimalloc_allocator_tests PROPERTIES FOLDER "Engine/Tests")
acs_apply_compiler_options(acs_mimalloc_allocator_tests)
target_link_libraries(acs_mimalloc_allocator_tests PRIVATE
    ACS::Foundation
    ACS::Threading
    ACS::Memory
    ACS::Test
)
add_test(NAME ACS.MimallocAllocator COMMAND acs_mimalloc_allocator_tests)
set_tests_properties(ACS.MimallocAllocator PROPERTIES TIMEOUT 60)

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

if(MSVC AND CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT ACS_ENABLE_ADDRESS_SANITIZER)
    add_test(NAME ACS.CrtDumpMemoryLeaksClean
             COMMAND acs_unit_tests --crt-dump-memory-leaks-clean-probe)
    add_test(NAME ACS.CrtDumpMemoryLeaksPositive
             COMMAND acs_unit_tests --crt-dump-memory-leaks-positive-probe)
    set_tests_properties(ACS.CrtDumpMemoryLeaksClean ACS.CrtDumpMemoryLeaksPositive
                         PROPERTIES TIMEOUT 30)
endif()

if(TARGET ACS::EditorAbi)
    add_executable(acs_editor_abi_lifecycle_tests editor_abi_lifecycle_tests.cpp)
    set_target_properties(acs_editor_abi_lifecycle_tests PROPERTIES FOLDER "Engine/Tests")
    acs_apply_compiler_options(acs_editor_abi_lifecycle_tests)
    target_link_libraries(acs_editor_abi_lifecycle_tests PRIVATE ACS::EditorAbi)
    add_test(NAME ACS.EditorAbiLifecycle COMMAND acs_editor_abi_lifecycle_tests)
endif()
