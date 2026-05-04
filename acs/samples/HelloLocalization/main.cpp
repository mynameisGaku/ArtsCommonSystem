// ACS の HelloLocalization サンプル
//
// 動作:
//   ・実行ファイルに埋め込んだ ja.lang / en.lang / fr.lang から翻訳を取得
//   ・F1: 日本語、F2: 英語、F3: フランス語に切替
//   ・Esc 終了
//
// 学習ポイント:
//   ・Localization の active / fallback 二段構成
//   ・LoadFromBytes で実行ファイル組み込み
//   ・ファイルが存在しないキーは自動で fallback、それも無ければ key そのまま

#include "app/Application.h"
#include "app/EntryPoint.h"
#include "app/Sample.h"
#include "platform/Input.h"
#include "platform/Localization.h"

#include "render/SpriteBatch.h"
#include "render/Font.h"

#include "memory/UniquePtr.h"
#include "foundation/Log.h"

#include <cstring>
#include <cstdio>

using namespace acs;

namespace {

// 言語ファイルを実行ファイルに直書き（実プロジェクトでは別ファイルがおすすめ）
const char* kJa = R"(# 日本語
title=ACS フレームワーク
greeting=ようこそ、ACS ゲーム基盤へ。
menu.start=ゲーム開始
menu.options=設定
menu.exit=終了
hint=F1: 日本語  F2: 英語  F3: フランス語  Esc: 終了
note=言語切替時、未訳キーは fallback (英語) で表示されます。
)";

const char* kEn = R"(# English
title=ACS Framework
greeting=Welcome to the ACS game foundation.
menu.start=Start Game
menu.options=Options
menu.exit=Quit
hint=F1: Japanese  F2: English  F3: French  Esc: Quit
note=When a key is missing, it falls back to English.
)";

const char* kFr = R"(# Français — partial (test missing-key fallback)
title=Cadre ACS
greeting=Bienvenue dans la fondation de jeu ACS.
menu.start=Commencer
menu.exit=Quitter
hint=F1: Japonais  F2: Anglais  F3: Français  Esc: Quitter
# menu.options と note を意図的に未訳にして fallback テスト
)";

} // namespace

class HelloLocalization : public Application {
public:
    void OnStart() noexcept override {
        IRhiDevice* dev = GetRenderer().Device();
        if (!dev) { Quit(); return; }

        // Fallback は英語固定。Active だけ切替。
        if (auto r = _loc.LoadFallbackBytes(reinterpret_cast<const u8*>(kEn),
                                            std::strlen(kEn)); r.IsErr()) {
            Quit(); return;
        }
        SwitchTo(0);    // 起動時は日本語

        _batch.Init(*dev, GetRenderer().ColorFormat());
        (void)Sample::TryLoadDefaultUIFont(_font_big,   *dev, 36.0f, 1024, true);
        (void)Sample::TryLoadDefaultUIFont(_font_small, *dev, 20.0f, 1024, true);
    }

    void OnUpdate(f32 /*dt*/) noexcept override {
        if (Input::IsKeyPressed(Key::Escape)) Quit();
        if (Input::IsKeyPressed(Key::F1)) SwitchTo(0);
        if (Input::IsKeyPressed(Key::F2)) SwitchTo(1);
        if (Input::IsKeyPressed(Key::F3)) SwitchTo(2);
    }

    void OnRender() noexcept override {
        IRhiCommandList* cl = GetRenderer().CommandList();
        if (!cl) return;
        const u32 sw = GetRenderer().Swapchain()->Width();
        const u32 sh = GetRenderer().Swapchain()->Height();

        _batch.Begin(*cl, sw, sh);
        _batch.DrawRect(0, 0, static_cast<f32>(sw), static_cast<f32>(sh),
                        Vec4{0.10f, 0.13f, 0.20f, 1});
        _batch.DrawRect(40, 40, static_cast<f32>(sw - 80), static_cast<f32>(sh - 80),
                        Vec4{0.16f, 0.20f, 0.30f, 1});

        if (_font_big.AtlasTexture()) {
            _batch.DrawString(_font_big, _loc.Tr("title"),
                            80, 80, Vec4{1, 0.85f, 0.4f, 1});
        }
        if (_font_small.AtlasTexture()) {
            _batch.DrawString(_font_small, _loc.Tr("greeting"),
                            80, 150, Vec4{0.95f, 0.95f, 1, 1});

            const char* lang_name = (_lang == 0) ? "(日本語 / Japanese)"
                                  : (_lang == 1) ? "(English)"
                                  : "(Français)";
            _batch.DrawString(_font_small, lang_name,
                            80, 180, Vec4{0.6f, 0.7f, 0.85f, 1});

            _batch.DrawString(_font_small, _loc.Tr("menu.start"),   80, 240, Vec4{1,1,1,1});
            _batch.DrawString(_font_small, _loc.Tr("menu.options"), 80, 270, Vec4{1,1,1,1});
            _batch.DrawString(_font_small, _loc.Tr("menu.exit"),    80, 300, Vec4{1,1,1,1});

            _batch.DrawString(_font_small, _loc.Tr("note"),
                            80, static_cast<f32>(sh - 130), Vec4{0.7f, 0.7f, 0.8f, 1});
            _batch.DrawString(_font_small, _loc.Tr("hint"),
                            80, static_cast<f32>(sh - 90), Vec4{0.6f, 0.7f, 0.95f, 1});
        }
        _batch.End();
    }

    void OnShutdown() noexcept override {
        if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
        _font_small.Shutdown();
        _font_big.Shutdown();
        _batch.Shutdown();
    }

private:
    void SwitchTo(u32 idx) noexcept {
        const char* src = (idx == 0) ? kJa : (idx == 1) ? kEn : kFr;
        _loc.LoadActiveBytes(reinterpret_cast<const u8*>(src), std::strlen(src));
        _lang = idx;
    }

    Localization _loc;
    SpriteBatch  _batch;
    Font         _font_big, _font_small;
    u32          _lang = 0;
};

ACS_DEFINE_MAIN(HelloLocalization)
