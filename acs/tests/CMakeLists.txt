find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

if(WIN32)
    find_program(ACS_PROJECT_OPERATIONS_POWERSHELL NAMES powershell.exe powershell)
    if(ACS_PROJECT_OPERATIONS_POWERSHELL)
        add_test(
            NAME ACS.ProjectOperationsSelfTest
            COMMAND "${ACS_PROJECT_OPERATIONS_POWERSHELL}"
                    -NoLogo -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_CURRENT_SOURCE_DIR}/project_operations_self_test.ps1")
        set_tests_properties(
            ACS.ProjectOperationsSelfTest
            PROPERTIES LABELS "StaticAnalysis;BuildSystem" TIMEOUT 60)
    endif()
endif()

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
    scene_timer_handle_tests.cpp
    component_factory_tests.cpp
    component_services_tests.cpp
    input_axis_options_tests.cpp
    input_feed_tests.cpp
    input_recording_view_tests.cpp
    rigidworld_tests.cpp
    rigidbody_component_tests.cpp
    debugdraw_tests.cpp
    draw_immediate_tests.cpp
    scene_travel_context_tests.cpp
    tilemap_tests.cpp
    steering_tests.cpp
    camera_tests.cpp
    behaviortree_tests.cpp
    behavior_tree_persistence_safety_tests.cpp
    easing_completeness_tests.cpp
    legacy_kit_ease_id_codec_tests.cpp
    fixed_step_clock_tests.cpp
    easing_tween_integration_tests.cpp
    easing_tween_numeric_safety_tests.cpp
    easy_easing_facade_tests.cpp
    animation_curve_safety_tests.cpp
    animation_curve_persistence_tests.cpp
    beat_grid_hold_safety_tests.cpp
    hot_reload_safety_tests.cpp
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
    aobject_header_compile_tests.cpp
    object_ptr_header_compile_tests.cpp
    component_id_header_compile_tests.cpp
    ecs_tests.cpp
    event_type_id_header_compile_tests.cpp
    event_tests.cpp
    delegate_tests.cpp
    message_broker_safety_tests.cpp
    simple_delegate_tests.cpp
    typed_event_tests.cpp
    jobgraph_tests.cpp
    mvvm_tests.cpp
    observable_array_lifetime_safety_tests.cpp
    ui_tests.cpp
    uilayer_tests.cpp
    json_tests.cpp
    collider_tests.cpp
    crypto_tests.cpp
    render_tests.cpp
    render_graph_transient_alias_tests.cpp
    rhi_command_statistics_tests.cpp
    diligent_memory_adapter_tests.cpp
    gpu_live_object_diagnostics_tests.cpp
    vertexscatter_tests.cpp
    post_effect_quality_tests.cpp
    post_effect_workload_tests.cpp
    node3d_tests.cpp
    legacy_scene3d_water_runtime_tests.cpp
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
    substrate_expression_tests.cpp
    substrate_expression_persistence_tests.cpp
    pbr_expression_contract_tests.cpp
    linear_lighting_contract_tests.cpp
    substrate_material_tests.cpp
    settings_safety_tests.cpp
    script_host_safety_tests.cpp
    net_snapshot_safety_tests.cpp
    replay_director_safety_tests.cpp
    sprite_pack_safety_tests.cpp
    studio_workflow_safety_tests.cpp
    asset_registry_tests.cpp
    save_archive_tests.cpp
    save_integration_tests.cpp
    savegame_archive_safety_v2_tests.cpp
    content_moderator_tests.cpp
    director_reentrancy_tests.cpp
    rollback_tests.cpp
    rollback_session_tests.cpp
    projectile_reentrancy_tests.cpp
    anode_tests.cpp
    camerastack_tests.cpp
    collisionworld2d_tests.cpp
    particleeffect_tests.cpp
    water2d_ripple_lifetime_tests.cpp
    water3d_ripple_lifetime_tests.cpp
    foundation_optimization_wave_b_tests.cpp
    foundation_public_layout_tests.cpp
    foundation_optimization_wave_k_tests.cpp
    foundation_optimization_wave_i_tests.cpp
    foundation_optimization_wave_m_tests.cpp
    volumetric_cloud_world_anchor_tests.cpp
    atmosphere_composite_tests.cpp
    shadow_map_cb_safety_tests.cpp
    assetpack_lifetime_tests.cpp
    assetpack_manifest_safety_tests.cpp
    application_lifetime_tests.cpp
    application_subsystem_tests.cpp
    subsystem_canonical_header_tests.cpp
    win32_resource_tests.cpp
    network_socket_io_tests.cpp
)
set_target_properties(acs_unit_tests PROPERTIES FOLDER "Engine/Tests")

acs_apply_compiler_options(acs_unit_tests)

# CDebugDraw の一括追加と確保失敗時の不変条件を、他の GameFramework テストから分離して検証する。
add_executable(acs_debug_draw_2d_transactional_tests
    main.cpp
    debugdraw_tests.cpp
)
set_target_properties(acs_debug_draw_2d_transactional_tests PROPERTIES FOLDER "Engine/Tests")
acs_apply_compiler_options(acs_debug_draw_2d_transactional_tests)
target_link_libraries(acs_debug_draw_2d_transactional_tests PRIVATE
    ACS::GameFramework
    ACS::Test
)
add_test(NAME ACS.DebugDraw2DTransactional COMMAND acs_debug_draw_2d_transactional_tests)
set_tests_properties(ACS.DebugDraw2DTransactional PROPERTIES
    LABELS "GameFramework;Safety;Compatibility"
    TIMEOUT 60)

# Log の失敗注入と同期点は製品 acs_foundation に混ぜず、この専用実行ファイルだけで再コンパイルする。
add_executable(acs_log_sink_registry_tests
    main.cpp
    log_sink_registry_tests.cpp
    "${ACS_SOURCE_ROOT}/foundation/Log.cpp"
    "${ACS_SOURCE_ROOT}/foundation/LogSinkSubscription.cpp"
)
set_target_properties(acs_log_sink_registry_tests PROPERTIES FOLDER "Engine/Tests")
acs_apply_compiler_options(acs_log_sink_registry_tests)
target_compile_definitions(acs_log_sink_registry_tests PRIVATE ACS_FOUNDATION_LOG_TEST_HOOKS=1)
target_link_libraries(acs_log_sink_registry_tests PRIVATE
    ACS::Foundation
    ACS::Threading
    ACS::Memory
    ACS::Test
)
add_test(NAME ACS.LogSinkRegistry COMMAND acs_log_sink_registry_tests)
set_tests_properties(ACS.LogSinkRegistry PROPERTIES LABELS "Foundation;Concurrency" TIMEOUT 60)

# CDebugDraw3Dの生成失敗を実GPUなしで注入し、Render本体と分離して検証する。
add_executable(acs_debug_draw_3d_safety_tests
    main.cpp
    debug_draw_3d_safety_tests.cpp
    "${ACS_SOURCE_ROOT}/render/DebugDraw.cpp"
)
set_target_properties(acs_debug_draw_3d_safety_tests PROPERTIES FOLDER "Engine/Tests")
acs_apply_compiler_options(acs_debug_draw_3d_safety_tests)
target_link_libraries(acs_debug_draw_3d_safety_tests PRIVATE
    ACS::Foundation
    ACS::Memory
    ACS::Container
    ACS::Math
    ACS::Test
)
add_test(NAME ACS.DebugDraw3DSafety COMMAND acs_debug_draw_3d_safety_tests)
set_tests_properties(ACS.DebugDraw3DSafety PROPERTIES LABELS "Render;Safety;Compatibility" TIMEOUT 60)

