// 共通エントリポイントマクロ
//
// 使い方:
//   class MyGame : public acs::Application { ... };
//   ACS_DEFINE_MAIN(MyGame)
//
// 効果: int main() を自動生成する。AppConfig はデフォルト値を使用。
//       細かく制御したい場合は ACS_DEFINE_MAIN を使わず main を自前で書く。
#pragma once

#include "app/Application.h"

#define ACS_DEFINE_MAIN(AppClass)                                              \
    int main() {                                                               \
        AppClass app;                                                          \
        ::acs::AppConfig cfg{};                                                \
        return app.Run(cfg);                                                   \
    }

#define ACS_DEFINE_MAIN_WITH_CONFIG(AppClass, cfg_factory)                     \
    int main() {                                                               \
        AppClass app;                                                          \
        ::acs::AppConfig cfg = cfg_factory();                                  \
        return app.Run(cfg);                                                   \
    }
