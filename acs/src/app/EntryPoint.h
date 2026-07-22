// SPDX-License-Identifier: Apache-2.0
// 共通エントリポイントマクロ
//
// 使い方:
//   class FMyGame : public acs::FApplication { ... };
//   ACS_DEFINE_MAIN(FMyGame)
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

    /**
     * AppClass を既定 FAppConfig で起動するエントリポイントを自動生成する。
     *
     * @details
     * AppClass をスタックに構築し、`FAppConfig{}` を渡して Run() を呼ぶ実装関数を作り、
     * main / WinMain / wWinMain の 3 エントリをすべて出してそこへ委譲する
     * (コンソール・Win32 両サブシステム対応)。
     * @param AppClass FApplication を継承したアプリ型。
     */
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

    /**
     * cfg_factory が返す FAppConfig で AppClass を起動するエントリポイントを生成する。
     *
     * @details
     * ACS_DEFINE_MAIN と同じだが、既定設定の代わりに cfg_factory() の戻り値を Run() に渡す。
     * @param AppClass FApplication を継承したアプリ型。
     * @param cfg_factory FAppConfig を返す呼び出し可能オブジェクト (関数等)。
     */
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
    /**
     * AppClass を既定 FAppConfig で起動する main() を生成する (非 Windows)。
     *
     * @param AppClass FApplication を継承したアプリ型。
     */
    #define ACS_DEFINE_MAIN(AppClass)                                          \
        int main() {                                                           \
            AppClass app;                                                      \
            ::acs::FAppConfig cfg{};                                            \
            return app.Run(cfg);                                               \
        }

    /**
     * cfg_factory が返す FAppConfig で AppClass を起動する main() を生成する (非 Windows)。
     *
     * @param AppClass FApplication を継承したアプリ型。
     * @param cfg_factory FAppConfig を返す呼び出し可能オブジェクト (関数等)。
     */
    #define ACS_DEFINE_MAIN_WITH_CONFIG(AppClass, cfg_factory)                 \
        int main() {                                                           \
            AppClass app;                                                      \
            ::acs::FAppConfig cfg = cfg_factory();                              \
            return app.Run(cfg);                                               \
        }
#endif
