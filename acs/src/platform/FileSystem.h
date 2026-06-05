// SPDX-License-Identifier: Apache-2.0
// ファイル I/O とパス操作
//
// 使い方:
//   auto data = FileSystem::ReadAllBytes(L"data/save.bin");
//   if (data.IsErr()) { /* エラー処理 */ }
//   TSpan<const byte> bytes = data.Value().AsSpan();
//
//   FileSystem::WriteAllBytes(L"data/save.bin", bytes);
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "container/Array.h"
#include "container/StringView.h"

namespace acs {

/** ファイル I/O とパス操作のユーティリティ (全メソッド static、Win32 実装)。 */
class FileSystem {
public:
    /**
     * ファイル全体をバイト列として読み込む。
     *
     * @details 4GB を超えるファイルはエラー。OPEN_EXISTING で開くため未存在は失敗。
     * @param path 読み込むファイルのパス。
     * @return 読み込んだバイト列、開けない・サイズ過大・読み取り失敗時はエラー。
     */
    static TResult<TArray<byte>> ReadAllBytes(const wchar_t* path) noexcept;

    /**
     * ファイル全体を文字列として読み込む。
     *
     * @details ReadAllBytes の結果に末尾 NUL を付与して返す (中身は無変換のバイト列)。
     * @param path 読み込むファイルのパス。
     * @return NUL 終端付きの文字配列、読み取り失敗時はエラー。
     */
    static TResult<TArray<char>> ReadAllText(const wchar_t* path) noexcept;

    /**
     * バイト列をファイルへ書き出す (既存ファイルは上書き)。
     *
     * @details CREATE_ALWAYS で開くため既存内容は破棄される。
     * @param path 書き出し先のファイルパス。
     * @param data 書き出すバイト列の先頭。
     * @param size 書き出すバイト数。
     * @return 成功なら空の TResult、開けない・書き込み不足時はエラー。
     */
    static TResult<void> WriteAllBytes(const wchar_t* path, const byte* data, usize size) noexcept;

    /**
     * NUL 終端文字列をファイルへ書き出す (終端 NUL は書き込まない)。
     *
     * @param path 書き出し先のファイルパス。
     * @param text 書き出す NUL 終端文字列 (nullptr は長さ 0 扱い)。
     * @return 成功なら空の TResult、書き込み失敗時はエラー。
     */
    static TResult<void> WriteAllText(const wchar_t* path, const char* text) noexcept;

    /**
     * ファイルサイズをバイト単位で取得する。
     *
     * @param path 対象ファイルのパス。
     * @return ファイルサイズ (byte)、属性取得失敗時はエラー。
     */
    static TResult<u64> FileSize(const wchar_t* path) noexcept;

    /**
     * 指定パスがファイルとして存在するかを返す。
     *
     * @param path 確認するパス。
     * @return 存在しディレクトリでなければ true。
     */
    static bool Exists(const wchar_t* path) noexcept;

    /**
     * 指定パスがディレクトリとして存在するかを返す。
     *
     * @param path 確認するパス。
     * @return 存在しディレクトリなら true。
     */
    static bool DirectoryExists(const wchar_t* path) noexcept;

    /**
     * ディレクトリを作成する (既存なら成功扱い、親ディレクトリも再帰作成)。
     *
     * @details パス中の各区切りで中間ディレクトリを順に作成する。最終作成が
     * ERROR_ALREADY_EXISTS の場合も成功とみなす。
     * @param path 作成するディレクトリのパス。
     * @return 成功なら空の TResult、作成失敗時はエラー。
     */
    static TResult<void> CreateDirectory(const wchar_t* path) noexcept;

    /**
     * ファイルを削除する。
     *
     * @param path 削除するファイルのパス。
     * @return 成功なら空の TResult、削除失敗時はエラー。
     */
    static TResult<void> Delete(const wchar_t* path) noexcept;
};

} // namespace acs
