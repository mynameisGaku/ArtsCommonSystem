// 多言語対応（i18n）
//
// 設計:
//   - 各言語は INI 形式の Storage（key=value、UTF-8）
//   - active = 現在表示中の言語、fallback = キーが無い時の代替
//   - Storage を直接利用するので、ファイル形式 / 内部構造を学び直す必要なし
//
// 使い方:
//   Localization L;
//   L.LoadActive(L"lang/ja.lang");
//   L.LoadFallback(L"lang/en.lang");
//
//   Renderer 内:
//     sb.DrawText(font, L.Tr("greeting"), 100, 100, color);
//
//   ja.lang 内容例:
//     # コメント
//     greeting=ハロー、世界！
//     menu.start=ゲーム開始
//     menu.exit=終了
//
//   en.lang 内容例:
//     greeting=Hello, World!
//     menu.start=Start Game
//     menu.exit=Quit
//
// 言語切替:
//   L.LoadActive(L"lang/de.lang");   // 別言語に切替（fallback はそのまま）
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "platform/Storage.h"

namespace acs {

class Localization {
public:
    Localization() noexcept = default;

    Localization(const Localization&)            = delete;
    Localization& operator=(const Localization&) = delete;

    // 現在表示中の言語をファイルから読み込む
    Result<void> LoadActive  (const wchar_t* path) noexcept { return _active.Load(path); }
    Result<void> LoadFallback(const wchar_t* path) noexcept { return _fallback.Load(path); }

    // バイト列から（埋め込み用）
    Result<void> LoadActiveBytes  (const u8* data, usize size) noexcept { return _active.LoadFromBytes(data, size); }
    Result<void> LoadFallbackBytes(const u8* data, usize size) noexcept { return _fallback.LoadFromBytes(data, size); }

    // active と fallback を入替え（言語切替の便利関数）
    void Swap() noexcept;

    void Clear() noexcept { _active.Clear(); _fallback.Clear(); }

    // 翻訳取得: active → fallback → key 自体（最後の手段）の順で探す
    const char* Tr(const char* key) const noexcept;

    // 存在チェック
    bool Has(const char* key) const noexcept;

    // 直接 Storage アクセス（独自加工したいとき用）
    Storage&       Active()         noexcept { return _active; }
    const Storage& Active()   const noexcept { return _active; }
    Storage&       Fallback()       noexcept { return _fallback; }
    const Storage& Fallback() const noexcept { return _fallback; }

private:
    Storage _active;
    Storage _fallback;
};

} // namespace acs
