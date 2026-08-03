// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// acs_assetpack — `.acpak` v1 アーカイブ操作 CLI ツール
// -----------------------------------------------------------------------------
// 使い方 (samples):
//   acs_assetpack pack ./assets game.acpak --compress
//   acs_assetpack list game.acpak
//   acs_assetpack unpack game.acpak ./extracted
//   acs_assetpack pack ./assets game_secret.acpak --compress --encrypt \
//                 --key-file game.key
//   acs_assetpack unpack game_secret.acpak ./extracted --key-file game.key
//   acs_assetpack verify game_secret.acpak --key-file game.key
//   acs_assetpack info game.acpak
//   acs_assetpack help [subcommand]
//
// exit code:
//   0 — success
//   1 — usage error (引数不足 / 未知 subcommand / parse 失敗)
//   2 — runtime error (I/O 失敗 / CRC mismatch / pak 破損)
//
// 設計上のルール:
//   ・STL 不使用 (TArray<T> / wchar_t* のみ)
//   ・<string> 禁止 (printf / wprintf + char[256] スタックバッファ)
//   ・例外なし (TResult<T,E> を使うが、ここでは IsErr で hard-exit する CLI なので
//     ACS_TRY マクロは使わず、明示分岐で 1 / 2 を返す)
//   ・Windows 専用 — `main(argc, argv)` + GetCommandLineW を使って UTF-16 引数を
//     取得する (wmain は MinGW など環境差があるため使わない)
//
// 暗号化 / 圧縮:
//   ・--compress は LZ4 (block)、--encrypt は AES-256-GCM (Windows CNG/BCrypt)。
//     どちらもライブラリ (FAcpakLz4 / FAcpakCrypto) で実装済。CLI は --key-file /
//     --key で 256-bit 鍵を読み、Writer/Reader::SetKey() に渡す。
//   ・鍵は --key-file 推奨 (生鍵を CLI 引数に書くとシェル履歴 / CI ログに残るため。
//     docs/AssetPack.md §4.4)。鍵ファイルはバージョン管理にコミットしないこと。
// =============================================================================

#include "assetpack/AcpakFormat.h"
#include "assetpack/AcpakReader.h"
#include "assetpack/AcpakWriter.h"
#include "platform/FileSystem.h"
#include "container/Array.h"
#include "memory/Memory.h"
#include "foundation/Platform.h"   // <windows.h> を 1 箇所に隠す
#include "foundation/Types.h"
#include "foundation/Result.h"
#include "foundation/Move.h"        // acs::Move (rvalue cast)

#include <cstdio>     // printf / wprintf / fwprintf / fputs
#include <shellapi.h> // CommandLineToArgvW

using namespace acs;
using namespace acs::assetpack;

