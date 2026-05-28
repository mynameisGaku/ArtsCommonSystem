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

acs_enable_module(Event)

acs_enable_module(Asset)

#
# Render バックエンド選択について
#
#   生 DX12 (デフォルト):       cmake configure で何も指定しない
#   Diligent Engine に切替:      -DACS_RENDER_DILIGENT=ON -DACS_RENDER_DX12_RAW=OFF
#   両方併存（過渡期用）:         -DACS_RENDER_DILIGENT=ON -DACS_RENDER_DX12_RAW=ON
#                                  実際にどちらが使われるかは CreateRhiDevice() の
#                                  実体（リンク時の選択）で決まる。
#
# Diligent ON のときは初回 cmake configure で DiligentCore/Tools/FX を git clone
# するため数分〜十数分かかる。CMake のキャッシュを保持していれば 2 回目以降は
# 高速。
#
acs_enable_module(Render
    FEATURES DX12_RAW
)

acs_enable_module(App)

# ---- Phase 4: 音声・ネットワーク ----
acs_enable_module(Audio
    FEATURES XAUDIO2
)

acs_enable_module(Network)

# ---- ImGui (任意): ImGui を使いたいときだけ有効化 ----
acs_enable_module(Imgui)

# Mvvm は ACS_MVVM_IMGUI_BINDINGS=ON のときだけ Imgui に依存する。
# 普通のビルドでは Imgui の後に Mvvm を入れておくと依存解決が素直。
acs_enable_module(Mvvm)

# Ui — ACS 純正 UI フレームワーク (Widget + Layout + Renderer)。Mvvm と組み合わせて
# 使うことで MVVM パターンを完結させる。
acs_enable_module(Ui)

# Easy — 初学者向けの「簡単モード」ファサード。Application / RHI を内部に隠し、
# 手続き的な関数だけで 2D ゲームを書ける（acs::easy）。
acs_enable_module(Easy)

# AssetPack — `.acpak` v1 アーカイブ (raw bytes + CRC32) の Reader/Writer を提供
# する独立エンジンモジュール。名前空間 acs::assetpack、CMake target ACS::AssetPack。
# GameFramework Pillar G (acs::game::IAssetPackReader) の concrete backend として
# も使える設計。詳細 docs/AssetPack.md
acs_enable_module(AssetPack)

# GameFramework — Application の上に Scene 切替・固定タイムステップ等を載せる
# フレームワーク。Phase 1 (Pillar A: App & Scene) は v3 確定設計の最小骨格のみ。
# 名前空間 acs::game、CMake target ACS::GameFramework。詳細 docs/GameFramework.md
acs_enable_module(GameFramework)

# Steamworks — Steamworks SDK の real backend 実装。ACS_BUILD_STEAMWORKS=ON
# のときだけビルドされる (Module.cmake が自分で OFF check)。OFF (default) の
# 場合は acs::game::SteamworksBridgeStub (no-op) を使う。
# 名前空間 acs::steamworks、CMake target ACS::Steamworks。
if(ACS_BUILD_STEAMWORKS)
    acs_enable_module(Steamworks)
endif()

# Scripting — Lua 5.4 scripting backend (IScriptVm の real 実装)。
# ACS_BUILD_SCRIPTING=ON のときだけビルド。OFF (default) なら
# acs::game::ScriptVmStub を使う。名前空間 acs::scripting、target ACS::Scripting。
if(ACS_BUILD_SCRIPTING)
    acs_enable_module(Scripting)
endif()

# MlOnnx - real ONNX Runtime CPU backend for IMlRuntime.
if(ACS_BUILD_ML_ONNX)
    acs_enable_module(MlOnnx)
endif()

# OpenXR - real Khronos OpenXR loader backend for IOpenXrBridge.
if(ACS_BUILD_OPENXR)
    acs_enable_module(OpenXr)
endif()

# CrashWin - real Windows DbgHelp minidump backend for ICrashReporterBackend.
if(ACS_BUILD_CRASH_REPORTER)
    acs_enable_module(CrashWin)
endif()

# TelemetryFile - real JSON Lines telemetry sink for IBackendClient.
if(ACS_BUILD_TELEMETRY_FILE)
    acs_enable_module(TelemetryFile)
endif()
