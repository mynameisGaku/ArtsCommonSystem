// SPDX-License-Identifier: Apache-2.0
// HelloLocalization — 埋め込み言語データの本体。
#include "Types.h"

namespace helloloc {

const char* const kLangJa = R"(# 日本語
title=ACS フレームワーク
greeting=ようこそ、ACS ゲーム基盤へ。
menu.start=ゲーム開始
menu.options=設定
menu.exit=終了
hint=F1: 日本語  F2: 英語  F3: フランス語  Esc: 終了
note=言語切替時、未訳キーは fallback (英語) で表示されます。
)";

const char* const kLangEn = R"(# English
title=ACS Framework
greeting=Welcome to the ACS game foundation.
menu.start=Start FGame
menu.options=Options
menu.exit=Quit
hint=F1: Japanese  F2: English  F3: French  Esc: Quit
note=When a key is missing, it falls back to English.
)";

const char* const kLangFr = R"(# Français — partial (test missing-key fallback)
title=Cadre ACS
greeting=Bienvenue dans la fondation de jeu ACS.
menu.start=Commencer
menu.exit=Quitter
hint=F1: Japonais  F2: Anglais  F3: Français  Esc: Quitter
# menu.options と note を意図的に未訳にして fallback テスト
)";

} // namespace helloloc