// ============================================================================
// usage 文字列 (静的リテラル — heap / std::string を使わない)
// ============================================================================
namespace {

constexpr const char* kUsageMain =
    "acs_assetpack — .acpak v1 archive utility\n"
    "\n"
    "usage:\n"
    "  acs_assetpack <subcommand> [args...]\n"
    "\n"
    "subcommands:\n"
    "  pack    <input_dir> <output.acpak> [--compress] [--encrypt --key-file <p>]\n"
    "  unpack  <input.acpak> <output_dir>             [--key-file <p> | --key <hex>]\n"
    "  list    <input.acpak>\n"
    "  verify  <input.acpak>                          [--key-file <p> | --key <hex>]\n"
    "  info    <input.acpak>\n"
    "  help    [subcommand]\n"
    "\n"
    "key options: --key-file <path> reads 64 hex chars (256-bit) from a file (preferred,\n"
    "             keeps the raw key out of shell history). --key <hex> passes it inline.\n"
    "exit codes: 0=ok, 1=usage error, 2=runtime error\n";

constexpr const char* kUsagePack =
    "acs_assetpack pack <input_dir> <output.acpak> [--compress] [--encrypt --key-file <p>]\n"
    "  input_dir を再帰スキャンして全ファイルを .acpak v1 アーカイブにまとめる。\n"
    "  --compress : エントリを LZ4 圧縮 (既圧縮なら自動で生格納)。\n"
    "  --encrypt  : AES-256-GCM で暗号化 (--key-file か --key で 256-bit 鍵を要求)。\n"
    "  --key-file <path> : 64 hex 文字の鍵ファイルを読む (推奨。鍵はコミット禁止)。\n"
    "  --key <hex>       : 64 hex の鍵を直接渡す (シェル履歴に残る点に注意)。\n";

constexpr const char* kUsageUnpack =
    "acs_assetpack unpack <input.acpak> <output_dir> [--key-file <path> | --key <hex>]\n"
    "  .acpak の全 entry を output_dir/<virtual_path> に書き出す。\n"
    "  暗号化 pak は --key-file / --key で復号鍵を渡す。CRC32 を検証しながら読み出す。\n";

constexpr const char* kUsageList =
    "acs_assetpack list <input.acpak>\n"
    "  各 entry の path / size / crc32 を 1 行ずつ表示する。\n";

constexpr const char* kUsageVerify =
    "acs_assetpack verify <input.acpak> [--key-file <path> | --key <hex>]\n"
    "  全 entry を読み出して検証し (暗号化 pak は復号鍵が必要)、失敗があれば exit 2。\n";

constexpr const char* kUsageInfo =
    "acs_assetpack info <input.acpak>\n"
    "  ヘッダ情報 (magic / version / flags / file count) を表示する。\n";

// ----------------------------------------------------------------------------
// 文字列ユーティリティ (依存ヘッダを増やさない自前実装)
// ----------------------------------------------------------------------------

usize StrLenW(const wchar_t* s) noexcept {
    usize n = 0;
    if (s == nullptr) return 0;
    while (s[n]) ++n;
    return n;
}

usize StrLenA(const char* s) noexcept {
    usize n = 0;
    if (s == nullptr) return 0;
    while (s[n]) ++n;
    return n;
}

bool EqualsW(const wchar_t* a, const wchar_t* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    while (*a && (*a == *b)) { ++a; ++b; }
    return *a == 0 && *b == 0;
}

// UTF-16 → UTF-8 変換 (out_size は終端 NUL を含むバッファサイズ)。
// 失敗時は 0 を out_buf[0] に書き、false を返す。
bool Utf16ToUtf8(const wchar_t* src, char* out_buf, int out_size) noexcept {
    if (out_size <= 0) return false;
    out_buf[0] = '\0';
    if (src == nullptr) return false;
    int n = ::WideCharToMultiByte(CP_UTF8, 0, src, -1, out_buf, out_size, nullptr, nullptr);
    return n > 0;
}

// UTF-8 → UTF-16 変換 (out_size は wchar_t 数で、終端 NUL を含む)。
bool Utf8ToUtf16(const char* src, wchar_t* out_buf, int out_size) noexcept {
    if (out_size <= 0) return false;
    out_buf[0] = L'\0';
    if (src == nullptr) return false;
    int n = ::MultiByteToWideChar(CP_UTF8, 0, src, -1, out_buf, out_size);
    return n > 0;
}

// hex char ('0'-'9' / 'a'-'f' / 'A'-'F') → 4-bit。範囲外は 0xFF。
u8 HexNibble(wchar_t c) noexcept {
    if (c >= L'0' && c <= L'9') return static_cast<u8>(c - L'0');
    if (c >= L'a' && c <= L'f') return static_cast<u8>(10 + (c - L'a'));
    if (c >= L'A' && c <= L'F') return static_cast<u8>(10 + (c - L'A'));
    return 0xFFu;
}

// hex string (UTF-16) → 32-byte (256-bit) key。長さは厳密に 64 文字を要求。
// 成功時 true、失敗時 false (途中まで書き込んだ buf は未定義扱い)。
bool ParseHex256(const wchar_t* hex, u8 out_key[32]) noexcept {
    if (hex == nullptr) return false;
    const usize n = StrLenW(hex);
    if (n != 64) return false;
    for (usize i = 0; i < 32; ++i) {
        u8 hi = HexNibble(hex[i * 2 + 0]);
        u8 lo = HexNibble(hex[i * 2 + 1]);
        if (hi > 0x0F || lo > 0x0F) return false;
        out_key[i] = static_cast<u8>((hi << 4) | lo);
    }
    return true;
}

// 鍵ファイルを読んで 32-byte key にする。ファイルは 64 hex 文字を含む
// (前後の空白 / 改行は許容、それ以外の非 hex 文字があれば失敗)。
// 設計方針 (docs/AssetPack.md §4.4): 生鍵を CLI 引数で渡すとシェル履歴 / CI ログに
// 残るため、鍵はファイルから読む。鍵ファイルはバージョン管理にコミットしない。
// 成功時 true、失敗時 false (理由は err_out に英語で書く。out_size は char 数)。
bool ReadKeyFile(const wchar_t* path, u8 out_key[32],
                 char* err_out, int err_size) noexcept {
    auto rb = FFileSystem::ReadAllBytes(path);
    if (rb.IsErr()) {
        std::snprintf(err_out, static_cast<usize>(err_size),
                      "cannot read key file (subcode=%u)",
                      static_cast<unsigned>(rb.Error().subcode));
        return false;
    }
    const TArray<byte>& bytes = rb.Value();

    // hex nibble だけを 64 個集める (空白 / 改行はスキップ)。
    wchar_t hex[65];
    usize   n = 0;
    for (usize i = 0; i < bytes.Num(); ++i) {
        char c = static_cast<char>(bytes[i]);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (n >= 64) {
            std::snprintf(err_out, static_cast<usize>(err_size),
                          "key file has more than 64 hex chars");
            return false;
        }
        if (HexNibble(static_cast<wchar_t>(c)) > 0x0F) {
            std::snprintf(err_out, static_cast<usize>(err_size),
                          "key file contains a non-hex character");
            return false;
        }
        hex[n++] = static_cast<wchar_t>(c);
    }
    if (n != 64) {
        std::snprintf(err_out, static_cast<usize>(err_size),
                      "key file must contain exactly 64 hex chars (256-bit), found %u",
                      static_cast<unsigned>(n));
        return false;
    }
    hex[64] = L'\0';
    if (!ParseHex256(hex, out_key)) {
        std::snprintf(err_out, static_cast<usize>(err_size),
                      "key file hex parse failed");
        return false;
    }
    return true;
}

// path と name を結合して dst に書く。
// dst_size は wchar_t 数。失敗 (バッファ不足) なら false。
bool JoinPath(const wchar_t* base, const wchar_t* name,
              wchar_t* dst, usize dst_size) noexcept {
    usize bn = StrLenW(base);
    usize nn = StrLenW(name);
    bool need_sep = (bn > 0 && base[bn - 1] != L'\\' && base[bn - 1] != L'/');
    usize total = bn + (need_sep ? 1u : 0u) + nn + 1u;
    if (total > dst_size) return false;
    usize p = 0;
    for (usize i = 0; i < bn; ++i) dst[p++] = base[i];
    if (need_sep) dst[p++] = L'\\';
    for (usize i = 0; i < nn; ++i) dst[p++] = name[i];
    dst[p] = L'\0';
    return true;
}

// "a/b\\c" → "a/b/c" に正規化 (pak 内 virtual path 用)。in-place。
void NormalizeSlashes(wchar_t* s) noexcept {
    if (s == nullptr) return;
    while (*s) { if (*s == L'\\') *s = L'/'; ++s; }
}

// ----------------------------------------------------------------------------
// FErrorCode 出力 (printf ベース、ロガー初期化不要)
// ----------------------------------------------------------------------------
void PrintError(const FErrorCode& e, const char* where) noexcept {
    char  buf[256];
    const char* msg = e.message ? e.message : "(no message)";
    if (e.os_error != 0) {
        std::snprintf(buf, sizeof(buf),
                      "[error] %s: %s (category=%s, subcode=%u, os_error=%u)\n",
                      where, msg, ToString(e.category),
                      static_cast<unsigned>(e.subcode),
                      static_cast<unsigned>(e.os_error));
    } else {
        std::snprintf(buf, sizeof(buf),
                      "[error] %s: %s (category=%s, subcode=%u)\n",
                      where, msg, ToString(e.category),
                      static_cast<unsigned>(e.subcode));
    }
    std::fputs(buf, stderr);
}

// ----------------------------------------------------------------------------
// 引数 parse 用の小さなコンテナ (argv を wchar_t** に変換した結果を保持)
// ----------------------------------------------------------------------------
struct FWideArgs {
    TArray<wchar_t*> argv;   // 各 ptr は CommandLineToArgvW の戻り値配列内を指す
    void*           raw_argv = nullptr;  // LocalFree 用 (LPWSTR*)
    int             argc = 0;

