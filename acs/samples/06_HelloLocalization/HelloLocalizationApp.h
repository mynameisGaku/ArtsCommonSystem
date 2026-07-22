// SPDX-License-Identifier: Apache-2.0
// HelloLocalization — FApplication 派生クラス。
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
#pragma once

#include "app/Application.h"
#include "platform/Localization.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "foundation/Types.h"

namespace helloloc {

class FHelloLocalizationApp : public acs::FApplication {
public:
    void OnStart()    noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;

private:
    void SwitchTo(acs::u32 idx) noexcept;

    acs::FLocalization m_Loc;
    acs::FSpriteBatch  m_Batch;
    acs::FFont         m_FontBig, m_FontSmall;
    acs::u32          m_Lang = 0;
};

} // namespace helloloc
