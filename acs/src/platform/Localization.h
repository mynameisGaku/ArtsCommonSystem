// SPDX-License-Identifier: Apache-2.0
// 多言語対応（i18n）
//
// 設計:
//   - 各言語は INI 形式の FStorage（key=value、UTF-8）
//   - active = 現在表示中の言語、fallback = キーが無い時の代替
//   - FStorage を直接利用するので、ファイル形式 / 内部構造を学び直す必要なし
//
// 使い方:
//   FLocalization L;
//   L.LoadActive(L"lang/ja.lang");
//   L.LoadFallback(L"lang/en.lang");
//
//   FRenderer 内:
//     sb.DrawText(font, L.Tr("greeting"), 100, 100, color);
//
//   ja.lang 内容例:
//     # コメント
//     greeting=ハロー、世界！
//     menu.start=ゲーム開始
//     menu.exit=終了
//
//   en.lang 内容例:
//     greeting=Hello, FWorld!
//     menu.start=Start FGame
//     menu.exit=Quit
//
// 言語切替:
//   L.LoadActive(L"lang/de.lang");   // 別言語に切替（fallback はそのまま）
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "platform/Storage.h"

namespace acs {

class FLocalization {
public:
    FLocalization() noexcept = default;

    FLocalization(const FLocalization&)            = delete;
    FLocalization& operator=(const FLocalization&) = delete;

    // 現在表示中の言語をファイルから読み込む
    TResult<void> LoadActive  (const wchar_t* path) noexcept { return _active.Load(path); }
    TResult<void> LoadFallback(const wchar_t* path) noexcept { return _fallback.Load(path); }

    // バイト列から（埋め込み用）
    TResult<void> LoadActiveBytes  (const u8* data, usize size) noexcept { return _active.LoadFromBytes(data, size); }
    TResult<void> LoadFallbackBytes(const u8* data, usize size) noexcept { return _fallback.LoadFromBytes(data, size); }

    // active と fallback を入替え（言語切替の便利関数）
    void Swap() noexcept;

    void Clear() noexcept { _active.Clear(); _fallback.Clear(); }

    // 翻訳取得: active → fallback → key 自体（最後の手段）の順で探す
    const char* Tr(const char* key) const noexcept;

    // 存在チェック
    bool Has(const char* key) const noexcept;

    // 直接 FStorage アクセス（独自加工したいとき用）
    FStorage&       Active()         noexcept { return _active; }
    const FStorage& Active()   const noexcept { return _active; }
    FStorage&       Fallback()       noexcept { return _fallback; }
    const FStorage& Fallback() const noexcept { return _fallback; }

private:
    FStorage _active;
    FStorage _fallback;
};

} // namespace acs
