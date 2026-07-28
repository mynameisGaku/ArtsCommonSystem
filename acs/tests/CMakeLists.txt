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
    behavior_tree_persistence_safety_tests.cpp
    easing_completeness_tests.cpp
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
    ecs_tests.cpp
    event_tests.cpp
    jobgraph_tests.cpp
    mvvm_tests.cpp
    observable_array_lifetime_safety_tests.cpp
    ui_tests.cpp
    uilayer_tests.cpp
    json_tests.cpp
    collider_tests.cpp
    crypto_tests.cpp
    render_tests.cpp
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
    volumetric_cloud_world_anchor_tests.cpp
    atmosphere_composite_tests.cpp
    shadow_map_cb_safety_tests.cpp
    assetpack_lifetime_tests.cpp
    assetpack_manifest_safety_tests.cpp
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
    ACS::Easy
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

add_test(NAME ACS.UnitTests COMMAND acs_unit_tests)
set_tests_properties(ACS.UnitTests PROPERTIES TIMEOUT 120)

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
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
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

# 手書き API リファレンスの class/struct/enum 表記を現行 C++ 宣言と照合し、
# 大規模 rename 後にサンプルだけ旧名へ戻る drift を防ぐ。
set(ACS_REFERENCE_TYPE_NAMES_AUDIT
    "${ACS_TREE_ROOT}/scripts/audit_reference_type_names.py")

add_custom_target(acs_reference_check
    COMMAND "${Python3_EXECUTABLE}" "${ACS_REFERENCE_TYPE_NAMES_AUDIT}"
            --root "${ACS_TREE_ROOT}"
    WORKING_DIRECTORY "${ACS_TREE_ROOT}"
    COMMENT "ACS: 手書き API リファレンスの型名を現行宣言と照合"
    USES_TERMINAL VERBATIM)
set_target_properties(acs_reference_check PROPERTIES FOLDER "Tools")

add_test(NAME ACS.ReferenceTypeNamesAuditSelfTest
         COMMAND "${Python3_EXECUTABLE}" "${ACS_REFERENCE_TYPE_NAMES_AUDIT}"
                 --self-test)
add_test(NAME ACS.ReferenceTypeNamesAudit
         COMMAND "${Python3_EXECUTABLE}" "${ACS_REFERENCE_TYPE_NAMES_AUDIT}"
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

# tracked dist/ は repository 最上位から開発するときだけ存在する配布成果物。
# ACS を add_subdirectory した利用者や source-only 配布では sibling dist を参照せず、
# 通常の configure/build/test を壊さない。
if(ACS_IS_TOP_LEVEL)
    get_filename_component(ACS_DIST_ROOT
        "${ACS_TREE_ROOT}/../dist" ABSOLUTE)
    set(ACS_AMALGAMATE_SCRIPT
        "${ACS_TREE_ROOT}/scripts/amalgamate.py")

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
                    TIMEOUT 30
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

        if(MSVC)
            # /Zs は object/link 成果物を作らず、配布側 include path だけで canonical
            # consumer を構文検証する。custom target 経由にして MSBuild の VC/SDK
            # environment を利用し、CTest からはこの明示 target だけを直列 build する。
            add_custom_target(acs_dist_header_syntax
                COMMAND "${CMAKE_CXX_COMPILER}"
                        /nologo /Zs /std:c++20 /utf-8
                        /permissive- /Zc:__cplusplus /Zc:preprocessor
                        /EHs-c- /GR- /D_HAS_EXCEPTIONS=0
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