    ~FWideArgs() noexcept {
        if (raw_argv) ::LocalFree(raw_argv);
    }
};

bool BuildWideArgs(FWideArgs& out) noexcept {
    LPWSTR  cmdline = ::GetCommandLineW();
    int     ac      = 0;
    LPWSTR* av      = ::CommandLineToArgvW(cmdline, &ac);
    if (av == nullptr || ac <= 0) return false;
    out.raw_argv = av;
    out.argc     = ac;
    out.argv.Reserve(static_cast<usize>(ac));
    for (int i = 0; i < ac; ++i) out.argv.Add(av[i]);
    return true;
}

// ----------------------------------------------------------------------------
// バイトサイズを "1.23 MB" 風に整形する。out_size は char 数。
// CRC32 検証は FAcpakReader::ReadFile が内部で行う (poly 0xEDB88320)。
// ----------------------------------------------------------------------------
void FormatBytes(u64 bytes, char* out, int out_size) noexcept {
    if (out_size <= 0) return;
    const char* units[5] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    if (u == 0) {
        std::snprintf(out, static_cast<usize>(out_size), "%llu %s",
                      static_cast<unsigned long long>(bytes), units[u]);
    } else {
        std::snprintf(out, static_cast<usize>(out_size), "%.2f %s", v, units[u]);
    }
}

// ============================================================================
// pack — input_dir 再帰スキャン → 各 file を ReadAllBytes → AddFile → Finalize
// ============================================================================
// AddFile は data ポインタをコピーせず参照だけ保持するため、Finalize までは
// 全ファイルの byte 配列を生かしておく必要がある。よって m_AllBlobs に
// TArray<byte> をすべて貯めてから Finalize する。

struct FPackedBlob {
    TArray<byte>    bytes;    // ファイルの全バイト (Finalize までは生かす)
    TArray<wchar_t> path;     // pak 内仮想パス (NUL 終端)
};

// Windows の FindFirstFileW / FindNextFileW が返す順序はファイルシステム依存。
// Writer は AddFile 順に payload/table を書くため、仮想パスの UTF-16 code unit
// ordinal 順へ固定してから渡し、同じ入力から byte-identical な pak を作る。
int ComparePackedPath(const wchar_t* a, const wchar_t* b) noexcept {
    if (a == nullptr) return b == nullptr ? 0 : -1;
    if (b == nullptr) return 1;
    while (*a != L'\0' && *b != L'\0') {
        if (*a < *b) return -1;
        if (*a > *b) return 1;
        ++a;
        ++b;
    }
    if (*a == *b) return 0;
    return *a == L'\0' ? -1 : 1;
}

void SiftDownPackedBlobs(TArray<FPackedBlob>& blobs,
                         usize root, usize count) noexcept {
    while (root <= (count - 1u) / 2u) {
        const usize left = root * 2u + 1u;
        if (left >= count) break;
        usize greater = left;
        const usize right = left + 1u;
        if (right < count &&
            ComparePackedPath(blobs[left].path.GetData(),
                              blobs[right].path.GetData()) < 0) {
            greater = right;
        }
        if (ComparePackedPath(blobs[root].path.GetData(),
                              blobs[greater].path.GetData()) >= 0) {
            break;
        }
        acs::Swap(blobs[root], blobs[greater]);
        root = greater;
    }
}

void SortPackedBlobs(TArray<FPackedBlob>& blobs) noexcept {
    const usize count = blobs.Num();
    if (count < 2u) return;

    for (usize start = count / 2u; start > 0u; --start) {
        SiftDownPackedBlobs(blobs, start - 1u, count);
    }
    for (usize end = count; end > 1u; --end) {
        acs::Swap(blobs[0], blobs[end - 1u]);
        SiftDownPackedBlobs(blobs, 0u, end - 1u);
    }
}

// dir を再帰スキャンし、各ファイルを out_blobs に push する。
// base_dir は input_dir の絶対基点 (これより下を virtual_path にする)。
// rel_prefix は現在 dir の base_dir からの相対 prefix (末尾セパレータなし)。
//
// 戻り値: 成功 = ok / 失敗 = error code。usage error は呼び出し側で処理する。
TResult<void> ScanDirRecursive(const wchar_t*       cur_dir,
                              const wchar_t*       rel_prefix,
                              TArray<FPackedBlob>&   out_blobs) noexcept {
    // ワイルドカードパス "cur_dir\\*" を作る
    wchar_t pattern[4096];
    if (!JoinPath(cur_dir, L"*", pattern, sizeof(pattern) / sizeof(pattern[0]))) {
        return ACS_ERR(IO, 9001, "pack: path too long");
    }

    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = ::GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) return Ok(); // 空ディレクトリ
        return ACS_ERR_OS(IO, 9002, "pack: FindFirstFileW failed", err);
    }

    do {
        // "." と ".." はスキップ
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == 0 ||
             (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) {
            continue;
        }

        // 次の階層用の rel path を作る
        wchar_t next_rel[4096];
        if (rel_prefix[0] == L'\0') {
            // root: cFileName そのものを rel_path にする
            usize n = StrLenW(fd.cFileName);
            if (n + 1 > sizeof(next_rel) / sizeof(next_rel[0])) {
                ::FindClose(h);
                return ACS_ERR(IO, 9003, "pack: rel path too long");
            }
            MemCopy(next_rel, fd.cFileName, (n + 1) * sizeof(wchar_t));
        } else {
            if (!JoinPath(rel_prefix, fd.cFileName, next_rel,
                          sizeof(next_rel) / sizeof(next_rel[0]))) {
                ::FindClose(h);
                return ACS_ERR(IO, 9004, "pack: rel path too long");
            }
        }

        // 絶対パスも組み立てる (CreateFileW / 次階層 ScanDirRecursive 用)
        wchar_t abs_path[4096];
        if (!JoinPath(cur_dir, fd.cFileName, abs_path,
                      sizeof(abs_path) / sizeof(abs_path[0]))) {
            ::FindClose(h);
            return ACS_ERR(IO, 9005, "pack: abs path too long");
        }

        // junction / symlink / cloud placeholder 等を辿ると入力 tree 外のデータを
        // 意図せず梱包できる。Cook pipeline も事前検証するが、CLI 自体も fail-closed。
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            ::FindClose(h);
            return ACS_ERR(IO, 9006, "pack: reparse point input is not allowed");
        }

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // 再帰
            auto r = ScanDirRecursive(abs_path, next_rel, out_blobs);
            if (r.IsErr()) {
                ::FindClose(h);
                return r;
            }
        } else {
            // 通常ファイル — 読み込んで blob に積む
            auto rb = FFileSystem::ReadAllBytes(abs_path);
            if (rb.IsErr()) {
                ::FindClose(h);
                return rb.Error();
            }
            FPackedBlob blob;
            blob.bytes = Move(rb.Value());

            // virtual path をコピーして slash 正規化
            usize plen = StrLenW(next_rel);
            blob.path.SetNum(plen + 1);
            MemCopy(blob.path.GetData(), next_rel, plen * sizeof(wchar_t));
            blob.path[plen] = L'\0';
            NormalizeSlashes(blob.path.GetData());

            out_blobs.Add(Move(blob));
        }
    } while (::FindNextFileW(h, &fd));

    ::FindClose(h);
    return Ok();
}

