// SPDX-License-Identifier: Apache-2.0
// HelloAudio — エントリポイント。
//
// 構成:
//   HelloAudioApp.{h,cpp} - CApplication 派生クラス (CAudioEngine + AAudioAsset)
//
// 操作:
//   Space     : 再生 / 停止
//   ↑ / ↓    : 音量
//   Esc       : 終了
#include "HelloAudioApp.h"
#include "app/EntryPoint.h"

ACS_DEFINE_MAIN(helloaudio::CHelloAudioApp)
