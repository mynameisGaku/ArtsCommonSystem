// SPDX-License-Identifier: Apache-2.0
// 共通エントリポイントマクロ
//
// 使い方:
//   class MyGame : public acs::FApplication { ... };
//   ACS_DEFINE_MAIN(MyGame)
//
// 効果: int main() を自動生成する。FAppConfig はデフォルト値を使用。
//       細かく制御したい場合は ACS_DEFINE_MAIN を使わず main を自前で書く。
#pragma once

#include "app/Application.h"

// Win32 サブシステムでビルドされた exe (CMake の `add_executable(... WIN32 ...)`)
// では `WinMain` がエントリポイント。コンソール subsystem では `main`。
// 両方に対応するため両エントリを出して、内部で同じ関数に委譲する。
#if defined(_WIN32)
    #include "foundation/Platform.h"   // <windows.h> を取り込む

    #define ACS_DEFINE_MAIN(AppClass)                                          \
        static int acs_run_main_impl() {                                       \
            AppClass app;                                                      \
            ::acs::FAppConfig cfg{};                                            \
            return app.Run(cfg);                                               \
        }                                                                      \
        int main() { return acs_run_main_impl(); }                             \
        int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {                 \
            return acs_run_main_impl();                                        \
        }                                                                      \
        int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {               \
            return acs_run_main_impl();                                        \
        }

    #define ACS_DEFINE_MAIN_WITH_CONFIG(AppClass, cfg_factory)                 \
        static int acs_run_main_impl() {                                       \
            AppClass app;                                                      \
            ::acs::FAppConfig cfg = cfg_factory();                              \
            return app.Run(cfg);                                               \
        }                                                                      \
        int main() { return acs_run_main_impl(); }                             \
        int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {                 \
            return acs_run_main_impl();                                        \
        }                                                                      \
        int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {               \
            return acs_run_main_impl();                                        \
        }
#else
    #define ACS_DEFINE_MAIN(AppClass)                                          \
        int main() {                                                           \
            AppClass app;                                                      \
            ::acs::FAppConfig cfg{};                                            \
            return app.Run(cfg);                                               \
        }

    #define ACS_DEFINE_MAIN_WITH_CONFIG(AppClass, cfg_factory)                 \
        int main() {                                                           \
            AppClass app;                                                      \
            ::acs::FAppConfig cfg = cfg_factory();                              \
            return app.Run(cfg);                                               \
        }
#endif