int CmdPack(const TArray<wchar_t*>& argv) noexcept {
    // argv: [0]=exe [1]=pack [2]=input_dir [3]=output.acpak [opts...]
    if (argv.Num() < 4) {
        std::fputs(kUsagePack, stderr);
        return 1;
    }

    const wchar_t* input_dir   = argv[2];
    const wchar_t* output_path = argv[3];

    // ---- option parse -----------------------------------------------------
    bool           opt_encrypt  = false;
    bool           opt_compress = false;
    const wchar_t* opt_key      = nullptr;   // --key <hex64>      (生鍵)
    const wchar_t* opt_key_file = nullptr;   // --key-file <path>  (推奨)

    for (usize i = 4; i < argv.Num(); ++i) {
        const wchar_t* a = argv[i];
        if      (EqualsW(a, L"--encrypt"))  opt_encrypt  = true;
        else if (EqualsW(a, L"--compress")) opt_compress = true;
        else if (EqualsW(a, L"--key")) {
            if (i + 1 >= argv.Num()) {
                std::fputs("pack: --key requires hex argument\n", stderr);
                return 1;
            }
            opt_key = argv[++i];
        } else if (EqualsW(a, L"--key-file")) {
            if (i + 1 >= argv.Num()) {
                std::fputs("pack: --key-file requires a path argument\n", stderr);
                return 1;
            }
            opt_key_file = argv[++i];
        } else {
            char u8buf[256];
            Utf16ToUtf8(a, u8buf, sizeof(u8buf));
            std::fprintf(stderr, "pack: unknown option '%s'\n", u8buf);
            return 1;
        }
    }

    if (opt_key != nullptr && opt_key_file != nullptr) {
        std::fputs("pack: use either --key or --key-file, not both\n", stderr);
        return 1;
    }
    if (opt_encrypt && opt_key == nullptr && opt_key_file == nullptr) {
        std::fputs("pack: --encrypt requires --key-file <path> (or --key <hex64>)\n", stderr);
        return 1;
    }

    // 鍵を 32-byte に解決する (encrypted 時のみ使う)。
    FAcpakKey enc_key{};
    bool      have_key = false;
    if (opt_key_file != nullptr) {
        char kerr[256];
        if (!ReadKeyFile(opt_key_file, enc_key.bytes, kerr, sizeof(kerr))) {
            std::fprintf(stderr, "pack: %s\n", kerr);
            return 1;
        }
        have_key = true;
    } else if (opt_key != nullptr) {
        if (!ParseHex256(opt_key, enc_key.bytes)) {
            std::fputs("pack: --key must be 64 hex chars (256-bit)\n", stderr);
            return 1;
        }
        have_key = true;
    }

    // ---- 入力 dir 存在チェック -------------------------------------------
    if (!FFileSystem::DirectoryExists(input_dir)) {
        char u8buf[1024];
        Utf16ToUtf8(input_dir, u8buf, sizeof(u8buf));
        std::fprintf(stderr, "pack: input_dir does not exist: %s\n", u8buf);
        return 2;
    }

    // ---- フラグ合成 -------------------------------------------------------
    u32 raw_flags = static_cast<u32>(AcpakFlagNone);
    if (opt_encrypt)  raw_flags |= static_cast<u32>(AcpakFlagEncrypted);
    if (opt_compress) raw_flags |= static_cast<u32>(AcpakFlagCompressed);
    const EAcpakFlags flags = static_cast<EAcpakFlags>(raw_flags);

    // ---- 再帰スキャン → blob 配列に展開 ----------------------------------
    TArray<FPackedBlob> blobs;
    {
        auto sr = ScanDirRecursive(input_dir, L"", blobs);
        if (sr.IsErr()) {
            PrintError(sr.Error(), "pack: scan");
            return 2;
        }
    }

    if (blobs.IsEmpty()) {
        std::fputs("pack: input_dir contains no files\n", stderr);
        return 2;
    }
    SortPackedBlobs(blobs);

    // ---- Writer 起動 ------------------------------------------------------
    FAcpakWriter writer;
    // 暗号化鍵は Open より前に設定する (Writer の契約)。flags に Encrypted が
    // 立っていなければ無視される。
    if (have_key) {
        writer.SetKey(enc_key);
    }
    {
        auto r = writer.Open(output_path, flags);
        if (r.IsErr()) {
            PrintError(r.Error(), "pack: FAcpakWriter::Open");
            return 2;
        }
    }

    u64 input_bytes_total = 0;
    for (usize i = 0; i < blobs.Num(); ++i) {
        const FPackedBlob& b = blobs[i];
        input_bytes_total += static_cast<u64>(b.bytes.Num());
        auto r = writer.AddFile(b.path.GetData(),
                                b.bytes.GetData(),
                                static_cast<u64>(b.bytes.Num()));
        if (r.IsErr()) {
            PrintError(r.Error(), "pack: FAcpakWriter::AddFile");
            return 2;
        }
    }

    {
        auto r = writer.Finalize();
        if (r.IsErr()) {
            PrintError(r.Error(), "pack: FAcpakWriter::Finalize");
            return 2;
        }
    }
    writer.Close();

    // ---- 出力ファイルサイズを取って報告 ------------------------------------
    u64 output_size = 0;
    {
        auto sr = FFileSystem::FileSize(output_path);
        if (sr.IsOk()) output_size = sr.Value();
    }

    char path_u8[1024];
    Utf16ToUtf8(output_path, path_u8, sizeof(path_u8));
    char in_str[64];
    char out_str[64];
    FormatBytes(input_bytes_total, in_str, sizeof(in_str));
    FormatBytes(output_size,       out_str, sizeof(out_str));
    std::printf("Packed %llu files (%s -> %s) into %s\n",
                static_cast<unsigned long long>(blobs.Num()),
                in_str, out_str, path_u8);
    if (opt_encrypt || opt_compress) {
        std::printf("  flags: %s%s\n",
                    opt_encrypt  ? "encrypted(AES-256-GCM) " : "",
                    opt_compress ? "compressed(LZ4) " : "");
    }
    return 0;
}

