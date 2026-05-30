// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar G — FSettings (型付きゲーム設定)
//
// 音量・解像度・キーバインド等の「ゲームを跨いで永続化したい設定値」を
// 型付き key-value で保持する小型ストア。FInputMap (Pillar D) のキーコンフィグや
// AudioMixer の音量、Display の解像度・ウィンドウモード等の永続化先として使う。
//
// 使い方:
//   FSettings s;
//   s.SetF32 ("audio.master",   0.8f);
//   s.SetI32 ("display.width",  1920);
//   s.SetBool("display.vsync",  true);
//   s.SetString("locale",       "ja");
//
//   f32  master = s.GetF32 ("audio.master",  1.0f);
//   bool vsync  = s.GetBool("display.vsync", false);
//
//   s.Save(L"settings.ini");  // 次回起動時に Load() で復元
//
// 設計選択 (Phase 1 = Pillar G Phase 1):
//   ・**flat key-value**: key は `audio.master` のようなドット階層を文字列で表現する
//     だけで、ツリー構造は持たない。深いネストが必要になったら Phase 2 で `Section`
//     型を上にかぶせる方が安全 (今は YAGNI)。
//   ・**4 型タグ付き union**: f32 / i32 / bool / const char*。FVec2/FVec3 等の複合型は
//     呼び出し側で複数 key (`pos.x`, `pos.y`) に分割するか、Phase 2 で配列型を追加。
//   ・**key / string 値は非所有 const char***: ACS の STL 禁止方針 + 文字列ストア
//     導入を避けるため、key と string 値の寿命は呼び出し側が保証する (リテラル or
//     長寿命バッファ前提)。短命バッファ渡しが dangling になる点は要注意。
//   ・**同 key の SetX は上書き**: 同名 key が既に存在すれば値と kind を上書きする。
//     ユーザーが UI で「音量」を動かす度に SetF32() が呼ばれる典型ケースに合わせる。
//   ・**線形検索**: settings 件数は通常 10〜200 程度なので TArray<Entry> の線形走査で
//     十分。ハッシュテーブル化は計測してから検討。
//   ・**コピー / ムーブ禁止**: settings は通常 1 セッションに 1 オブジェクト
//     (グローバル所有) で運用される。誤って値渡しされて分裂すると同期ずれを
//     検知しづらいため、最初から非コピー・非ムーブで固定する。
//   ・**全 noexcept**: 例外不使用方針 (TResult<T,E> + bool 戻り値)。
//
// Save / Load (実装済み、round-trip 検証済み):
//   ・INI 風 `<tag>:<key>=<value>` テキスト (UTF-8 + LF) で読み書きする。Save は
//     FSaveArchive と同じ atomic write (`.tmp` → MoveFileExW rename) で破損を防ぐ。
//   ・型は **prefix tag** (`f:`, `i:`, `b:`, `s:`) でディスクに残し、Load 時に復元する。
//
// 範囲外 (Phase 2+):
//   ・配列値 / FVec2 / FVec3 / FColor 等の複合型 (現状は 4 プリミティブのみ)
//   ・change notification (オブザーバ pattern / callback)
//   ・section / namespace ツリー
//   ・暗号化 / 改竄検知 (Pillar S Storefront / FAssetPack 側で扱う)
//   ・schema migration (バージョン番号の埋め込みと変換)
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "container/Array.h"
#include "container/String.h"

namespace acs::game {

// 1 つの値の型タグ。Set 系で書き込まれ、Get 系で kind 一致を確認する。
// 不一致な型での Get は default_value を返す (silent fallback)。
enum class ESettingKind : u8 {
    None,   // 未設定 (内部用、外部からは見えない)
    F32,
    I32,
    Bool,
    FString,
};

class FSettings {
public:
    FSettings()  noexcept = default;
    ~FSettings() noexcept = default;

    FSettings(const FSettings&)            = delete;
    FSettings& operator=(const FSettings&) = delete;
    FSettings(FSettings&&)                 = delete;
    FSettings& operator=(FSettings&&)      = delete;

    // ----- 書き込み (同名 key は上書き、key == nullptr は no-op) -----
    void SetF32   (const char* key, f32         v) noexcept;
    void SetI32   (const char* key, i32         v) noexcept;
    void SetBool  (const char* key, bool        v) noexcept;
    // string 値の生存責任は呼び出し側 (リテラル or 長寿命バッファ)。
    void SetString(const char* key, const char* v) noexcept;

    // ----- 読み出し (未設定 / 型不一致 / key == nullptr は default_value を返す) -----
    f32         GetF32   (const char* key, f32         default_value = 0.0f) const noexcept;
    i32         GetI32   (const char* key, i32         default_value = 0)    const noexcept;
    bool        GetBool  (const char* key, bool        default_value = false) const noexcept;
    const char* GetString(const char* key, const char* default_value = "")   const noexcept;

    // key が登録済みか (kind 不問)。key == nullptr は false。
    bool Has   (const char* key) const noexcept;
    // 1 件削除 (該当なし / nullptr は no-op)。
    void Remove(const char* key) noexcept;
    // 全削除。
    void Clear () noexcept;
    // 件数 (kind 不問)。
    u32  Count () const noexcept;

    // ----- 永続化 -----
    // INI 風 `<tag>:<key>=<value>\n` テキスト (UTF-8 / LF) で読み書きする。
    // tag は型を round-trip させるための 1 文字 prefix:
    //   f:  f32    (例 `f:audio.master=0.8`)
    //   i:  i32    (例 `i:display.width=1920`)
    //   b:  bool   (例 `b:display.vsync=true`、値は true / false)
    //   s:  string (例 `s:locale=ja`)
    // Save は FSaveArchive と同じ atomic write (`.tmp` に書いて MoveFileExW で
    // rename) を使い、途中失敗で既存ファイルが破損しないようにする。
    // Load は各行を parse し、tag に応じて Set{F32,I32,Bool,String} へ復元する。
    // Load で復元した key / string 値は m_StringPool が所有する (リテラル前提の
    // Set* と違いファイル由来の文字列はストアより寿命が短いため、内部で複製する)。
    TResult<void> Save(const wchar_t* file_path) noexcept;
    TResult<void> Load(const wchar_t* file_path) noexcept;

private:
    // 1 件のエントリ。union で 4 種類の値を保持し、kind で実効型を区別する。
    // key / string 値は非所有 const char* (寿命は呼び出し側保証)。
    struct Entry {
        const char* key  = nullptr;
        ESettingKind kind = ESettingKind::None;
        union Value {
            f32         f;
            i32         i;
            bool        b;
            const char* s;
            Value() noexcept : f(0.0f) {}
        } value;
    };

    // key 一致 entry の index を返す。未検出は -1。key == nullptr も -1。
    isize FindIndex(const char* key) const noexcept;

    // FindIndex で見つかった entry を上書き or 新規 PushBack するヘルパ。
    Entry& UpsertEntry(const char* key) noexcept;

    TArray<Entry> m_Entries;

    // Load() が複製した key / string 値の所有プール。Set* に渡す const char* は
    // ここに格納した FString の Data() を指す。Load 冒頭で「行数 × 2」分を Reserve
    // して TArray の再確保を封じるため、格納後も各 FString の位置 (= Data() が返す
    // ポインタ) は object の寿命まで安定する (dangling 回避)。
    TArray<FString> m_StringPool;
};

} // namespace acs::game
