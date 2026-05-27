// SPDX-License-Identifier: Apache-2.0
// acs::FSample — サンプル用の高レベル便利機能まとめ
//
// 目的:
//   ・ サンプルの定型コード (Init チェーンの IsErr+Quit 5 連や Win 限定フォントパス) を
//     一掃して、サンプルが「何を見せたいか」だけに集中できるようにする。
//
// 使用例 (HelloHelloMVVM 等):
//   class MyApp : public FApplication {
//       void OnStart() noexcept override {
//           ACS_SAMPLE_INIT(m_Imgui.Init(GetWindow(), GetRenderer()));
//           ACS_SAMPLE_INIT(m_Shader.Init(*GetRenderer().Device(),
//                                        GetRenderer().ColorFormat(),
//                                        GetRenderer().DepthFormat()));
//           // 失敗した時点で自動的に Quit() + ログ出して return される
//       }
//       /* ... */
//   };
//
// 標準フォント解決:
//   const wchar_t* fp = acs::FSample::DefaultUIFontPath();   // OS 別の優先パス
//   m_Font.LoadFromFile(*dev, fp, 18.0f);
//
// 文字列ベースで複数候補を試すヘルパ:
//   if (acs::FSample::TryLoadDefaultUIFont(m_Font, *dev, 18.0f).IsErr()) {
//       // どれも見つからなかった
//   }
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "foundation/Log.h"

namespace acs {

class FApplication;
class Font;
class IRhiDevice;

namespace FSample {

// プラットフォーム別のデフォルト UI フォントを 1 つ返す
// (見つからなければ最初の候補を返す。後段の LoadFromFile が IsErr を返す)
const wchar_t* DefaultUIFontPath() noexcept;

// 候補フォントを順に試して最初に成功したものをロードする。
//   atlas_size  : Font のアトラスサイズ (LoadFromFile に渡す)
//   include_cjk : 日本語等を含めるか (true で大きな atlas を使う)
TResult<void> TryLoadDefaultUIFont(Font& font, IRhiDevice& device,
                                   f32  size_px     = 18.0f,
                                   u32  atlas_size  = 1024,
                                   bool include_cjk = false) noexcept;

} // namespace FSample
} // namespace acs

// ----------------------------------------------------------------------------
// マクロ: 「Init を呼んで失敗したら Quit して return」を 1 行で
// ----------------------------------------------------------------------------
//
// 使い方:
//   ACS_SAMPLE_INIT(m_RendererThing.Init(args));
//
// マクロ内では `auto m_R = (expr); if (m_R.IsErr()) { ログ + Quit + return; }` を展開。
// FApplication のメンバ関数からのみ使える (Quit() メソッドへのアクセスが要るため)。
#define ACS_SAMPLE_INIT(expr)                                                      \
    do {                                                                           \
        auto m_AcsSampleR = (expr);                                               \
        if (m_AcsSampleR.IsErr()) {                                               \
            ACS_LOG_ERROR("FSample init failed at " #expr ": %s",                  \
                          m_AcsSampleR.Error().message);                          \
            Quit();                                                                \
            return;                                                                \
        }                                                                          \
    } while (0)

// TResult<T> を受けて、成功なら value を name に束縛、失敗なら Quit + return。
// 使い方:
//   ACS_SAMPLE_TAKE(tex, CreateRhiTexture(*dev, td));
//   // 以降 tex を使える
#define ACS_SAMPLE_TAKE(name, expr)                                                \
    auto ACS_CONCAT(m_AcsTakeR, __LINE__) = (expr);                              \
    if (ACS_CONCAT(m_AcsTakeR, __LINE__).IsErr()) {                              \
        ACS_LOG_ERROR("FSample take failed at " #expr ": %s",                      \
                      ACS_CONCAT(m_AcsTakeR, __LINE__).Error().message);         \
        Quit();                                                                    \
        return;                                                                    \
    }                                                                              \
    auto name = ::acs::Move(ACS_CONCAT(m_AcsTakeR, __LINE__).Value())