// ============================================================================
// unpack — Reader 開く → 各 entry を ReadFile → output_dir/<path> に書き出す
// ============================================================================
int CmdUnpack(const TArray<wchar_t*>& argv) noexcept {
    if (argv.Num() < 4) {
        std::fputs(kUsageUnpack, stderr);
        return 1;
    }

    const wchar_t* input_path = argv[2];
    const wchar_t* output_dir = argv[3];

    // --key <hex64> / --key-file <path> を解決する (暗号化 pak の復号用)。
    FAcpakKey dec_key{};
    bool      have_key = false;
    for (usize i = 4; i < argv.Num(); ++i) {
        const wchar_t* a = argv[i];
        if (EqualsW(a, L"--key")) {
            if (i + 1 >= argv.Num()) {
                std::fputs("unpack: --key requires hex argument\n", stderr);
                return 1;
            }
            if (!ParseHex256(argv[++i], dec_key.bytes)) {
                std::fputs("unpack: --key must be 64 hex chars (256-bit)\n", stderr);
                return 1;
            }
            have_key = true;
        } else if (EqualsW(a, L"--key-file")) {
            if (i + 1 >= argv.Num()) {
                std::fputs("unpack: --key-file requires a path argument\n", stderr);
                return 1;
            }
            char kerr[256];
            if (!ReadKeyFile(argv[++i], dec_key.bytes, kerr, sizeof(kerr))) {
                std::fprintf(stderr, "unpack: %s\n", kerr);
                return 1;
            }
            have_key = true;
        } else {
            char u8buf[256];
            Utf16ToUtf8(a, u8buf, sizeof(u8buf));
            std::fprintf(stderr, "unpack: unknown option '%s'\n", u8buf);
            return 1;
        }
    }

    FAcpakReader reader;
    if (have_key) {
        reader.SetKey(dec_key);
    }
    {
        auto r = reader.Open(input_path);
        if (r.IsErr()) {
            PrintError(r.Error(), "unpack: FAcpakReader::Open");
            return 2;
        }
    }

    // 出力 dir 作成
    {
        auto r = FFileSystem::CreateDirectory(output_dir);
        if (r.IsErr()) {
            PrintError(r.Error(), "unpack: CreateDirectory");
            return 2;
        }
    }

    const u32 count = reader.FileCount();
    u64       total_bytes = 0;

    for (u32 i = 0; i < count; ++i) {
        const FAcpakFileEntry* e = reader.GetEntry(i);
        if (e == nullptr) {
            std::fprintf(stderr, "unpack: GetEntry(%u) returned null\n", i);
            return 2;
        }

        // 出力先パス組み立て
        wchar_t dst_path[4096];
        if (!JoinPath(output_dir, e->path, dst_path,
                      sizeof(dst_path) / sizeof(dst_path[0]))) {
            std::fputs("unpack: dst path too long\n", stderr);
            return 2;
        }

        // 親 dir を再帰作成 (path 内の '/' / '\\' を区切りに dir を順次作る)
        // 出力 dir 配下の path しかない前提なので、末尾セグメントを除いた部分を
        // CreateDirectory に渡す。
        {
            wchar_t parent[4096];
            usize n = StrLenW(dst_path);
            if (n >= sizeof(parent) / sizeof(parent[0])) {
                std::fputs("unpack: parent path too long\n", stderr);
                return 2;
            }
            MemCopy(parent, dst_path, (n + 1) * sizeof(wchar_t));
            // 末尾セグメントを切り落とす
            for (usize k = n; k > 0; --k) {
                if (parent[k - 1] == L'\\' || parent[k - 1] == L'/') {
                    parent[k - 1] = L'\0';
                    break;
                }
            }
            if (parent[0] != L'\0') {
                auto r = FFileSystem::CreateDirectory(parent);
                if (r.IsErr()) {
                    PrintError(r.Error(), "unpack: CreateDirectory (parent)");
                    return 2;
                }
            }
        }

        // entry のバイト数 → buffer 確保 → ReadFile
        TArray<byte> buf;
        buf.SetNum(static_cast<usize>(e->size_uncompressed));
        if (e->size_uncompressed > 0) {
            auto rr = reader.ReadFile(e->path, buf.GetData(), e->size_uncompressed);
            if (rr.IsErr()) {
                PrintError(rr.Error(), "unpack: ReadFile");
                return 2;
            }
        }

        // 書き出し
        auto wr = FFileSystem::WriteAllBytes(dst_path, buf.GetData(), buf.Num());
        if (wr.IsErr()) {
            PrintError(wr.Error(), "unpack: WriteAllBytes");
            return 2;
        }
        total_bytes += e->size_uncompressed;
    }

    reader.Close();

    char size_str[64];
    FormatBytes(total_bytes, size_str, sizeof(size_str));
    std::printf("Unpacked %u files (%s)\n",
                static_cast<unsigned>(count), size_str);
    return 0;
}

