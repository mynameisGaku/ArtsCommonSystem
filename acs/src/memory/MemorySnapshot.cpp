// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — FMemorySnapshot 実装
// -----------------------------------------------------------------------------
// メモリ使用状況をスナップショットとして可視化する。
// 出力形式: SVG (ブラウザで開ける、ラベル付き) / BMP (24bpp 無圧縮) / stdout
// =============================================================================
#include "memory/MemorySnapshot.h"
#include "memory/MemorySystem.h"
#include "foundation/Platform.h"

#include <cstdio>
#include <cstring>

namespace acs {

namespace {

/** RGB の 3 バイト色 (描画/出力共通)。 */
struct RgbColor { u8 r, g, b; };

/** セグメントごとの固定色 (バーの色だけで識別できるよう割り当て)。 */
constexpr RgbColor kSegmentColor[(usize)ESegment::_Count] = {
    /* Default   */ {  60, 120, 220 },
    /* Permanent */ { 110, 200, 110 },
    /* Temp      */ { 240, 180,  60 },
    /* Resource  */ { 200,  80, 180 },
    /* Develop   */ { 160, 160, 160 },
};

/**
 * バッファ全体をファイルハンドルへ書き出す。
 *
 * @param h 書き込み先ファイルハンドル。
 * @param p 書き込むデータ。
 * @param n バイト数。
 */
ACS_FORCEINLINE void WriteAll(HANDLE h, const void* p, usize n) noexcept {
    DWORD wrote = 0;
    ::WriteFile(h, p, static_cast<DWORD>(n), &wrote, nullptr);
}

/**
 * バイト数を "12.3 MB" 形式の文字列に変換する。
 *
 * @param buf 出力先バッファ。
 * @param cap buf の容量。
 * @param b バイト数。
 * @return snprintf が返した書き込み文字数。
 */
int FormatBytes(char* buf, usize cap, u64 b) noexcept {
    const char* unit = "B";
    double v = static_cast<double>(b);
    if (v >= 1024.0) { v /= 1024.0; unit = "KB"; }
    if (v >= 1024.0) { v /= 1024.0; unit = "MB"; }
    if (v >= 1024.0) { v /= 1024.0; unit = "GB"; }
    return ::snprintf(buf, cap, "%.1f %s", v, unit);
}

} // namespace

// SVG 出力: 各セグメントを 1 行のバーとして描画
TResult<void> FMemorySnapshot::WriteSvg(const wchar_t* path, u32 width, u32 row_height) noexcept {
    SegmentStats stats[(usize)ESegment::_Count];
    const u32 n = FMemorySystem::GetStats(stats, (u32)ESegment::_Count);
    if (n == 0) return ACS_ERR(Memory, 40, "FMemorySystem has no segments");

    // ファイルを上書き作成
    const HANDLE h = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return ACS_ERR_OS(IO, 1, "CreateFileW failed (svg)", ::GetLastError());

    char buf[1024];
    int len;

    // SVG ルート + スタイル + タイトル
    const u32 height = (n + 1) * row_height + 40;
    len = ::snprintf(buf, sizeof(buf),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%u\" height=\"%u\" viewBox=\"0 0 %u %u\">\n"
        "<style>text{font-family:monospace;font-size:14px;fill:#222;}.lbl{font-size:11px;fill:#fff;}</style>\n"
        "<rect width=\"100%%\" height=\"100%%\" fill=\"#f7f7fa\"/>\n"
        "<text x=\"10\" y=\"22\" font-weight=\"bold\">ACS Memory Snapshot</text>\n",
        width, height, width, height);
    WriteAll(h, buf, len);

    const u32 bar_x = 120;
    // width が小さいと width - bar_x - 10 が u32 アンダーフローして巨大値になる。下限ガード。
    const u32 bar_w = (width > bar_x + 10) ? (width - bar_x - 10) : 0;

    for (u32 i = 0; i < n; ++i) {
        const SegmentStats& s = stats[i];
        const u32 y = 30 + i * row_height;

        // セグメント名ラベル
        len = ::snprintf(buf, sizeof(buf),
            "<text x=\"10\" y=\"%u\">%s</text>\n",
            y + row_height / 2 + 5, s.name);
        WriteAll(h, buf, len);

        // 背景バー（予約全体、グレー）
        len = ::snprintf(buf, sizeof(buf),
            "<rect x=\"%u\" y=\"%u\" width=\"%u\" height=\"%u\" fill=\"#ddd\"/>\n",
            bar_x, y, bar_w, row_height - 4);
        WriteAll(h, buf, len);

        // 使用量バー（カラー、使用率に比例した幅）
        u32 used_w = s.reserve > 0
            ? static_cast<u32>((u64)bar_w * s.used / s.reserve)
            : 0;
        if (used_w > bar_w) used_w = bar_w;
        const RgbColor& c = kSegmentColor[(usize)s.segment];
        len = ::snprintf(buf, sizeof(buf),
            "<rect x=\"%u\" y=\"%u\" width=\"%u\" height=\"%u\" fill=\"rgb(%u,%u,%u)\"/>\n",
            bar_x, y, used_w, row_height - 4, c.r, c.g, c.b);
        WriteAll(h, buf, len);

        // ピーク位置を赤い縦線で示す
        const u32 peak_x = s.reserve > 0
            ? bar_x + static_cast<u32>((u64)bar_w * s.peak / s.reserve)
            : bar_x;
        len = ::snprintf(buf, sizeof(buf),
            "<line x1=\"%u\" y1=\"%u\" x2=\"%u\" y2=\"%u\" stroke=\"#c00\" stroke-width=\"2\"/>\n",
            peak_x, y, peak_x, y + row_height - 4);
        WriteAll(h, buf, len);

        // バー右端に「使用 / 予約」を表示
        char used_s[32], reserve_s[32];
        FormatBytes(used_s, sizeof(used_s), s.used);
        FormatBytes(reserve_s, sizeof(reserve_s), s.reserve);
        len = ::snprintf(buf, sizeof(buf),
            "<text x=\"%u\" y=\"%u\" text-anchor=\"end\" class=\"lbl\" fill=\"#222\">%s / %s</text>\n",
            bar_x + bar_w - 4, y + row_height / 2 + 4, used_s, reserve_s);
        WriteAll(h, buf, len);
    }

    const char* const footer = "</svg>\n";
    WriteAll(h, footer, ::strlen(footer));
    ::CloseHandle(h);
    return Ok();
}

namespace {

// packed=1 でコンパイラの詰め物を抑制（BMP ヘッダ仕様通りに並べる）
#pragma pack(push, 1)
/** BMP ファイルヘッダ (14 バイト、仕様通りに packed)。 */
struct BmpFileHeader {
    /** マジック 'BM' (0x4D42)。 */
    u16 type;

