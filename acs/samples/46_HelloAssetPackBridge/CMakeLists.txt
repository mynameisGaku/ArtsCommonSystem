# HelloAssetPackBridge - real IAssetPackReader/Writer adapter smoke test.
add_executable(hello_asset_pack_bridge
    main.cpp
    AssetPackBridgeDemoApp.cpp
    AssetPackBridgeDemoApp.h
)
acs_apply_compiler_options(hello_asset_pack_bridge)
target_link_libraries(hello_asset_pack_bridge PRIVATE ACS::GameFramework ACS::AssetPack)
