// ファイル I/O 実装 — std::filesystem + std::ifstream/ofstream で portable
//
// 旧実装は Win32 CreateFileW 直叩きだったが、std 実装に置換。Windows でも
// MSVC の std::filesystem は内部で同じ API を呼ぶので性能差は無視できる。
// 公開 API は wchar_t* のままで Win 後方互換、内部で std::filesystem::path に
// 変換 (Win では native wchar、POSIX では UTF-8 narrow に変換)。
#include "platform/FileSystem.h"
#include "foundation/Compiler.h"
#include "foundation/Move.h"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace acs {

namespace {

// wchar_t* → fs::path (両プラットフォーム対応)。POSIX では wcstombs で UTF-8 narrow へ。
fs::path ToPath(const wchar_t* p) noexcept {
    if (!p) return fs::path{};
#if ACS_PLATFORM_WINDOWS
    return fs::path{ p };
#else
    // wchar_t を UTF-8 char* に変換 (簡易版: ASCII 範囲なら 1:1、CJK は wcstombs)
    char buf[1024];
    std::mbstate_t st{};
    const wchar_t* src = p;
    std::wcsrtombs(buf, &src, sizeof(buf) - 1, &st);
    buf[sizeof(buf) - 1] = 0;
    return fs::path{ buf };
#endif
}

} // namespace

// ファイル全体をバイト列として読み込む
Result<Array<byte>> FileSystem::ReadAllBytes(const wchar_t* path) noexcept {
    fs::path p = ToPath(path);
    std::error_code ec;
    auto sz = fs::file_size(p, ec);
    if (ec) return ACS_ERR_OS(IO, 100, "file_size failed", static_cast<u32>(ec.value()));
    if (sz > 0xFFFFFFFFull) return ACS_ERR(IO, 102, "File too large (>4GB)");

    std::ifstream f(p, std::ios::binary);
    if (!f) return ACS_ERR(IO, 103, "ifstream open failed");

    Array<byte> buf;
    buf.Resize(static_cast<usize>(sz));
    if (sz > 0) {
        f.read(reinterpret_cast<char*>(buf.Data()), static_cast<std::streamsize>(sz));
        if (f.gcount() != static_cast<std::streamsize>(sz))
            return ACS_ERR(IO, 104, "ifstream short read");
    }
    return Result<Array<byte>>(OkInit, Move(buf));
}

// ファイル全体を文字列として読み込む（末尾に NUL を付与する）
Result<Array<char>> FileSystem::ReadAllText(const wchar_t* path) noexcept {
    auto br = ReadAllBytes(path);
    if (br.IsErr()) return Err<Array<char>>(br.Error());
    Array<byte>& b = br.Value();
    Array<char> out;
    out.Resize(b.Size() + 1);
    for (usize i = 0; i < b.Size(); ++i) out[i] = static_cast<char>(b[i]);
    out[b.Size()] = '\0';
    return Result<Array<char>>(OkInit, Move(out));
}

// バイト列を書き出す（上書き）
Result<void> FileSystem::WriteAllBytes(const wchar_t* path, const byte* data, usize size) noexcept {
    fs::path p = ToPath(path);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return ACS_ERR(IO, 110, "ofstream open failed");
    if (size > 0) {
        f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!f.good()) return ACS_ERR(IO, 111, "ofstream write failed");
    }
    return Ok();
}

// 文字列を書き出す（NUL 終端は書かない）
Result<void> FileSystem::WriteAllText(const wchar_t* path, const char* text) noexcept {
    usize len = 0;
    while (text && text[len]) ++len;
    return WriteAllBytes(path, reinterpret_cast<const byte*>(text), len);
}

// ファイルサイズ取得
Result<u64> FileSystem::FileSize(const wchar_t* path) noexcept {
    fs::path p = ToPath(path);
    std::error_code ec;
    auto sz = fs::file_size(p, ec);
    if (ec) return ACS_ERR_OS(IO, 120, "file_size failed", static_cast<u32>(ec.value()));
    return Result<u64>(OkInit, static_cast<u64>(sz));
}

// ファイル存在確認
bool FileSystem::Exists(const wchar_t* path) noexcept {
    std::error_code ec;
    return fs::is_regular_file(ToPath(path), ec);
}

// ディレクトリ存在確認
bool FileSystem::DirectoryExists(const wchar_t* path) noexcept {
    std::error_code ec;
    return fs::is_directory(ToPath(path), ec);
}

// ディレクトリ作成 (親も再帰作成、既存なら成功扱い)
Result<void> FileSystem::CreateDirectory(const wchar_t* path) noexcept {
    fs::path p = ToPath(path);
    std::error_code ec;
    if (fs::is_directory(p, ec)) return Ok();
    fs::create_directories(p, ec);
    if (ec) return ACS_ERR_OS(IO, 130, "create_directories failed",
                               static_cast<u32>(ec.value()));
    return Ok();
}

// ファイル削除
Result<void> FileSystem::Delete(const wchar_t* path) noexcept {
    std::error_code ec;
    if (!fs::remove(ToPath(path), ec))
        return ACS_ERR_OS(IO, 140, "remove failed", static_cast<u32>(ec.value()));
    return Ok();
}

} // namespace acs