    /** ファイル全体のバイト数。 */
    u32 size;

    /** 予約 1 (0)。 */
    u16 res1;

    /** 予約 2 (0)。 */
    u16 res2;

    /** ピクセル配列の開始オフセット。 */
    u32 offbits;
};

/** BMP 情報ヘッダ (BITMAPINFOHEADER、40 バイト)。 */
struct BmpInfoHeader {
    /** この構造体のサイズ。 */
    u32 size;

    /** 画像の幅 (ピクセル)。 */
    i32 width;

    /** 画像の高さ (正なら下→上、負なら上→下スキャン)。 */
    i32 height;

    /** プレーン数 (常に 1)。 */
    u16 planes;

    /** 1 ピクセルあたりのビット数 (24 = RGB)。 */
    u16 bitcount;

    /** 圧縮方式 (0 = 無圧縮)。 */
    u32 compression;

    /** ピクセル配列のバイト数。 */
    u32 sizeImage;

    /** 水平解像度 (ppm、未使用)。 */
    i32 xpels;

    /** 垂直解像度 (ppm、未使用)。 */
    i32 ypels;

    /** 使用パレット色数 (未使用)。 */
    u32 clrUsed;

    /** 重要パレット色数 (未使用)。 */
    u32 clrImp;
};
#pragma pack(pop)

/**
 * ピクセル行の x 番目に色を書き込む (BMP は 24bpp BGR バイト順)。
 *
 * @param row ピクセル行の先頭。
 * @param x 行内のピクセル位置。
 * @param c 書き込む色。
 */
ACS_FORCEINLINE void PutPixel(u8* row, u32 x, RgbColor c) noexcept {
    row[x * 3 + 0] = c.b;
    row[x * 3 + 1] = c.g;
    row[x * 3 + 2] = c.r;
}

} // namespace

TResult<void> FMemorySnapshot::WriteBmp(const wchar_t* path, u32 width, u32 row_height) noexcept {
    SegmentStats stats[(usize)ESegment::_Count];
    const u32 n = FMemorySystem::GetStats(stats, (u32)ESegment::_Count);
    if (n == 0) return ACS_ERR(Memory, 41, "FMemorySystem has no segments");

    const u32 height = n * row_height;
    // BMP は 1 行を 4 バイト境界にアラインする
    const u32 stride = (width * 3 + 3) & ~3u;
    const u32 pixel_bytes = stride * height;

    // ヘッダ初期化
    BmpFileHeader fh {};
    BmpInfoHeader ih {};
    fh.type = 0x4D42;
    fh.offbits = sizeof(BmpFileHeader) + sizeof(BmpInfoHeader);
    fh.size = fh.offbits + pixel_bytes;

    ih.size = sizeof(BmpInfoHeader);
    ih.width = static_cast<i32>(width);
    ih.height = -static_cast<i32>(height);  // 負: 上→下スキャン
    ih.planes = 1;
    ih.bitcount = 24;
    ih.compression = 0;
    ih.sizeImage = pixel_bytes;

    // ピクセルバッファをゼロクリア確保
    u8* const pixels = static_cast<u8*>(::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, pixel_bytes));
    if (!pixels) return ACS_ERR(Memory, 42, "BMP pixel buffer alloc failed");