# Randomの既存列、layout、検査済みKit吸収APIを独立して反復する。
add_executable(acs_random_compatibility_tests
    main.cpp
    random_compatibility_tests.cpp
)
set_target_properties(acs_random_compatibility_tests PROPERTIES FOLDER "Engine/Tests")
acs_apply_compiler_options(acs_random_compatibility_tests)
target_link_libraries(acs_random_compatibility_tests PRIVATE
    ACS::GameFramework
    ACS::Test
)
add_test(NAME ACS.RandomCompatibility COMMAND acs_random_compatibility_tests)
set_tests_properties(ACS.RandomCompatibility PROPERTIES LABELS "GameFramework;Compatibility" TIMEOUT 60)

# Subsystem のtransaction、phase、再入解体を通常unit群から独立して再現する。
add_executable(acs_subsystem_lifecycle_tests
    main.cpp
    subsystem_tests.cpp
)
set_target_properties(acs_subsystem_lifecycle_tests PROPERTIES FOLDER "Engine/Tests")
acs_apply_compiler_options(acs_subsystem_lifecycle_tests)
target_link_libraries(acs_subsystem_lifecycle_tests PRIVATE
    ACS::Subsystem
    ACS::GameFramework
    ACS::Test
)
add_test(NAME ACS.SubsystemLifecycle COMMAND acs_subsystem_lifecycle_tests)
set_tests_properties(ACS.SubsystemLifecycle PROPERTIES LABELS "Subsystem;GameFramework;Lifecycle" TIMEOUT 60)

# catalog関数を直接呼ばず、static libraryのCGame anchorだけでbuiltin登録を検証する。
add_executable(acs_subsystem_static_link_tests
    main.cpp
    subsystem_static_link_tests.cpp
)
set_target_properties(acs_subsystem_static_link_tests PROPERTIES FOLDER "Engine/Tests")
acs_apply_compiler_options(acs_subsystem_static_link_tests)
target_link_libraries(acs_subsystem_static_link_tests PRIVATE
    ACS::GameFramework
    ACS::Test
)
add_test(NAME ACS.SubsystemStaticLink COMMAND acs_subsystem_static_link_tests)
set_tests_properties(ACS.SubsystemStaticLink PROPERTIES LABELS "Subsystem;GameFramework;Compatibility" TIMEOUT 60)

# canonical top-level headerと旧GameFramework forwarding名を単独consumerで固定する。
add_executable(acs_subsystem_canonical_header_tests
    main.cpp
    app_forward_header_compile_tests.cpp
    gameframework_forward_header_compile_tests.cpp
    subsystem_acomponent_header_compile_tests.cpp
    subsystem_anode_header_compile_tests.cpp
    subsystem_canonical_header_tests.cpp
    subsystem_scene_header_compile_tests.cpp
    subsystem_scene_manager_header_compile_tests.cpp
    subsystem_spawn_header_compile_tests.cpp
)
set_target_properties(acs_subsystem_canonical_header_tests PROPERTIES FOLDER "Engine/Tests")
acs_apply_compiler_options(acs_subsystem_canonical_header_tests)
target_link_libraries(acs_subsystem_canonical_header_tests PRIVATE
    ACS::GameFramework
    ACS::Test
)
add_test(NAME ACS.SubsystemCanonicalHeaders COMMAND acs_subsystem_canonical_header_tests)
set_tests_properties(ACS.SubsystemCanonicalHeaders PROPERTIES LABELS "Subsystem;Compatibility" TIMEOUT 60)

# CApplication所有のEngine adapterが既存実体identityとphaseを保つ。
add_executable(acs_application_subsystem_tests
    main.cpp
    application_subsystem_tests.cpp
)
set_target_properties(acs_application_subsystem_tests PROPERTIES FOLDER "Engine/Tests")
acs_apply_compiler_options(acs_application_subsystem_tests)
target_link_libraries(acs_application_subsystem_tests PRIVATE
    ACS::App
    ACS::Test
)
add_test(NAME ACS.ApplicationSubsystem COMMAND acs_application_subsystem_tests)
set_tests_properties(ACS.ApplicationSubsystem PROPERTIES LABELS "Subsystem;App;Compatibility" TIMEOUT 60)

# FStorage の文字列一括設定、失敗時不変、入力上限を独立して反復する。
add_executable(acs_storage_transactional_batch_tests
    main.cpp
    storage_transactional_batch_tests.cpp
)
set_target_properties(acs_storage_transactional_batch_tests PROPERTIES FOLDER "Engine/Tests")
acs_apply_compiler_options(acs_storage_transactional_batch_tests)
target_link_libraries(acs_storage_transactional_batch_tests PRIVATE
    ACS::Platform
    ACS::Test
)
add_test(NAME ACS.StorageTransactionalBatch COMMAND acs_storage_transactional_batch_tests)
set_tests_properties(ACS.StorageTransactionalBatch PROPERTIES LABELS "Platform;Persistence;Compatibility" TIMEOUT 60)

# プリミティブメッシュ生成の入力検査と確保失敗時の一括反映を検証する。
add_executable(acs_mesh_primitive_safety_tests
    main.cpp
    mesh_primitive_safety_tests.cpp
)
set_target_properties(acs_mesh_primitive_safety_tests PROPERTIES FOLDER "Engine/Tests")
acs_apply_compiler_options(acs_mesh_primitive_safety_tests)
target_link_libraries(acs_mesh_primitive_safety_tests PRIVATE
    ACS::Asset
    ACS::Test
)
add_test(NAME ACS.MeshPrimitiveSafety COMMAND acs_mesh_primitive_safety_tests)
set_tests_properties(ACS.MeshPrimitiveSafety PROPERTIES LABELS "Asset;Safety;Compatibility" TIMEOUT 60)

# FBX loaderのcount検査、OOM時の一括破棄、旧geometry順を独立して反復する。
add_executable(acs_fbx_loader_transactional_tests
    main.cpp
    mesh_asset_fbx_transactional_tests.cpp
)
set_target_properties(acs_fbx_loader_transactional_tests PROPERTIES FOLDER "Engine/Tests")
acs_apply_compiler_options(acs_fbx_loader_transactional_tests)
target_link_libraries(acs_fbx_loader_transactional_tests PRIVATE
    ACS::Asset
    ACS::Test
)
add_test(NAME ACS.FbxLoaderTransactional COMMAND acs_fbx_loader_transactional_tests)
set_tests_properties(ACS.FbxLoaderTransactional PROPERTIES LABELS "Asset;Safety;Compatibility" TIMEOUT 60)

# 実機タイミングだけに依存せず、削減した呼び出し・起床回数を JSON で検証する。
add_executable(acs_foundation_performance
    foundation_performance_main.cpp
)
set_target_properties(acs_foundation_performance PROPERTIES FOLDER "Engine/Tests")
acs_apply_compiler_options(acs_foundation_performance)
target_include_directories(acs_foundation_performance PRIVATE "${ACS_SOURCE_ROOT}")
add_test(NAME ACS.FoundationPerformance COMMAND acs_foundation_performance)
set_tests_properties(ACS.FoundationPerformance PROPERTIES
    LABELS "Performance;Foundation"
    TIMEOUT 30)

# Foundation ライブラリをリンクせず、無効ログがシンボル参照ごと消えることを確認する。
add_executable(acs_foundation_log_compile_gate
    foundation_log_compile_gate_main.cpp
)
set_target_properties(acs_foundation_log_compile_gate PROPERTIES FOLDER "Engine/Tests")
acs_apply_compiler_options(acs_foundation_log_compile_gate)
target_include_directories(acs_foundation_log_compile_gate PRIVATE "${ACS_SOURCE_ROOT}")
add_test(NAME ACS.FoundationLogCompileGate COMMAND acs_foundation_log_compile_gate)
set_tests_properties(ACS.FoundationLogCompileGate PROPERTIES
    LABELS "Performance;Foundation"
    TIMEOUT 30)