// ============================================================================
// list — entry 一覧 (path / size / crc32)
// ============================================================================
int CmdList(const TArray<wchar_t*>& argv) noexcept {
    if (argv.Num() < 3) {
        std::fputs(kUsageList, stderr);
        return 1;
    }
    const wchar_t* input_path = argv[2];

    FAcpakReader reader;
    {
        auto r = reader.Open(input_path);
        if (r.IsErr()) {
            PrintError(r.Error(), "list: FAcpakReader::Open");
            return 2;
        }
    }

    const u32 count = reader.FileCount();
    std::printf("%-12s %-10s %s\n", "crc32", "size", "path");
    std::printf("---------------------------------------------------------------\n");

    for (u32 i = 0; i < count; ++i) {
        const FAcpakFileEntry* e = reader.GetEntry(i);
        if (e == nullptr) continue;

        char path_u8[1024];
        Utf16ToUtf8(e->path, path_u8, sizeof(path_u8));
        std::printf("0x%08X   %10llu  %s\n",
                    static_cast<unsigned>(e->crc32),
                    static_cast<unsigned long long>(e->size_uncompressed),
                    path_u8);
    }
    std::printf("---------------------------------------------------------------\n");
    std::printf("%u entries\n", static_cast<unsigned>(count));
    reader.Close();
    return 0;
}

