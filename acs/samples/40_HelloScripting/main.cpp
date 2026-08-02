// SPDX-License-Identifier: Apache-2.0
// HelloScripting — エントリポイント。
//
// 構成:
//   ScriptDemoApp.{h,cpp} - Lua VM を Init し、4 つのデモを順に走らせる
//
// 動作:
//   ・ACS_BUILD_SCRIPTING=ON でビルドすると real Lua 5.4 backend (CLuaVm) を使う。
//   ・OFF だと acs::game::ScriptVmStub (no-op) に fall back する。
//   ・window / renderer は不要なので CApplication 派生ではなく自前 main。
#include "ScriptDemoApp.h"

int main() {
    helloscript::CScriptDemoApp app;
    return app.Run();
}