add_test(
    NAME ACS.FoundationPerformanceVerifierSelfTest
    COMMAND "${Python3_EXECUTABLE}"
            "${ACS_TREE_ROOT}/scripts/verify_foundation_performance.py"
            --self-test)
add_test(
    NAME ACS.FoundationEndToEndAggregatorSelfTest
    COMMAND "${Python3_EXECUTABLE}"
            "${ACS_TREE_ROOT}/scripts/run_foundation_end_to_end.py"
            --self-test)
add_test(
    NAME ACS.ChangedCppRulesAuditSelfTest
    COMMAND "${Python3_EXECUTABLE}"
            "${ACS_TREE_ROOT}/scripts/audit_changed_cpp_rules.py"
            --self-test)
add_test(
    NAME ACS.FoundationPerformanceContract
    COMMAND "${Python3_EXECUTABLE}"
            "${ACS_TREE_ROOT}/scripts/verify_foundation_performance.py"
            --executable "$<TARGET_FILE:acs_foundation_performance>")
set_tests_properties(
    ACS.FoundationPerformanceVerifierSelfTest
    ACS.FoundationEndToEndAggregatorSelfTest
    ACS.ChangedCppRulesAuditSelfTest
    ACS.FoundationPerformanceContract
    PROPERTIES LABELS "Performance;Foundation" TIMEOUT 30)

# Foundation Optimization Wave B を他の長時間テストから分離して反復する。
add_executable(acs_foundation_optimization_wave_b_tests
    main.cpp
    foundation_optimization_wave_b_tests.cpp
)
set_target_properties(
    acs_foundation_optimization_wave_b_tests
    PROPERTIES FOLDER "Engine/Tests")
acs_apply_compiler_options(acs_foundation_optimization_wave_b_tests)
target_link_libraries(acs_foundation_optimization_wave_b_tests PRIVATE
    ACS::Foundation
    ACS::Threading
    ACS::Memory
    ACS::Container
    ACS::Platform
    ACS::Event
    ACS::Test
)
add_test(
    NAME ACS.FoundationOptimizationWaveB
    COMMAND acs_foundation_optimization_wave_b_tests)
set_tests_properties(
    ACS.FoundationOptimizationWaveB
    PROPERTIES LABELS "Foundation;Stress" TIMEOUT 60)

# 最適化対象の公開型が意図せず肥大化しないことをWin64 ABIで固定する。
add_executable(acs_foundation_public_layout_tests
    main.cpp
    foundation_public_layout_tests.cpp
)
set_target_properties(
    acs_foundation_public_layout_tests
    PROPERTIES FOLDER "Engine/Tests")
acs_apply_compiler_options(acs_foundation_public_layout_tests)
target_link_libraries(acs_foundation_public_layout_tests PRIVATE
    ACS::Foundation
    ACS::Threading
    ACS::Memory
    ACS::Container
    ACS::Platform
    ACS::Event
    ACS::Test
)
add_test(
    NAME ACS.FoundationPublicLayout
    COMMAND acs_foundation_public_layout_tests)
set_tests_properties(
    ACS.FoundationPublicLayout
    PROPERTIES LABELS "Foundation;ABI" TIMEOUT 30)

# 別process反復でglobal ownerの初期化・終了、queue、cancelの世代跨ぎを検証する。
add_test(
    NAME ACS.FoundationStressRunnerSelfTest
    COMMAND "${Python3_EXECUTABLE}"
            "${ACS_TREE_ROOT}/scripts/run_foundation_stress.py"
            --self-test
            --isolate-cwd)
add_test(
    NAME ACS.FoundationOwnershipStress
    COMMAND "${Python3_EXECUTABLE}"
            "${ACS_TREE_ROOT}/scripts/run_foundation_stress.py"
            --executable "$<TARGET_FILE:acs_foundation_optimization_wave_b_tests>"
            --iterations 32
            --timeout-seconds 60
            --isolate-cwd)
set_tests_properties(
    ACS.FoundationStressRunnerSelfTest
    ACS.FoundationOwnershipStress
    PROPERTIES LABELS "Foundation;Stress" TIMEOUT 2100)

# Foundation Optimization Wave K の mapped I/O / ownership / batch 契約を
# Debug / Release で独立反復する。
add_executable(acs_foundation_optimization_wave_k_tests
    main.cpp
    foundation_optimization_wave_k_tests.cpp
)
set_target_properties(
    acs_foundation_optimization_wave_k_tests
    PROPERTIES FOLDER "Engine/Tests")
acs_apply_compiler_options(acs_foundation_optimization_wave_k_tests)
target_compile_definitions(
    acs_foundation_optimization_wave_k_tests
    PRIVATE ACS_GAMEFRAMEWORK_TEST_HOOKS=1)
target_link_libraries(acs_foundation_optimization_wave_k_tests PRIVATE
    ACS::Foundation
    ACS::Threading
    ACS::Memory
    ACS::Container
    ACS::Platform
    ACS::Asset
    ACS::GameFramework
    ACS::AssetPack
    ACS::Test
)
add_test(
    NAME ACS.FoundationOptimizationWaveK
    COMMAND acs_foundation_optimization_wave_k_tests)
set_tests_properties(
    ACS.FoundationOptimizationWaveK
    PROPERTIES LABELS "Performance;Foundation" TIMEOUT 120)

# Foundation Optimization Wave I を並行性 stress と共に独立反復する。
add_executable(acs_foundation_optimization_wave_i_tests
    main.cpp
    foundation_optimization_wave_i_tests.cpp
)
set_target_properties(
    acs_foundation_optimization_wave_i_tests
    PROPERTIES FOLDER "Engine/Tests")
acs_apply_compiler_options(acs_foundation_optimization_wave_i_tests)
target_link_libraries(acs_foundation_optimization_wave_i_tests PRIVATE
    ACS::Foundation
    ACS::Threading
    ACS::Memory
    ACS::Test
)
add_test(
    NAME ACS.FoundationOptimizationWaveI
    COMMAND acs_foundation_optimization_wave_i_tests)
set_tests_properties(
    ACS.FoundationOptimizationWaveI
    PROPERTIES LABELS "Performance;Foundation;Stress" TIMEOUT 60)

# Foundation Optimization Wave M の completion / event / path 契約を独立反復する。
add_executable(acs_foundation_optimization_wave_m_tests
    main.cpp
    foundation_optimization_wave_m_tests.cpp
)
set_target_properties(
    acs_foundation_optimization_wave_m_tests
    PROPERTIES FOLDER "Engine/Tests")
acs_apply_compiler_options(acs_foundation_optimization_wave_m_tests)
target_link_libraries(acs_foundation_optimization_wave_m_tests PRIVATE
    ACS::Foundation
    ACS::Threading
    ACS::Memory
    ACS::Container
    ACS::Platform
    ACS::Event
    ACS::AssetPack
    ACS::Test
)
add_test(
    NAME ACS.FoundationOptimizationWaveM
    COMMAND acs_foundation_optimization_wave_m_tests)
set_tests_properties(
    ACS.FoundationOptimizationWaveM
    PROPERTIES LABELS "Foundation;Stress" TIMEOUT 120)