// ============================================================================
// verify — 全 entry を ReadFile して CRC32 検証 (失敗があれば exit 2)
// ============================================================================
int CmdVerify(const TArray<wchar_t*>& argv) noexcept {
    if (argv.Num() < 3) {
        std::fputs(kUsageVerify, stderr);
        return 1;
    }
    const wchar_t* input_path = argv[2];

    // --key <hex64> / --key-file <path> を解決する (暗号化 pak の検証用)。
    FAcpakKey dec_key{};
    bool      have_key = false;
    for (usize i = 3; i < argv.Num(); ++i) {
        const wchar_t* a = argv[i];
        if (EqualsW(a, L"--key")) {
            if (i + 1 >= argv.Num()) {
                std::fputs("verify: --key requires hex argument\n", stderr);
                return 1;
            }
            if (!ParseHex256(argv[++i], dec_key.bytes)) {
                std::fputs("verify: --key must be 64 hex chars (256-bit)\n", stderr);
                return 1;
            }
            have_key = true;
        } else if (EqualsW(a, L"--key-file")) {
            if (i + 1 >= argv.Num()) {
                std::fputs("verify: --key-file requires a path argument\n", stderr);
                return 1;
            }
            char kerr[256];
            if (!ReadKeyFile(argv[++i], dec_key.bytes, kerr, sizeof(kerr))) {
                std::fprintf(stderr, "verify: %s\n", kerr);
                return 1;
            }
            have_key = true;
        } else {
            char u8buf[256];
            Utf16ToUtf8(a, u8buf, sizeof(u8buf));
            std::fprintf(stderr, "verify: unknown option '%s'\n", u8buf);
            return 1;
        }
    }

    FAcpakReader reader;
    if (have_key) {
        reader.SetKey(dec_key);
    }
    {
        auto r = reader.Open(input_path);
        if (r.IsErr()) {
            PrintError(r.Error(), "verify: FAcpakReader::Open");
            return 2;
        }
    }

    const u32 count = reader.FileCount();
    u32       ok_count = 0;
    u32       fail_count = 0;

    for (u32 i = 0; i < count; ++i) {
        const FAcpakFileEntry* e = reader.GetEntry(i);
        if (e == nullptr) {
            ++fail_count;
            continue;
        }
        // 0 バイトエントリは ReadFile を呼ばない。Resize(0) で Data() が nullptr に
        // なり、ReadFile(path, nullptr, 0) が IO エラーを返すため (unpack も同じ
        // ガードを持つ)。空ファイルは中身が無く検証対象も無いので ok 扱い。
        if (e->size_uncompressed == 0) {
            ++ok_count;
            continue;
        }
        TArray<byte> buf;
        buf.SetNum(static_cast<usize>(e->size_uncompressed));
        auto rr = reader.ReadFile(e->path, buf.GetData(), e->size_uncompressed);
        if (rr.IsErr()) {
            char path_u8[1024];
            Utf16ToUtf8(e->path, path_u8, sizeof(path_u8));
            std::fprintf(stderr, "  FAIL %s (subcode=%u)\n",
                         path_u8,
                         static_cast<unsigned>(rr.Error().subcode));
            ++fail_count;
        } else {
            ++ok_count;
        }
    }

    reader.Close();
    std::printf("verify: %u ok, %u failed (of %u entries)\n",
                static_cast<unsigned>(ok_count),
                static_cast<unsigned>(fail_count),
                static_cast<unsigned>(count));
    return fail_count == 0 ? 0 : 2;
}

