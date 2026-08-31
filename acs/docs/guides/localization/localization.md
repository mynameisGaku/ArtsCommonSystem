# 翻訳文の読み込みと切り替え

ACS には、ファイルや埋め込みデータを所有して読む [`FLocalization`](../../../src/platform/Localization.h) と、長寿命の文字列をセッション中に登録する [`FLocalizationDirector`](../../../src/gameframework/LocalizationDirector.h) があります。

## ファイルから読み込む

`FLocalization` は現在言語と代替言語を、それぞれ `CStorage` として所有します。`Tr` は現在言語、代替言語、キー自身の順で検索します。

```cpp
#include "platform/FileSystem.h"
#include "platform/Localization.h"

acs::FLocalization text;
const wchar_t* fallback_path = L"lang/en.lang";
const wchar_t* active_path = L"lang/ja.lang";

if (!acs::CFileSystem::Exists(fallback_path) || !acs::CFileSystem::Exists(active_path)) {
    return;
}
if (text.LoadFallback(fallback_path).IsErr() || text.LoadActive(active_path).IsErr()) {
    return;
}

const char* start_label = text.Tr("menu.start");
```

言語ファイルは UTF-8 の `key=value` 形式です。

```ini
menu.start=ゲーム開始
menu.exit=終了
```

空行と、先頭が `#`、`;`、`[` の行は読み飛ばします。キーの前後にある空白は除きますが、`=` より後ろは先頭空白を含めて値として保持します。同じキーが二度現れた場合は読み込みに失敗し、読み込み前の翻訳表を保持します。

`CStorage::Load` はファイルが存在しない場合も成功を返し、既存の翻訳表を変更しません。ファイルの存在を必須にする場合は、例のように `CFileSystem::Exists` で先に確認します。

## 埋め込みデータを読む

`LoadActiveBytes` と `LoadFallbackBytes` は、実行ファイルに含めた翻訳文を直接読み込みます。

```cpp
static constexpr char kJapaneseText[] =
    "menu.start=ゲーム開始\n"
    "menu.exit=終了\n";

if (text.LoadActiveBytes(reinterpret_cast<const acs::u8*>(kJapaneseText), sizeof(kJapaneseText) - 1u).IsErr()) {
    return;
}
```

`data == nullptr` は空の翻訳表として成功します。解析またはメモリ確保に失敗した場合は、読み込み前の表を保持します。

## 言語を切り替える

別の現在言語を `LoadActive` で読み込むか、`Swap` で現在言語と代替言語を入れ替えます。`Clear` は両方の表を消します。

`Tr` が返す `const char*` は内部文字列への参照です。`LoadActive`、`LoadFallback`、`Swap`、`Clear`、または `Active` と `Fallback` を介した変更の後まで保持せず、必要なら呼び出し側の文字列へコピーします。`Tr(nullptr)` は空文字列を返します。

## セッション登録型を使う

`acs::game::FLocalizationDirector` は、`ELocale` ごとに非所有の `const char*` を登録します。文字列リテラルや、セッションより長く生存するバッファに向いています。

```cpp
#include "gameframework/LocalizationDirector.h"

acs::game::FLocalizationDirector text;
text.RegisterString(acs::game::ELocale::En, "menu.start", "Start");
text.RegisterString(acs::game::ELocale::Ja, "menu.start", "ゲーム開始");
text.SetLocale(acs::game::ELocale::Ja);

const char* start_label = text.Get("menu.start");
```

`Get` は現在の `ELocale`、`ELocale::Default`、キー自身の順で検索します。`ELocale::Default` は `ELocale::En` と同じ値です。`Has` は現在の言語だけを調べ、代替言語は調べません。

登録したキーと値はコピーされません。登録元を先に破棄してはいけません。同じ言語とキーを複数回登録した場合は、先に登録した値を返します。