# CFileSystemの入力境界、既存root、通常ファイル衝突を単独検証する。
add_executable(acs_file_system_safety_tests
    main.cpp
    file_system_safety_tests.cpp
)
set_target_properties(acs_file_system_safety_tests PROPERTIES FOLDER "Engine/Tests")
acs_apply_compiler_options(acs_file_system_safety_tests)
target_link_libraries(acs_file_system_safety_tests PRIVATE
    ACS::Platform
    ACS::Test
)
add_test(NAME ACS.FileSystemSafety COMMAND acs_file_system_safety_tests)
set_tests_properties(ACS.FileSystemSafety PROPERTIES TIMEOUT 30)

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

# 公開音量経路の正規化とbackend失敗時の非変更契約を実音声機器なしで検証する。
add_executable(acs_audio_volume_safety_tests audio_volume_safety_tests.cpp)
set_target_properties(acs_audio_volume_safety_tests PROPERTIES FOLDER "Engine/Tests")
acs_apply_compiler_options(acs_audio_volume_safety_tests)
target_compile_definitions(acs_audio_volume_safety_tests PRIVATE ACS_AUDIO_TEST_HOOKS=1)
target_link_libraries(acs_audio_volume_safety_tests PRIVATE
    ACS::Foundation
    ACS::Audio
)
add_test(NAME ACS.AudioVolumeSafety COMMAND acs_audio_volume_safety_tests)
set_tests_properties(ACS.AudioVolumeSafety PROPERTIES TIMEOUT 30)

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

# WinSock の入力境界と空データグラム契約を描画モジュールから独立して検証する。
add_executable(acs_network_socket_io_tests
    main.cpp
    network_socket_io_tests.cpp
)
set_target_properties(acs_network_socket_io_tests PROPERTIES FOLDER "Engine/Tests")
acs_apply_compiler_options(acs_network_socket_io_tests)
target_link_libraries(acs_network_socket_io_tests PRIVATE
    ACS::Foundation
    ACS::Network
    ACS::Test
)
add_test(NAME ACS.NetworkSocketIo COMMAND acs_network_socket_io_tests)
set_tests_properties(ACS.NetworkSocketIo PROPERTIES RUN_SERIAL TRUE TIMEOUT 30)

# GameFramework の失敗注入は unit test を構成した build にだけ組み込む。
target_compile_definitions(acs_gameframework PRIVATE ACS_GAMEFRAMEWORK_TEST_HOOKS=1)
target_compile_definitions(acs_unit_tests PRIVATE ACS_GAMEFRAMEWORK_TEST_HOOKS=1)

# UI tests は Widget tree のレイアウト計算 / 入力ハンドリング部分を検証する。
# UiRenderer.cpp は Render に依存するため、Ui を link すると Render / Platform /
# Asset も transitive で必要。Render 経由で GPU 系のコードが入るが、tests は
# 描画は行わないので Renderer を実体化しなければ実行時影響無し。
# uilayer_tests は CUiLayer (gameframework) を検証するため GameFramework を link する
# (HandleInput に合成 Event を流す純 logic テストで、GPU/Scene は実体化しない)。
target_link_libraries(acs_unit_tests PRIVATE
    ACS::Foundation
    ACS::Threading
    ACS::Memory
    ACS::Container
    ACS::Math
    ACS::Timing
    ACS::Ecs
    ACS::Event
    ACS::Mvvm
    ACS::Ui
    ACS::Render
    ACS::GameFramework
    ACS::Easy
    ACS::Network
    ACS::Collision
    ACS::AssetPack
    ACS::App
    ACS::Test
)

add_executable(acs_fixed_step_clock_tests
    main.cpp
    fixed_step_clock_tests.cpp
)
set_target_properties(acs_fixed_step_clock_tests PROPERTIES FOLDER "Engine/Tests")
acs_apply_compiler_options(acs_fixed_step_clock_tests)
target_link_libraries(acs_fixed_step_clock_tests PRIVATE
    ACS::Foundation
    ACS::Memory
    ACS::Test
    ACS::Timing
)
add_test(NAME ACS.FixedStepClock COMMAND acs_fixed_step_clock_tests)
set_tests_properties(ACS.FixedStepClock PROPERTIES LABELS "Foundation;Timing" TIMEOUT 30)

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
    target_sources(acs_unit_tests PRIVATE
        imgui_lifetime_tests.cpp
        fxedit_serializer_safety_tests.cpp
        editor_core_persistence_safety_tests.cpp
    )
    target_link_libraries(acs_unit_tests PRIVATE ACS::Imgui)
endif()

if(TARGET ACS::MlOnnx)
    target_sources(acs_unit_tests PRIVATE mlonnx_failure_tests.cpp)
    target_link_libraries(acs_unit_tests PRIVATE ACS::MlOnnx)
    acs_onnxruntime_runtime(acs_unit_tests)
endif()

if(TARGET ACS::Scripting)
    # Lua関数登録の確保・公開失敗、再入拒否、退避関数を大規模テスト群から分離して検証する。
    add_executable(acs_lua_vm_allocation_safety_tests
        main.cpp
        lua_vm_allocation_safety_tests.cpp
        "${ACS_SOURCE_ROOT}/scripting/LuaDefault.cpp"
        "${ACS_SOURCE_ROOT}/scripting/LuaVm.cpp"
        "${ACS_SOURCE_ROOT}/scripting/LuaVmImpl.cpp"
    )
    set_target_properties(acs_lua_vm_allocation_safety_tests PROPERTIES FOLDER "Engine/Tests")
    acs_apply_compiler_options(acs_lua_vm_allocation_safety_tests)
    target_compile_definitions(acs_lua_vm_allocation_safety_tests PRIVATE
        ACS_SCRIPTING_TEST_HOOKS=1
        WITH_ACS_SCRIPTING=1
    )
    target_link_libraries(acs_lua_vm_allocation_safety_tests PRIVATE
        ACS::Container
        ACS::Foundation
        ACS::GameFramework
        ACS::Memory
        ACS::Test
        acs_third_party::lua
    )
    add_test(NAME ACS.LuaVmAllocationSafety COMMAND acs_lua_vm_allocation_safety_tests)
    set_tests_properties(ACS.LuaVmAllocationSafety PROPERTIES TIMEOUT 30)
endif()

add_test(NAME ACS.UnitTests COMMAND acs_unit_tests)
set_tests_properties(ACS.UnitTests PROPERTIES TIMEOUT 120)

# 4byte のシーンハンドルと 8byte の event ハンドルを別契約として独立検証する。
add_executable(acs_scene_timer_handle_tests
    main.cpp
    scene_timer_handle_tests.cpp
)
set_target_properties(
    acs_scene_timer_handle_tests
    PROPERTIES FOLDER "Engine/Tests")
acs_apply_compiler_options(acs_scene_timer_handle_tests)
target_link_libraries(acs_scene_timer_handle_tests PRIVATE
    ACS::Foundation
    ACS::Memory
    ACS::Container
    ACS::Event
    ACS::GameFramework
    ACS::Test
)
add_test(
    NAME ACS.SceneTimerHandle
    COMMAND acs_scene_timer_handle_tests)
set_tests_properties(
    ACS.SceneTimerHandle
    PROPERTIES LABELS "GameFramework;ABI" TIMEOUT 30)

# FoundationOptimizationWaveA 用 Release マイクロベンチマーク。実時間は
# 診断用の参考値なので、意図的に CTest へは登録しない。
add_executable(acs_foundation_optimization_bench
    foundation_optimization_bench.cpp
)
set_target_properties(
    acs_foundation_optimization_bench
    PROPERTIES FOLDER "Engine/Benchmarks")
acs_apply_compiler_options(acs_foundation_optimization_bench)
target_link_libraries(acs_foundation_optimization_bench PRIVATE
    ACS::Foundation
    ACS::Threading
    ACS::Memory
    ACS::Container
    ACS::Math
)