// ============================================================================
// info — header 情報のみ表示 (open 中に Reader が parse 済)
// ============================================================================
int CmdInfo(const TArray<wchar_t*>& argv) noexcept {
    if (argv.Num() < 3) {
        std::fputs(kUsageInfo, stderr);
        return 1;
    }
    const wchar_t* input_path = argv[2];

    // file_size はヘッダ parse の前に取れる
    u64 file_size = 0;
    {
        auto sr = FFileSystem::FileSize(input_path);
        if (sr.IsErr()) {
            PrintError(sr.Error(), "info: FileSize");
            return 2;
        }
        file_size = sr.Value();
    }

    FAcpakReader reader;
    {
        auto r = reader.Open(input_path);
        if (r.IsErr()) {
            PrintError(r.Error(), "info: FAcpakReader::Open");
            return 2;
        }
    }

    char path_u8[1024];
    Utf16ToUtf8(input_path, path_u8, sizeof(path_u8));
    char size_str[64];
    FormatBytes(file_size, size_str, sizeof(size_str));

    const u32 flags = reader.Flags();
    std::printf("file       : %s\n", path_u8);
    std::printf("file size  : %s (%llu bytes)\n", size_str,
                static_cast<unsigned long long>(file_size));
    std::printf("magic      : ACPAK\\0\\0\\0\n");
    std::printf("version    : %u\n", static_cast<unsigned>(kAcpakVersion));
    std::printf("flags      : 0x%08X (%s%s%s)\n",
                static_cast<unsigned>(flags),
                (flags & static_cast<u32>(AcpakFlagEncrypted))  ? "Encrypted " : "",
                (flags & static_cast<u32>(AcpakFlagCompressed)) ? "Compressed " : "",
                flags == 0 ? "None" : "");
    std::printf("file count : %u\n", static_cast<unsigned>(reader.FileCount()));

    reader.Close();
    return 0;
}

// ============================================================================
// help — 全体 usage または specific subcommand の usage
// ============================================================================
int CmdHelp(const TArray<wchar_t*>& argv) noexcept {
    if (argv.Num() < 3) {
        std::fputs(kUsageMain, stdout);
        return 0;
    }
    const wchar_t* sub = argv[2];
    if      (EqualsW(sub, L"pack"))   std::fputs(kUsagePack,   stdout);
    else if (EqualsW(sub, L"unpack")) std::fputs(kUsageUnpack, stdout);
    else if (EqualsW(sub, L"list"))   std::fputs(kUsageList,   stdout);
    else if (EqualsW(sub, L"verify")) std::fputs(kUsageVerify, stdout);
    else if (EqualsW(sub, L"info"))   std::fputs(kUsageInfo,   stdout);
    else if (EqualsW(sub, L"help"))   std::fputs(kUsageMain,   stdout);
    else {
        char u8buf[64];
        Utf16ToUtf8(sub, u8buf, sizeof(u8buf));
        std::fprintf(stderr, "help: unknown subcommand '%s'\n", u8buf);
        std::fputs(kUsageMain, stderr);
        return 1;
    }
    return 0;
}

} // namespace

// ============================================================================
// main — UTF-16 引数を CommandLineToArgvW で取って subcommand dispatch
// ============================================================================
int main(int /*argc*/, char** /*argv*/) {
    // UTF-8 出力に統一 (Windows 既定の Shift_JIS を避ける)。
    // 失敗しても致命ではない (英語 ASCII 範囲は普通に出るので無視)。
    ::SetConsoleOutputCP(CP_UTF8);

    FWideArgs wa;
    if (!BuildWideArgs(wa)) {
        std::fputs("acs_assetpack: failed to parse command line\n", stderr);
        return 1;
    }

    if (wa.argc < 2) {
        std::fputs(kUsageMain, stdout);
        return 0;
    }

    const wchar_t* sub = wa.argv[1];

    if      (EqualsW(sub, L"pack"))   return CmdPack(wa.argv);
    else if (EqualsW(sub, L"unpack")) return CmdUnpack(wa.argv);
    else if (EqualsW(sub, L"list"))   return CmdList(wa.argv);
    else if (EqualsW(sub, L"verify")) return CmdVerify(wa.argv);
    else if (EqualsW(sub, L"info"))   return CmdInfo(wa.argv);
    else if (EqualsW(sub, L"help") ||
             EqualsW(sub, L"--help") ||
             EqualsW(sub, L"-h"))     return CmdHelp(wa.argv);

    char u8buf[256];
    Utf16ToUtf8(sub, u8buf, sizeof(u8buf));
    std::fprintf(stderr, "acs_assetpack: unknown subcommand '%s'\n", u8buf);
    std::fputs(kUsageMain, stderr);
    return 1;
}
