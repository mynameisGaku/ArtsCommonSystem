# ACS — Module enablement.
#
# Edit this file to choose which modules to build and which features to
# enable. The CMake build will validate dependencies (e.g. enabling
# Threading without Foundation will fail loudly) and emit corresponding
# WITH_ACS_<MODULE> / WITH_<FEATURE> preprocessor defines.
#
# Inspired by Unreal Engine's .Build.cs / .uplugin model.

# ---- Phase 1: Core runtime ------------------------------------------------
acs_enable_module(Foundation REQUIRED
    FEATURES STACKTRACE LOG_FILE_SINK LOG_DEBUG_OUTPUT
)

acs_enable_module(Threading REQUIRED
    FEATURES THREADPOOL
)

acs_enable_module(Memory REQUIRED
    FEATURES LINEAR_ALLOCATOR POOL_ALLOCATOR ARENA_ALLOCATOR
)

acs_enable_module(Container REQUIRED
    FEATURES HASHMAP STRING_SSO
)

acs_enable_module(Math REQUIRED
    FEATURES DIRECTXMATH AVX2 RUNTIME_DISPATCH
)

acs_enable_module(Test
    # Test framework — disable for shipping builds.
)

# ---- Phase 2 (planned, not yet implemented) -------------------------------
# acs_enable_module(Render
#     # Pick exactly one rendering backend:
#     FEATURES         DX12_RAW                     # raw DirectX 12
#     # FEATURES       THE_FORGE                    # ConfettiFX/The-Forge
#     # FEATURES       BGFX                         # bkaradzic/bgfx
# )
# acs_enable_module(Asset)
# acs_enable_module(ECS FEATURES ENTT_BACKEND)
# acs_enable_module(Editor FEATURES IMGUI)