# Render Graph の alias 寿命解析を GPU 初期化なしで高速に反復する。
add_executable(acs_render_graph_transient_alias_tests
    main.cpp
    render_graph_transient_alias_tests.cpp
)
set_target_properties(acs_render_graph_transient_alias_tests PROPERTIES FOLDER "Engine/Tests")
acs_apply_compiler_options(acs_render_graph_transient_alias_tests)
target_link_libraries(acs_render_graph_transient_alias_tests PRIVATE
    ACS::Foundation
    ACS::Memory
    ACS::Container
    ACS::Render
    ACS::Test
)
add_test(NAME ACS.RenderGraphTransientAlias COMMAND acs_render_graph_transient_alias_tests)
set_tests_properties(ACS.RenderGraphTransientAlias PROPERTIES TIMEOUT 30)

# 描画packet sortの決定的な作業量削減と補助時間を分離計測する。
add_executable(acs_render_packet_performance
    render_packet_performance_main.cpp
)
set_target_properties(acs_render_packet_performance PROPERTIES FOLDER "Engine/Benchmarks")
acs_apply_compiler_options(acs_render_packet_performance)
target_link_libraries(acs_render_packet_performance PRIVATE
    ACS::Foundation
    ACS::Memory
    ACS::Container
    ACS::Math
    ACS::Render
)
add_test(NAME ACS.RenderPacketPerformance COMMAND acs_render_packet_performance)
set_tests_properties(ACS.RenderPacketPerformance PROPERTIES LABELS "Performance;Render" TIMEOUT 30)

# FoundationOptimizationWaveI 用 Release 診断 benchmark。時間値は CTest 合否に使わない。
add_executable(acs_foundation_optimization_wave_i_bench
    foundation_optimization_wave_i_bench.cpp
)
set_target_properties(
    acs_foundation_optimization_wave_i_bench
    PROPERTIES FOLDER "Engine/Benchmarks")
acs_apply_compiler_options(acs_foundation_optimization_wave_i_bench)
target_link_libraries(acs_foundation_optimization_wave_i_bench PRIVATE
    ACS::Foundation
    ACS::Threading
    ACS::Memory
    ACS::Platform
)

# Debug CRT 専用 probe は multi-config でも target を生成し、実行構成だけ Debug に限定する。
if(MSVC
   AND NOT ACS_ENABLE_ADDRESS_SANITIZER
   AND (CMAKE_CONFIGURATION_TYPES OR CMAKE_BUILD_TYPE STREQUAL "Debug"))
    add_executable(acs_crt_debug_heap_process_tests
        crt_debug_heap_process_tests.cpp
    )
    set_target_properties(acs_crt_debug_heap_process_tests PROPERTIES FOLDER "Engine/Tests")
    acs_apply_compiler_options(acs_crt_debug_heap_process_tests)
    target_link_libraries(acs_crt_debug_heap_process_tests PRIVATE ACS::Memory)

    add_test(NAME ACS.CrtDumpMemoryLeaksClean
             COMMAND acs_crt_debug_heap_process_tests --clean
             CONFIGURATIONS Debug)
    add_test(NAME ACS.CrtDumpMemoryLeaksPositive
             COMMAND acs_crt_debug_heap_process_tests --positive
             CONFIGURATIONS Debug)
    set_tests_properties(ACS.CrtDumpMemoryLeaksClean ACS.CrtDumpMemoryLeaksPositive
                         PROPERTIES TIMEOUT 30)
endif()

if(TARGET ACS::EditorAbi)
    add_executable(acs_editor_abi_lifecycle_tests editor_abi_lifecycle_tests.cpp)
    set_target_properties(acs_editor_abi_lifecycle_tests PROPERTIES FOLDER "Engine/Tests")
    acs_apply_compiler_options(acs_editor_abi_lifecycle_tests)
    # Raw DX12 uses a CPU-only startup worker. Diligent submits shader jobs on
    # the owner thread and polls its backend compiler pool. Both must expose a
    # pending startup boundary and support destruction while compilation runs.
    if(ACS_RENDER_DX12_RAW OR ACS_RENDER_DILIGENT)
        set(ACS_EDITOR_ABI_EXPECTS_ASYNC_WARMUP 1)
    else()
        set(ACS_EDITOR_ABI_EXPECTS_ASYNC_WARMUP 0)
    endif()
    target_compile_definitions(acs_editor_abi_lifecycle_tests PRIVATE
        ACS_EDITOR_ABI_EXPECTS_ASYNC_WARMUP=${ACS_EDITOR_ABI_EXPECTS_ASYNC_WARMUP})
    target_include_directories(acs_editor_abi_lifecycle_tests PRIVATE
        "${ACS_SOURCE_ROOT}")
    target_link_libraries(acs_editor_abi_lifecycle_tests PRIVATE ACS::EditorAbi)
    add_test(NAME ACS.EditorAbiLifecycle COMMAND acs_editor_abi_lifecycle_tests)
endif()

# C++ の実宣言だけを字句解析し、型 prefix・列挙子とメンバー呼び出しの型名衝突・
# 削除済み Node API の再流入を防ぐ。
# コメント・文字列・raw string 内の HLSL は監査対象にしない。
set(ACS_CPP_CONVENTIONS_AUDIT
    "${ACS_TREE_ROOT}/scripts/audit_cpp_conventions.py")

add_custom_target(acs_conventions_check
    COMMAND "${Python3_EXECUTABLE}" "${ACS_CPP_CONVENTIONS_AUDIT}"
            --root "${ACS_TREE_ROOT}"
    WORKING_DIRECTORY "${ACS_TREE_ROOT}"
    COMMENT "ACS: C++ 型・列挙子・メソッド命名と Node 統一の再流入を監査"
    USES_TERMINAL VERBATIM)
set_target_properties(acs_conventions_check PROPERTIES FOLDER "Tools")

add_test(NAME ACS.CppConventionsAuditSelfTest
         COMMAND "${Python3_EXECUTABLE}" "${ACS_CPP_CONVENTIONS_AUDIT}" --self-test)
add_test(NAME ACS.CppConventionsAudit
         COMMAND "${Python3_EXECUTABLE}" "${ACS_CPP_CONVENTIONS_AUDIT}"
                 --root "${ACS_TREE_ROOT}")
set_tests_properties(
    ACS.CppConventionsAuditSelfTest
    ACS.CppConventionsAudit
    PROPERTIES LABELS "StaticAnalysis" TIMEOUT 60)

# C++型の意味とA/C/F/I/T/E接頭辞を照合し、未レビューdebtの増加を防ぐ。
set(ACS_CPP_TYPE_ROLES_AUDIT
    "${ACS_TREE_ROOT}/scripts/audit_cpp_type_roles.py")
set(ACS_CPP_TYPE_ROLES_DEBT
    "${ACS_TREE_ROOT}/scripts/data/cpp_type_role_migration_debt.json")
set(ACS_CPP_PREFIX_CONSUMERS_AUDIT
    "${ACS_TREE_ROOT}/scripts/audit_cpp_prefix_consumers.py")

