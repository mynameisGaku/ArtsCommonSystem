// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "container/Array.h"
#include "container/StringView.h"
#include "platform/FileExtensionKind.h"
#include "platform/FileSystemDiagnostics.h"

#include <type_traits>

namespace acs {

/** ファイル I/O とパス操作のユーティリティ (全メソッド static、Win32 実装)。 */
class CFileSystem {
public:
    /**
     * ASCII 大文字を小文字へ変換し、それ以外は変更しない。
     *
     * @param value 変換する文字。
     * @return 小文字化した文字。
     */
    static constexpr char AsciiLower(char value) noexcept {
        return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
    }

    /**
     * 文字が ASCII 範囲なら true を返す。
     *
     * @tparam Char 入力文字型。
     * @param value 確認する文字。
     * @return ASCII 範囲なら true。
     */
    template<typename Char>
    static constexpr bool IsAscii(Char value) noexcept {
        /** 符号なしへ変換した文字型。 */
        using U = std::make_unsigned_t<Char>;
        return static_cast<U>(value) <= static_cast<U>(0x7f);
    }

    /**
     * Windows または portable なパス区切りなら true を返す。
     *
     * @tparam Char 入力文字型。
     * @param value 確認する文字。
     * @return スラッシュまたはバックスラッシュなら true。
     */
    template<typename Char>
    static constexpr bool IsPathSeparator(Char value) noexcept {
        return value == static_cast<Char>('\\') || value == static_cast<Char>('/');
    }

    /**
     * NUL 終端パスの最終要素を ASCII 拡張子で分類する。
     *
     * @details 隠しファイル、末尾 dot、複数 dot の空拡張子、非 ASCII 拡張子は Unknown。
     * パス本体の Unicode は読み替えず、最終 dot より後ろだけを安全に ASCII 比較する。
     * @tparam Char 入力文字型。
     * @param path 分類する NUL 終端パス。
     * @return 判定できた拡張子種別。不正または未対応なら Unknown。
     */
    template<typename Char>
    static constexpr EFileExtensionKind ClassifyExtension(const Char* path) noexcept {
        if (!path) return EFileExtensionKind::Unknown;
        /** 最終パス要素の開始位置。 */
        usize segment_begin = 0;
        /** 最終パス要素にある最後の dot 位置。 */
        usize last_dot = static_cast<usize>(-1);
        /** パス全体の文字数。 */
        usize length = 0;
        for (; path[length] != static_cast<Char>(0); ++length) {
            if (IsPathSeparator(path[length])) {
                segment_begin = length + 1;
                last_dot = static_cast<usize>(-1);
            } else if (path[length] == static_cast<Char>('.')) {
                last_dot = length;
            }
        }
        if (last_dot == static_cast<usize>(-1) || last_dot == segment_begin || last_dot + 1 >= length) {
            return EFileExtensionKind::Unknown;
        }

        /** dot を除いた拡張子の文字数。 */
        const usize extension_size = length - last_dot - 1;
        /** ASCII 小文字へ正規化した拡張子。 */
        char extension[8]{};
        if (extension_size >= sizeof(extension)) return EFileExtensionKind::Unknown;
        /** 正規化する拡張子位置。 */
        for (usize i = 0; i < extension_size; ++i) {
            /** 現在正規化する文字。 */
            const Char value = path[last_dot + 1 + i];
            if (!IsAscii(value)) return EFileExtensionKind::Unknown;
            extension[i] = AsciiLower(static_cast<char>(value));
        }

        /** 正規化済み拡張子を ASCII 文字列と比較する。 */
        const auto equals = [&](const char* expected) constexpr noexcept {
            /** 比較する文字位置。 */
            usize i = 0;
            while (i < extension_size && expected[i] != '\0') {
                if (extension[i] != expected[i]) return false;
                ++i;
            }
            return i == extension_size && expected[i] == '\0';
        };
        if (equals("ini")) return EFileExtensionKind::Ini;
        if (equals("cfg")) return EFileExtensionKind::Config;
        if (equals("json")) return EFileExtensionKind::Json;
        if (equals("txt")) return EFileExtensionKind::Text;
        if (equals("bin")) return EFileExtensionKind::Binary;
        if (equals("acpak")) return EFileExtensionKind::AssetPack;
        return EFileExtensionKind::Unknown;
    }

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
     * @details char 配列を 1 回だけ確保して直接読み込み、末尾 NUL を付与する。
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
     * 同一ディレクトリの一時ファイルへ書き、rename で内容を原子的に公開する。
     *
     * @details 書き込みと FlushFileBuffers が成功するまで既存ファイルを変更しない。
     * 対象が reparse point の場合はリンクそのものを置換せず、従来の WriteAllBytes へ
     * 委譲してリンク先を書き換える。
     * @param path 書き出し先のファイルパス。
     * @param data 書き出すバイト列の先頭。
     * @param size 書き出すバイト数。
     * @return 成功なら空の TResult。一時ファイル作成・書き込み・置換失敗時はエラー。
     */
    static TResult<void> WriteAllBytesAtomic(const wchar_t* path, const byte* data, usize size) noexcept;

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
     * @details パス中の各区切りで中間ディレクトリを順に作成する。
     * ERROR_ALREADY_EXISTS は対象が実ディレクトリの場合だけ成功とみなす。
     * 既存のドライブrootとUNC share rootも成功扱いを維持する。
     * @param path 作成するディレクトリのパス。
     * @return 成功なら空の TResult。nullまたは空はIO:131、通常ファイルとの衝突や作成失敗はエラー。
     */
    static TResult<void> CreateDirectory(const wchar_t* path) noexcept;

    /**
     * ファイルを削除する。
     *
     * @param path 削除するファイルのパス。
     * @return 成功なら空の TResult、削除失敗時はエラー。
     */
    static TResult<void> Delete(const wchar_t* path) noexcept;

    /** 現在の I/O 診断値をスナップショットとして返す。 */
    static FFileSystemDiagnostics Diagnostics() noexcept;

    /** I/O 診断値だけを 0 に戻す。 */
    static void ResetDiagnostics() noexcept;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FFileSystem = CFileSystem;

} // namespace acs
