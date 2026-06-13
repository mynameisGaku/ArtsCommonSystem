// SPDX-License-Identifier: Apache-2.0
// プロジェクト設定 (UE の Project Settings 相当)
//
// 用途: プロジェクトごとの設定 (レンダリング / エディタ / 物理 / ゲーム / ユーザー定義) を
//       カテゴリ分けして INI テキスト (<project>/Config/ProjectSettings.ini) で永続化する。
//       ビルトイン項目はスキーマ (型・既定値・説明) を持ち、ユーザー定義項目は任意の
//       カテゴリ/キー/値 (文字列) を追加できる。エディタ (editor_abi) とランタイム
//       (スタンドアロンゲーム) の両方から同じファイルを読める。
//
// ファイル形式:
//   ; コメント
//   [Rendering]
//   MsaaSamples=8
//   AmbientColor=0.10,0.11,0.13
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "container/Array.h"

namespace acs::game {

/** 設定値の型 (エディタ UI のエディタ選択と検証に使う)。 */
enum class ESettingType : u8 {
    Float,    ///< 単一の実数。
    Int,      ///< 整数。
    Bool,     ///< true/false (INI では 1/0)。
    Color,    ///< "r,g,b" (0..1 の実数 3 つ)。
    String,   ///< 任意文字列 (ユーザー定義項目の既定)。
    Enum,     ///< options ("A|B|C") から選ぶ。値は選択肢の文字列。
};

/** ビルトイン設定項目のスキーマ (静的テーブル)。 */
struct FSettingDesc {
    const char*  category;   ///< カテゴリ名 (INI のセクション)。
    const char*  key;        ///< キー名。
    ESettingType type;       ///< 値の型。
    const char*  def;        ///< 既定値 (文字列表現)。
    const char*  options;    ///< Enum の選択肢 "A|B|C" (Enum 以外は nullptr)。
    const char*  desc;       ///< 説明 (エディタのツールチップ)。
};

/** 1 つの設定エントリ (ビルトイン or ユーザー定義)。 */
struct FSettingEntry {
    char         category[32] = {};
    char         key[64]      = {};
    char         value[192]   = {};
    ESettingType type         = ESettingType::String;
    const char*  options      = nullptr;   ///< ビルトインスキーマ参照 (カスタムは nullptr)。
    const char*  desc         = nullptr;   ///< 同上。
    bool         builtin      = false;     ///< false=ユーザー定義 (削除可)。
};

/**
 * プロジェクト設定のストア (ビルトインスキーマ + ユーザー定義、INI 読み書き)。
 *
 * @details
 * ResetToDefaults でスキーマ全項目を既定値で持ち、Load で INI の値を上書き +
 * 未知のキーをユーザー定義として取り込む。値は全て文字列で保持し、型付き Get*
 * でパースして返す (パース失敗は既定値)。
 */
class FProjectSettings {
public:
    /** ビルトインスキーマの全項目を既定値で再構築する (ユーザー定義は消える)。 */
    void ResetToDefaults() noexcept;

    /**
     * INI ファイルから読み込む。
     *
     * @details 先に ResetToDefaults 相当を行い、ファイルの値で上書きする。スキーマに
     * 無い [カテゴリ] キー はユーザー定義 (String) として追加する。ファイルが無い
     * 場合は既定値のまま true を返す (初回起動)。
     * @param ini_path 読み込む INI のパス (UTF-8)。
     * @return パスが開けたか既定値で初期化できたら true。
     */
    bool Load(const char* ini_path) noexcept;

    /**
     * INI ファイルへ保存する (カテゴリごとにセクション出力)。
     *
     * @param ini_path 書き込む INI のパス (UTF-8。親ディレクトリは呼び出し側が用意)。
     * @return 書き込みに成功したら true。
     */
    bool Save(const char* ini_path) const noexcept;

    /**
     * INI テキスト (メモリ上) から読み込む。
     *
     * @details Load(path) と同じ規則。エディタは C# がファイル I/O を担うため、
     * テキストを受け取ってここでパースする。
     * @param text INI 全文 (UTF-8、NUL 終端)。
     * @return text が非 null なら true。
     */
    bool LoadText(const char* text) noexcept;

    /**
     * INI テキストへシリアライズする。
     *
     * @param out 出力バッファ (NUL 終端される)。
     * @param cap out の容量 (バイト)。
     * @return 書いた文字数 (NUL 除く。cap 不足時は cap-1 で打ち切り)。
     */
    u32 SerializeText(char* out, usize cap) const noexcept;

    /** エントリ数を返す。 */
    u32 Count() const noexcept { return m_Entries.Size(); }

    /** index 番目のエントリを返す (範囲外は触らないこと)。 */
    const FSettingEntry& At(u32 i) const noexcept { return m_Entries[i]; }

    /** カテゴリ+キーでエントリを探す (無ければ nullptr)。 */
    const FSettingEntry* Find(const char* cat, const char* key) const noexcept;

    /** 既存エントリの値を設定する (無ければ false)。 */
    bool Set(const char* cat, const char* key, const char* value) noexcept;

    /** ユーザー定義エントリを追加する (既存キーと重複したら値更新)。 */
    bool Add(const char* cat, const char* key, const char* value) noexcept;

    /** ユーザー定義エントリを削除する (ビルトインは削除不可 → false)。 */
    bool Remove(const char* cat, const char* key) noexcept;

    /** 実数として取得する (無い/パース不能は def)。 */
    f32 GetFloat(const char* cat, const char* key, f32 def) const noexcept;

    /** 整数として取得する (無い/パース不能は def)。 */
    i32 GetInt(const char* cat, const char* key, i32 def) const noexcept;

    /** 真偽として取得する ("1"/"true" = true)。 */
    bool GetBool(const char* cat, const char* key, bool def) const noexcept;

    /** 色 "r,g,b" として取得する (無い/パース不能は def)。 */
    FVec3 GetColor(const char* cat, const char* key, FVec3 def) const noexcept;

    /** 文字列として取得する (無ければ def)。 */
    const char* GetString(const char* cat, const char* key, const char* def) const noexcept;

private:
    /** スキーマからエントリを作る (Load/ResetToDefaults 用)。 */
    void AppendBuiltin(const FSettingDesc& d) noexcept;

    TArray<FSettingEntry> m_Entries;
};

/**
 * ビルトイン設定スキーマを返す (静的テーブル)。
 *
 * @param out_count テーブルの項目数の出力。
 * @return スキーマ配列の先頭 (静的寿命)。
 */
const FSettingDesc* BuiltinSettingSchema(u32& out_count) noexcept;

} // namespace acs::game