add_custom_target(acs_type_roles_check
    COMMAND "${CMAKE_COMMAND}" -E env "PYTHONDONTWRITEBYTECODE=1" "${Python3_EXECUTABLE}" "${ACS_CPP_TYPE_ROLES_AUDIT}"
            --root "${ACS_SOURCE_ROOT}"
            --migration-debt "${ACS_CPP_TYPE_ROLES_DEBT}"
    COMMAND "${CMAKE_COMMAND}" -E env "PYTHONDONTWRITEBYTECODE=1" "${Python3_EXECUTABLE}" "${ACS_CPP_TYPE_ROLES_AUDIT}"
            --root "${ACS_SOURCE_ROOT}/event"
            --migration-debt "${ACS_CPP_TYPE_ROLES_DEBT}"
    COMMAND "${CMAKE_COMMAND}" -E env "PYTHONDONTWRITEBYTECODE=1" "${Python3_EXECUTABLE}" "${ACS_CPP_TYPE_ROLES_AUDIT}"
            --root "${ACS_SOURCE_ROOT}/ecs"
            --migration-debt "${ACS_CPP_TYPE_ROLES_DEBT}"
    COMMAND "${CMAKE_COMMAND}" -E env "PYTHONDONTWRITEBYTECODE=1" "${Python3_EXECUTABLE}" "${ACS_CPP_TYPE_ROLES_AUDIT}"
            --root "${ACS_SOURCE_ROOT}/scripting"
            --migration-debt "${ACS_CPP_TYPE_ROLES_DEBT}"
    COMMAND "${CMAKE_COMMAND}" -E env "PYTHONDONTWRITEBYTECODE=1" "${Python3_EXECUTABLE}" "${ACS_CPP_TYPE_ROLES_AUDIT}"
            --root "${ACS_TREE_ROOT}/tests"
    COMMAND "${CMAKE_COMMAND}" -E env "PYTHONDONTWRITEBYTECODE=1" "${Python3_EXECUTABLE}" "${ACS_CPP_TYPE_ROLES_AUDIT}"
            --root "${ACS_TREE_ROOT}/samples"
    COMMAND "${CMAKE_COMMAND}" -E env "PYTHONDONTWRITEBYTECODE=1" "${Python3_EXECUTABLE}" "${ACS_CPP_PREFIX_CONSUMERS_AUDIT}"
            --self-test
    COMMAND "${CMAKE_COMMAND}" -E env "PYTHONDONTWRITEBYTECODE=1" "${Python3_EXECUTABLE}" "${ACS_CPP_PREFIX_CONSUMERS_AUDIT}"
            --root "${ACS_TREE_ROOT}"
    WORKING_DIRECTORY "${ACS_TREE_ROOT}"
    COMMENT "ACS: source、tests、samples、配布consumerの型役割を監査"
    USES_TERMINAL VERBATIM)
set_target_properties(acs_type_roles_check PROPERTIES FOLDER "Tools")

add_test(NAME ACS.CppTypeRoleAuditSelfTest
         COMMAND "${CMAKE_COMMAND}" -E env "PYTHONDONTWRITEBYTECODE=1" "${Python3_EXECUTABLE}" "${ACS_CPP_TYPE_ROLES_AUDIT}" --self-test)
add_test(NAME ACS.CppTypeRoleAudit
         COMMAND "${CMAKE_COMMAND}" -E env "PYTHONDONTWRITEBYTECODE=1" "${Python3_EXECUTABLE}" "${ACS_CPP_TYPE_ROLES_AUDIT}"
                 --root "${ACS_SOURCE_ROOT}"
                 --migration-debt "${ACS_CPP_TYPE_ROLES_DEBT}")
add_test(NAME ACS.EventTypeRoleAudit
         COMMAND "${CMAKE_COMMAND}" -E env "PYTHONDONTWRITEBYTECODE=1" "${Python3_EXECUTABLE}" "${ACS_CPP_TYPE_ROLES_AUDIT}"
                 --root "${ACS_SOURCE_ROOT}/event"
                 --migration-debt "${ACS_CPP_TYPE_ROLES_DEBT}")
add_test(NAME ACS.EcsTypeRoleAudit
         COMMAND "${CMAKE_COMMAND}" -E env "PYTHONDONTWRITEBYTECODE=1" "${Python3_EXECUTABLE}" "${ACS_CPP_TYPE_ROLES_AUDIT}"
                 --root "${ACS_SOURCE_ROOT}/ecs"
                 --migration-debt "${ACS_CPP_TYPE_ROLES_DEBT}")
add_test(NAME ACS.ScriptingTypeRoleAudit
         COMMAND "${CMAKE_COMMAND}" -E env "PYTHONDONTWRITEBYTECODE=1" "${Python3_EXECUTABLE}" "${ACS_CPP_TYPE_ROLES_AUDIT}"
                 --root "${ACS_SOURCE_ROOT}/scripting"
                 --migration-debt "${ACS_CPP_TYPE_ROLES_DEBT}")
add_test(NAME ACS.TestTypeRoleAudit
         COMMAND "${CMAKE_COMMAND}" -E env "PYTHONDONTWRITEBYTECODE=1" "${Python3_EXECUTABLE}" "${ACS_CPP_TYPE_ROLES_AUDIT}"
                 --root "${ACS_TREE_ROOT}/tests")
add_test(NAME ACS.SampleTypeRoleAudit
         COMMAND "${CMAKE_COMMAND}" -E env "PYTHONDONTWRITEBYTECODE=1" "${Python3_EXECUTABLE}" "${ACS_CPP_TYPE_ROLES_AUDIT}"
                 --root "${ACS_TREE_ROOT}/samples")
set_tests_properties(
    ACS.CppTypeRoleAudit
    ACS.EventTypeRoleAudit
    ACS.EcsTypeRoleAudit
    ACS.ScriptingTypeRoleAudit
    ACS.TestTypeRoleAudit
    ACS.SampleTypeRoleAudit
    PROPERTIES LABELS "StaticAnalysis" TIMEOUT 60)
set_tests_properties(
    ACS.CppTypeRoleAuditSelfTest
    PROPERTIES LABELS "StaticAnalysis" TIMEOUT 120)

add_test(NAME ACS.CppPrefixConsumerAuditSelfTest
         COMMAND "${CMAKE_COMMAND}" -E env "PYTHONDONTWRITEBYTECODE=1"
                 "${Python3_EXECUTABLE}" "${ACS_CPP_PREFIX_CONSUMERS_AUDIT}"
                 --self-test)
add_test(NAME ACS.CppPrefixConsumerAudit
         COMMAND "${CMAKE_COMMAND}" -E env "PYTHONDONTWRITEBYTECODE=1"
                 "${Python3_EXECUTABLE}" "${ACS_CPP_PREFIX_CONSUMERS_AUDIT}"
                 --root "${ACS_TREE_ROOT}")
set_tests_properties(
    ACS.CppPrefixConsumerAuditSelfTest
    PROPERTIES LABELS "StaticAnalysis" TIMEOUT 120)
set_tests_properties(
    ACS.CppPrefixConsumerAudit
    PROPERTIES LABELS "StaticAnalysis" TIMEOUT 60)

# 手書き API リファレンスの class/struct/enum 表記を現行 C++ 宣言と照合し、
# 大規模 rename 後にサンプルだけ旧名へ戻る drift を防ぐ。
set(ACS_REFERENCE_TYPE_NAMES_AUDIT
    "${ACS_TREE_ROOT}/scripts/audit_reference_type_names.py")

add_custom_target(acs_reference_check
    COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
            "${Python3_EXECUTABLE}" "${ACS_REFERENCE_TYPE_NAMES_AUDIT}"
            --root "${ACS_TREE_ROOT}"
    WORKING_DIRECTORY "${ACS_TREE_ROOT}"
    COMMENT "ACS: 手書き API リファレンスの型名を現行宣言と照合"
    USES_TERMINAL VERBATIM)
set_target_properties(acs_reference_check PROPERTIES FOLDER "Tools")

add_test(NAME ACS.ReferenceTypeNamesAuditSelfTest
         COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
                 "${Python3_EXECUTABLE}" "${ACS_REFERENCE_TYPE_NAMES_AUDIT}"
                 --self-test)
