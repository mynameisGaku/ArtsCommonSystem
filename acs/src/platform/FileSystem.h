// ファイル I/O とパス操作
//
// 使い方:
//   auto data = FileSystem::ReadAllBytes(L"data/save.bin");
//   if (data.IsErr()) { /* エラー処理 */ }
//   Span<const byte> bytes = data.Value().AsSpan();
//
//   FileSystem::WriteAllBytes(L"data/save.bin", bytes);
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "container/Array.h"
#include "container/StringView.h"

namespace acs {

class FileSystem {
public:
    // ファイル全体をバイト列として読み込む
    static Result<Array<byte>> ReadAllBytes(const wchar_t* path) noexcept;

    // ファイル全体を文字列として読み込む（NUL 終端付与）
    static Result<Array<char>> ReadAllText(const wchar_t* path) noexcept;

    // バイト列を書き出す（既存ファイルは上書き）
    static Result<void> WriteAllBytes(const wchar_t* path, const byte* data, usize size) noexcept;

    // 文字列を書き出す（NUL 終端は書き込まない）
    static Result<void> WriteAllText(const wchar_t* path, const char* text) noexcept;

    // ファイルサイズを取得
    static Result<u64> FileSize(const wchar_t* path) noexcept;

    // ファイルが存在するか
    static bool Exists(const wchar_t* path) noexcept;

    // ディレクトリが存在するか
    static bool DirectoryExists(const wchar_t* path) noexcept;

    // ディレクトリを作成（既存なら成功扱い、親ディレクトリも再帰作成）
    static Result<void> CreateDirectory(const wchar_t* path) noexcept;

    // ファイルを削除
    static Result<void> Delete(const wchar_t* path) noexcept;
};

} // namespace acs
