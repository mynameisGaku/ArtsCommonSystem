// SPDX-License-Identifier: Apache-2.0
// ファイル I/O とパス操作
//
// 使い方:
//   auto data = FFileSystem::ReadAllBytes(L"data/save.bin");
//   if (data.IsErr()) { /* エラー処理 */ }
//   TSpan<const byte> bytes = data.Value().AsSpan();
//
//   FFileSystem::WriteAllBytes(L"data/save.bin", bytes);
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "container/Array.h"
#include "container/StringView.h"

namespace acs {

class FFileSystem {
public:
    // ファイル全体をバイト列として読み込む
    static TResult<TArray<byte>> ReadAllBytes(const wchar_t* path) noexcept;

    // ファイル全体を文字列として読み込む（NUL 終端付与）
    static TResult<TArray<char>> ReadAllText(const wchar_t* path) noexcept;

    // バイト列を書き出す（既存ファイルは上書き）
    static TResult<void> WriteAllBytes(const wchar_t* path, const byte* data, usize size) noexcept;

    // 文字列を書き出す（NUL 終端は書き込まない）
    static TResult<void> WriteAllText(const wchar_t* path, const char* text) noexcept;

    // ファイルサイズを取得
    static TResult<u64> FileSize(const wchar_t* path) noexcept;

    // ファイルが存在するか
    static bool Exists(const wchar_t* path) noexcept;

    // ディレクトリが存在するか
    static bool DirectoryExists(const wchar_t* path) noexcept;

    // ディレクトリを作成（既存なら成功扱い、親ディレクトリも再帰作成）
    static TResult<void> CreateDirectory(const wchar_t* path) noexcept;

    // ファイルを削除
    static TResult<void> Delete(const wchar_t* path) noexcept;
};

} // namespace acs
