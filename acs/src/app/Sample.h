// SPDX-License-Identifier: Apache-2.0
// acs::FSample — サンプル用の高レベル便利機能まとめ
//
// 目的:
//   ・ サンプルの定型コード (Init チェーンの IsErr+Quit 5 連や Win 限定フォントパス) を
//     一掃して、サンプルが「何を見せたいか」だけに集中できるようにする。
//
// 使用例 (HelloHelloMVVM 等):
//   class FMyApp : public CApplication {
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
#include "app/Forward.h"

namespace acs {

class FFont;
class IRhiDevice;

namespace FSample {

/**
 * プラットフォーム別の既定 UI フォントパスを 1 つ返す。
 *
 * @details 見つからない場合も最初の候補をそのまま返す (後段の LoadFromFile が IsErr を返す)。
 * @return OS ごとに優先される UI フォントのファイルパス。
 */
const wchar_t* DefaultUIFontPath() noexcept;

/**
 * 候補フォントを順に試し、最初に読み込めたものを font に展開する。
 *
 * @param font 読み込み先の FFont (成功時に内容が構築される)。
 * @param device アトラス生成に使う RHI デバイス。
 * @param size_px フォントのピクセルサイズ (既定 18.0)。
 * @param atlas_size グリフアトラスの一辺サイズ (既定 1024)。
 * @param include_cjk 日本語等を含めるか (true で大きな atlas を使う、既定 false)。
 * @return いずれか成功なら空の TResult、全候補が失敗ならエラー。
 */
TResult<void> TryLoadDefaultUIFont(FFont& font, IRhiDevice& device,
                                   f32  size_px     = 18.0f,
                                   u32  atlas_size  = 1024,
                                   bool include_cjk = false) noexcept;

} // namespace FSample
} // namespace acs

/**
 * expr を評価し、TResult が IsErr なら理由をログして Quit() + return する。
 *
 * @details
 * 「Init を呼んで失敗したら Quit して return」を 1 行で書くためのヘルパ。Quit() を
 * 参照するため CApplication のメンバ関数 (戻り値 void) の中からのみ使える。
 * @param expr TResult を返す初期化式。
 */
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

/**
 * expr を評価し、成功なら value を name に束縛、失敗なら Quit() + return する。
 *
 * @details
 * 行内の一意な一時名 (__LINE__ 連結) に受けてから Move して name に束縛する。
 * ACS_SAMPLE_INIT 同様 CApplication のメンバ関数からのみ使える。
 * @param name 取り出した値を束縛するローカル変数名。
 * @param expr TResult を返す式。
 */
#define ACS_SAMPLE_TAKE(name, expr)                                                \
    auto ACS_CONCAT(m_AcsTakeR, __LINE__) = (expr);                              \
    if (ACS_CONCAT(m_AcsTakeR, __LINE__).IsErr()) {                              \
        ACS_LOG_ERROR("FSample take failed at " #expr ": %s",                      \
                      ACS_CONCAT(m_AcsTakeR, __LINE__).Error().message);         \
        Quit();                                                                    \
        return;                                                                    \
    }                                                                              \
    auto name = ::acs::Move(ACS_CONCAT(m_AcsTakeR, __LINE__).Value())