    // 全体を浅いグレー (#F7F7F7) で塗る
    ::memset(pixels, 0xF7, pixel_bytes);

    // 各セグメントのバーを描画
    for (u32 i = 0; i < n; ++i) {
        const SegmentStats& s = stats[i];
        const RgbColor& c = kSegmentColor[(usize)s.segment];
        const u32 y_start = i * row_height;
        const u32 y_end   = (i + 1) * row_height - 2;
        u32 used_w  = s.reserve > 0
                     ? static_cast<u32>((u64)width * s.used / s.reserve)
                     : 0;
        const u32 reserve_w = width;
        if (used_w > reserve_w) used_w = reserve_w;

        for (u32 y = y_start; y < y_end; ++y) {
            u8* const row = pixels + y * stride;
            // 背景色 → 使用量カラーの順に塗る
            for (u32 x = 0; x < reserve_w; ++x) PutPixel(row, x, RgbColor{0xDD, 0xDD, 0xDD});
            for (u32 x = 0; x < used_w; ++x)    PutPixel(row, x, c);
        }
    }

    // ファイル出力
    const HANDLE h = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        ::HeapFree(::GetProcessHeap(), 0, pixels);
        return ACS_ERR_OS(IO, 2, "CreateFileW failed (bmp)", ::GetLastError());
    }
    WriteAll(h, &fh, sizeof(fh));
    WriteAll(h, &ih, sizeof(ih));
    WriteAll(h, pixels, pixel_bytes);
    ::CloseHandle(h);
    ::HeapFree(::GetProcessHeap(), 0, pixels);
    return Ok();
}

// stdout への簡易テキスト表（CI ログ・コンソール確認用）
void FMemorySnapshot::DumpToStdOut() noexcept {
    SegmentStats stats[(usize)ESegment::_Count];
    const u32 n = FMemorySystem::GetStats(stats, (u32)ESegment::_Count);
    ::printf("[ACS Memory Snapshot]\n");
    ::printf("  %-12s | %-12s | %-12s | %-12s | %-12s\n",
             "ESegment", "Reserve", "Committed", "Used", "Peak");
    ::printf("  %s\n", "-----------------------------------------------------------------------------");
    for (u32 i = 0; i < n; ++i) {
        const SegmentStats& s = stats[i];
        char r[16], c[16], u[16], p[16];
        FormatBytes(r, sizeof(r), s.reserve);
        FormatBytes(c, sizeof(c), s.committed);
        FormatBytes(u, sizeof(u), s.used);
        FormatBytes(p, sizeof(p), s.peak);
        ::printf("  %-12s | %-12s | %-12s | %-12s | %-12s\n",
                 s.name, r, c, u, p);
    }
}

} // namespace acs