add_test(NAME ACS.ReferenceTypeNamesAudit
         COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
                 "${Python3_EXECUTABLE}" "${ACS_REFERENCE_TYPE_NAMES_AUDIT}"
                 --root "${ACS_TREE_ROOT}")
set_tests_properties(
    ACS.ReferenceTypeNamesAuditSelfTest
    ACS.ReferenceTypeNamesAudit
    PROPERTIES LABELS "StaticAnalysis" TIMEOUT 60)

# assembled Module.cmake を含め、全 compile source の登録漏れと削除済み entry を監査する。
set(ACS_MODULE_SOURCES_AUDIT
    "${ACS_TREE_ROOT}/scripts/audit_module_sources.py")

add_custom_target(acs_module_sources_check
    COMMAND "${Python3_EXECUTABLE}" "${ACS_MODULE_SOURCES_AUDIT}"
            --root "${ACS_TREE_ROOT}"
    WORKING_DIRECTORY "${ACS_TREE_ROOT}"
    COMMENT "ACS: Module.cmake の compile source 登録を監査"
    USES_TERMINAL VERBATIM)
set_target_properties(acs_module_sources_check PROPERTIES FOLDER "Tools")

add_test(NAME ACS.ModuleSourcesAuditSelfTest
         COMMAND "${Python3_EXECUTABLE}" "${ACS_MODULE_SOURCES_AUDIT}"
                 --self-test)
add_test(NAME ACS.ModuleSourcesAudit
         COMMAND "${Python3_EXECUTABLE}" "${ACS_MODULE_SOURCES_AUDIT}"
                 --root "${ACS_TREE_ROOT}")
set_tests_properties(
    ACS.ModuleSourcesAuditSelfTest
    ACS.ModuleSourcesAudit
    PROPERTIES LABELS "StaticAnalysis" TIMEOUT 60)

# tracked artifact の履歴 anchor、移動先、byte identity、参照 0 を台帳と照合する。
set(ACS_ARTIFACT_LEDGER_AUDIT
    "${ACS_TREE_ROOT}/scripts/audit_artifact_ledger.py")

add_custom_target(acs_artifact_ledger_check
    COMMAND "${CMAKE_COMMAND}" -E env "PYTHONDONTWRITEBYTECODE=1"
            "${Python3_EXECUTABLE}" "${ACS_ARTIFACT_LEDGER_AUDIT}"
            --root "${ACS_TREE_ROOT}/.."
    WORKING_DIRECTORY "${ACS_TREE_ROOT}/.."
    COMMENT "ACS: tracked artifact 台帳の履歴・byte・参照を照合"
    USES_TERMINAL VERBATIM)
set_target_properties(acs_artifact_ledger_check PROPERTIES FOLDER "Tools")

add_test(NAME ACS.ArtifactLedgerAuditSelfTest
         COMMAND "${CMAKE_COMMAND}" -E env "PYTHONDONTWRITEBYTECODE=1"
                 "${Python3_EXECUTABLE}" "${ACS_ARTIFACT_LEDGER_AUDIT}"
                 --self-test)
add_test(NAME ACS.ArtifactLedgerAudit
         COMMAND "${CMAKE_COMMAND}" -E env "PYTHONDWRITEBYTECODE=1"
                 "${Python3_EXECUTABLE}" "${ACS_ARTIFACT_LEDGER_AUDIT}"
                 --root "${ACS_TREE_ROOT}/..")
set_tests_properties(
    ACS.ArtifactLedgerAuditSelfTest
    ACS.ArtifactLedgerAudit
    PROPERTIES LABELS "StaticAnalysis;BuildSystem" TIMEOUT 60)

# C++ の旧 Kit 境界と独立 Kit ディレクトリが再流入しないことを確認する。
set(ACS_KIT_ABSORPTION_AUDIT "${ACS_TREE_ROOT}/scripts/audit_kit_absorption.py")
add_custom_target(acs_kit_absorption_check
    COMMAND "${CMAKE_COMMAND}" -E env "PYTHONDONTWRITEBYTECODE=1" "PYTHONUTF8=1" "PYTHONIOENCODING=utf-8" "${Python3_EXECUTABLE}" "${ACS_KIT_ABSORPTION_AUDIT}" --root "${ACS_TREE_ROOT}" --json-output "${CMAKE_BINARY_DIR}/kit-absorption-audit.json"
    WORKING_DIRECTORY "${ACS_TREE_ROOT}"
    COMMENT "ACS: Kit の C++ 境界と独立ディレクトリの再流入を監査"
    USES_TERMINAL VERBATIM)
set_target_properties(acs_kit_absorption_check PROPERTIES FOLDER "Tools")
add_test(NAME ACS.KitAbsorptionAuditSelfTest COMMAND "${CMAKE_COMMAND}" -E env "PYTHONDONTWRITEBYTECODE=1" "PYTHONUTF8=1" "PYTHONIOENCODING=utf-8" "${Python3_EXECUTABLE}" "${ACS_KIT_ABSORPTION_AUDIT}" --self-test)
add_test(NAME ACS.KitAbsorptionAudit COMMAND "${CMAKE_COMMAND}" -E env "PYTHONDONTWRITEBYTECODE=1" "PYTHONUTF8=1" "PYTHONIOENCODING=utf-8" "${Python3_EXECUTABLE}" "${ACS_KIT_ABSORPTION_AUDIT}" --root "${ACS_TREE_ROOT}" --json-output "${CMAKE_BINARY_DIR}/kit-absorption-audit.json")
set_tests_properties(ACS.KitAbsorptionAuditSelfTest ACS.KitAbsorptionAudit PROPERTIES LABELS "StaticAnalysis;KitAbsorption" TIMEOUT 60)

# 外部プロジェクトと ACS source が別 drive にある構成でも IDE group を安全に分ける。
add_test(
    NAME ACS.CompilerOptionsPathClassification
    COMMAND "${CMAKE_COMMAND}"
            "-DACS_COMPILER_OPTIONS_FILE=${ACS_TREE_ROOT}/engine/cmake/ACSCompilerOptions.cmake"
            -P "${ACS_TREE_ROOT}/tests/compiler_options_path_classification.cmake")
set_tests_properties(
    ACS.CompilerOptionsPathClassification
    PROPERTIES LABELS "StaticAnalysis;BuildSystem" TIMEOUT 30)

# CPack は宣言したゲームruntimeだけを収録し、private依存のinstall規則を隔離する。
add_test(
    NAME ACS.PackageConfigurationContract
    COMMAND "${CMAKE_COMMAND}"
            "-DACS_CPACK_CONFIG=${CMAKE_BINARY_DIR}/CPackConfig.cmake"
            "-DACS_INSTALL_SCRIPT=${CMAKE_BINARY_DIR}/cmake_install.cmake"
            "-DACS_EXPECT_IMGUI_LICENSE=${ACS_RENDER_DX12_RAW}"
            "-DACS_EXPECT_DILIGENT_LICENSES=${ACS_RENDER_DILIGENT}"
            -P "${ACS_TREE_ROOT}/tests/package_configuration_contract.cmake")
set_tests_properties(
    ACS.PackageConfigurationContract
    PROPERTIES LABELS "StaticAnalysis;BuildSystem;Distribution" TIMEOUT 30)

