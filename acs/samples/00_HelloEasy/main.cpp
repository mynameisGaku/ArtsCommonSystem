// SPDX-License-Identifier: Apache-2.0
// ACS サンプル 00 — HelloEasy エントリポイント。
//
// acs::easy レイヤは（WinMain ではなく）ふつうの int main() で書くため、
// CMake 側は WIN32 を付けずコンソール subsystem でビルドする。
// 学習中はログがコンソールに出るのでむしろ都合がよい。
#include "HelloEasyApp.h"

int main() {
    return hello00::CHelloEasyApp::Run();
}
