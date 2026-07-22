// SPDX-License-Identifier: Apache-2.0
// HelloSave — FApplication 派生クラス。
//
// 動作:
//   ・起動時に %APPDATA%\acs_demo\hello_save.ini を読み込む
//   ・「起動回数」「累計クリック数」「ハイスコア」などをインクリメント
//   ・終了時 (OnShutdown) に自動保存
//   ・スペースで「クリック数」+1、R で全リセット、S で手動保存
//   ・Esc 終了
//
// 学習ポイント:
//   ・FStorage::GetAppDataPath で OS 標準のセーブ場所を解決
//   ・FStorage::Load / Save の典型的な使い方
//   ・型付き Get/Set (Int / Float / Bool / FString)
#pragma once

#include "app/Application.h"
#include "platform/Storage.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "foundation/Types.h"

namespace hellosave {

class FHelloSaveApp : public acs::FApplication {
public:
    void OnStart()    noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;

private:
    void FlushAndSave() noexcept;

    acs::FStorage     m_Store;
    acs::FSpriteBatch m_Batch;
    acs::FFont        m_FontBig;
    acs::FFont        m_FontSmall;

    wchar_t   m_SavePath[260] = {};
    acs::i64  m_Clicks = 0;
    acs::i64  m_HighScore = 0;
    bool      m_Dirty = false;
};

} // namespace hellosave