# tracked dist/ は repository 最上位から開発するときだけ存在する配布成果物。
# ACS を add_subdirectory した利用者や source-only 配布では sibling dist を参照せず、
# 通常の configure/build/test を壊さない。
if(ACS_IS_TOP_LEVEL)
    get_filename_component(ACS_DIST_ROOT
        "${ACS_TREE_ROOT}/../dist" ABSOLUTE)
    set(ACS_AMALGAMATE_SCRIPT
        "${ACS_TREE_ROOT}/scripts/amalgamate.py")

    add_test(NAME ACS.AmalgamationAliasSelfTest
             COMMAND "${Python3_EXECUTABLE}" "${ACS_AMALGAMATE_SCRIPT}"
                     --self-test)
    set_tests_properties(
        ACS.AmalgamationAliasSelfTest
        PROPERTIES LABELS "StaticAnalysis;Distribution" TIMEOUT 30)

    if(WIN32)
        find_program(ACS_POWERSHELL_EXECUTABLE NAMES pwsh powershell)
        if(ACS_POWERSHELL_EXECUTABLE)
            add_test(
                NAME ACS.SingleHeaderPipelineSelfTest
                COMMAND "${ACS_POWERSHELL_EXECUTABLE}"
                        -NoProfile -ExecutionPolicy Bypass
                        -File "${ACS_TREE_ROOT}/scripts/build_single_header.ps1"
                        -SelfTest)
            set_tests_properties(
                ACS.SingleHeaderPipelineSelfTest
                PROPERTIES
                    LABELS "StaticAnalysis;Distribution"
                    TIMEOUT 60
                    RUN_SERIAL TRUE)
        endif()
    endif()

    if(EXISTS "${ACS_DIST_ROOT}/acs.h"
       AND EXISTS "${ACS_DIST_ROOT}/examples/check.cpp")
        add_custom_target(acs_dist_check
            COMMAND "${Python3_EXECUTABLE}" "${ACS_AMALGAMATE_SCRIPT}" --check
            COMMAND "${Python3_EXECUTABLE}" "${ACS_CPP_CONVENTIONS_AUDIT}"
                    --root "${ACS_DIST_ROOT}" --scope "."
            WORKING_DIRECTORY "${ACS_TREE_ROOT}/.."
            COMMENT "ACS: tracked 単一ヘッダの生成 drift と命名規約を検証"
            USES_TERMINAL VERBATIM)
        set_target_properties(acs_dist_check PROPERTIES FOLDER "Tools")

        add_test(NAME ACS.AmalgamationDrift
                 COMMAND "${Python3_EXECUTABLE}" "${ACS_AMALGAMATE_SCRIPT}"
                         --check)
        add_test(NAME ACS.DistributionConventions
                 COMMAND "${Python3_EXECUTABLE}" "${ACS_CPP_CONVENTIONS_AUDIT}"
                         --root "${ACS_DIST_ROOT}" --scope ".")
        set_tests_properties(
            ACS.AmalgamationDrift
            ACS.DistributionConventions
            PROPERTIES LABELS "StaticAnalysis;Distribution" TIMEOUT 60)

        add_test(
            NAME ACS.DistributionConsumerSmokeSelfTest
            COMMAND "${Python3_EXECUTABLE}"
                    "${ACS_TREE_ROOT}/scripts/run_distribution_consumer_smoke.py"
                    --self-test)
        set_tests_properties(
            ACS.DistributionConsumerSmokeSelfTest
            PROPERTIES LABELS "StaticAnalysis;Distribution" TIMEOUT 60)

        if(ACS_ENABLE_DISTRIBUTION_CONSUMER_SMOKE)
            if(ACS_DISTRIBUTION_CONSUMER_ROOT)
                get_filename_component(_acs_distribution_consumer_root
                    "${ACS_DISTRIBUTION_CONSUMER_ROOT}" ABSOLUTE)
            else()
                set(_acs_distribution_consumer_root "${ACS_DIST_ROOT}")
            endif()
            set(_acs_distribution_consumer_smoke_arguments
                --distribution-root "${_acs_distribution_consumer_root}"
                --cmake "${CMAKE_COMMAND}"
                --generator "${CMAKE_GENERATOR}"
                --timeout-seconds 300)
            if(CMAKE_GENERATOR_PLATFORM)
                list(APPEND _acs_distribution_consumer_smoke_arguments
                    --generator-platform "${CMAKE_GENERATOR_PLATFORM}")
            endif()
            if(CMAKE_GENERATOR_TOOLSET)
                list(APPEND _acs_distribution_consumer_smoke_arguments
                    --generator-toolset "${CMAKE_GENERATOR_TOOLSET}")
            endif()

            foreach(_acs_distribution_configuration Debug Release)
                add_test(
                    NAME "ACS.DistributionConsumerSmoke${_acs_distribution_configuration}"
                    COMMAND "${Python3_EXECUTABLE}"
                            "${ACS_TREE_ROOT}/scripts/run_distribution_consumer_smoke.py"
                            ${_acs_distribution_consumer_smoke_arguments}
                            --configuration "${_acs_distribution_configuration}")
                set_tests_properties(
                    "ACS.DistributionConsumerSmoke${_acs_distribution_configuration}"
                    PROPERTIES
                        LABELS "StaticAnalysis;Distribution"
                        TIMEOUT 420
                        RUN_SERIAL TRUE)
            endforeach()
            unset(_acs_distribution_consumer_smoke_arguments)
            unset(_acs_distribution_configuration)
            unset(_acs_distribution_consumer_root)
        endif()

        if(MSVC)
            # /Zs は object/link 成果物を作らず、配布側 include path だけで canonical
            # consumer を構文検証する。custom target 経由にして MSBuild の VC/SDK
            # environment を利用し、CTest からはこの明示 target だけを直列 build する。
            add_custom_target(acs_dist_header_syntax
                COMMAND "${CMAKE_CXX_COMPILER}"
                        /nologo /Zs /std:c++20 /utf-8
                        /permissive- /Zc:__cplusplus /Zc:preprocessor
                        /EHsc /GR- /D_HAS_EXCEPTIONS=1
                        "/I${ACS_DIST_ROOT}"
                        "${ACS_DIST_ROOT}/examples/check.cpp"
                WORKING_DIRECTORY "${ACS_DIST_ROOT}"
                COMMENT "ACS: dist/examples/check.cpp を /Zs で単一ヘッダ構文検証"
                USES_TERMINAL VERBATIM)
            set_target_properties(
                acs_dist_header_syntax PROPERTIES FOLDER "Tools")

            if(CMAKE_CONFIGURATION_TYPES)
                add_test(NAME ACS.DistributionHeaderSyntax
                         COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
                                 --config "$<CONFIG>"
                                 --target acs_dist_header_syntax
                                 --parallel 1)
            else()
                add_test(NAME ACS.DistributionHeaderSyntax
                         COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
                                 --target acs_dist_header_syntax
                                 --parallel 1)
            endif()
            set_tests_properties(
                ACS.DistributionHeaderSyntax
                PROPERTIES
                    LABELS "StaticAnalysis;Distribution"
                    TIMEOUT 120
                    RUN_SERIAL TRUE)
        endif()
    endif()
endif()

# HID の解析関数は無名 namespace に閉じているので、実装ごと取り込む専用の実行ファイルで回す
# (通常 unit 群へ混ぜると、engine 側の実体と二重定義になる)。
if(WIN32)
    add_executable(acs_hid_gamepad_report_tests
        main.cpp
        hid_gamepad_report_tests.cpp
    )
    set_target_properties(acs_hid_gamepad_report_tests PROPERTIES FOLDER "Engine/Tests")
    acs_apply_compiler_options(acs_hid_gamepad_report_tests)
    target_link_libraries(acs_hid_gamepad_report_tests PRIVATE
        ACS::Foundation
        ACS::Test
        setupapi
        hid
    )
    add_test(NAME ACS.HidGamepadReport COMMAND acs_hid_gamepad_report_tests)
    set_tests_properties(ACS.HidGamepadReport PROPERTIES LABELS "Platform;Input" TIMEOUT 60)
endif()
