# ACS — モジュール有効化設定
#
# このファイルを編集してビルドするモジュールと機能を選ぶ。
# CMake は依存関係を検証し、対応する WITH_ACS_<MODULE> / WITH_<FEATURE>
# プリプロセッサ define を自動的に付与する。

# ---- Phase 1: ランタイム基盤 ---------------------------------------------
acs_enable_module(Foundation REQUIRED
    FEATURES STACKTRACE LOG_FILE_SINK LOG_DEBUG_OUTPUT
)

acs_enable_module(Threading REQUIRED
    FEATURES THREADPOOL
)

acs_enable_module(Memory REQUIRED
    FEATURES LINEAR_ALLOCATOR POOL_ALLOCATOR ARENA_ALLOCATOR
             VIRTUAL_MEMORY TLSF SEGMENT_SYSTEM SNAPSHOT
)

acs_enable_module(Container REQUIRED
    FEATURES HASHMAP STRING_SSO
)

acs_enable_module(Math REQUIRED
    FEATURES DIRECTXMATH AVX2 RUNTIME_DISPATCH
)

acs_enable_module(Test
    # テストフレームワーク（ship build では無効化推奨）
)

# ---- Phase 2: プラットフォーム / ECS / アセット / レンダ / アプリ ---------
acs_enable_module(Platform
    FEATURES WINDOW INPUT
)

acs_enable_module(Ecs)

acs_enable_module(Asset)

acs_enable_module(Render
    FEATURES DX12_RAW
)

acs_enable_module(App)
