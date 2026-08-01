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
struct FRgbColor {
    u8 r, g, b;
};

/** セグメントごとの固定色 (バーの色だけで識別できるよう割り当て)。 */
constexpr FRgbColor kSegmentColor[(usize)ESegment::_Count] = {
    /* Default   */ {60, 120, 220},
    /* Permanent */ {110, 200, 110},
    /* Temp      */ {240, 180, 60},
    /* Resource  */ {200, 80, 180},
    /* Develop   */ {160, 160, 160},
};

/**
 * バッファ全体をファイルハンドルへ書き出す。
 *
 * @param FileHandle 書き込み先ファイルハンドル。
 * @param Data 書き込むデータ。
 * @param Size バイト数。
 */
bool WriteAll(HANDLE FileHandle, const void* Data, usize Size) noexcept
{
    if (!Data && Size != 0u) return false;

    const u8* Cursor = static_cast<const u8*>(Data);
    usize RemainingSize = Size;
    while (RemainingSize != 0u) {
        const DWORD ChunkSize = RemainingSize > static_cast<usize>(MAXDWORD)
                                    ? MAXDWORD
                                    : static_cast<DWORD>(RemainingSize);
        DWORD BytesWritten = 0u;
        if (!::WriteFile(FileHandle, Cursor, ChunkSize, &BytesWritten, nullptr) || BytesWritten == 0u) {
            return false;
        }
        Cursor += BytesWritten;
        RemainingSize -= BytesWritten;
    }
    return true;
}

/** snprintf の結果を検証してからファイルへ書き込む。 */
bool WriteFormatted(HANDLE FileHandle, const char* Buffer, usize Capacity, int Length) noexcept
{
    if (Length < 0 || static_cast<usize>(Length) >= Capacity) return false;
    return WriteAll(FileHandle, Buffer, static_cast<usize>(Length));
}

/** 64bit 統計値から描画幅をオーバーフローなしで求める。 */
u32 CalculateBarWidth(u32 Width, u64 UsedBytes, u64 CapacityBytes) noexcept
{
    if (Width == 0u || UsedBytes == 0u) return 0u;
    if (CapacityBytes == 0u || UsedBytes >= CapacityBytes) return Width;

    const double Ratio = static_cast<double>(UsedBytes) / static_cast<double>(CapacityBytes);
    const double ScaledWidth = Ratio * static_cast<double>(Width);
    return ScaledWidth >= static_cast<double>(Width) ? Width : static_cast<u32>(ScaledWidth);
}

/**
 * バイト数を "12.3 MB" 形式の文字列に変換する。
 *
 * @param Buffer 出力先バッファ。
 * @param Capacity Buffer の容量。
 * @param Bytes バイト数。
 * @return snprintf が返した書き込み文字数。
 */
int FormatBytes(char* Buffer, usize Capacity, u64 Bytes) noexcept
{
    const char* Unit = "B";
    double Value = static_cast<double>(Bytes);
    if (Value >= 1024.0) {
        Value /= 1024.0;
        Unit = "KB";
    }
    if (Value >= 1024.0) {
        Value /= 1024.0;
        Unit = "MB";
    }
    if (Value >= 1024.0) {
        Value /= 1024.0;
        Unit = "GB";
    }
    return ::snprintf(Buffer, Capacity, "%.1f %s", Value, Unit);
}

} // namespace

// SVG 出力: 各セグメントを 1 行のバーとして描画
TResult<void> FMemorySnapshot::WriteSvg(const wchar_t* Path, u32 Width, u32 RowHeight) noexcept
{
    if (!Path || Path[0] == L'\0' || Width == 0u || RowHeight < 4u) {
        return ACS_ERR(Memory, 43, "invalid SVG dimensions or path");
    }

    FSegmentStats StatisticsArray[(usize)ESegment::_Count];
    const u32 SegmentCount = CMemorySystem::GetStats(StatisticsArray, (u32)ESegment::_Count);
    if (SegmentCount == 0) return ACS_ERR(Memory, 40, "CMemorySystem has no segments");
    if (SegmentCount + 1u > ((~u32(0)) - 40u) / RowHeight) {
        return ACS_ERR(Memory, 43, "SVG dimensions overflow");
    }

    // ファイルを上書き作成
    const HANDLE FileHandle = ::CreateFileW(Path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                            nullptr);
    if (FileHandle == INVALID_HANDLE_VALUE) return ACS_ERR_OS(IO, 1, "CreateFileW failed (svg)", ::GetLastError());

    char Buffer[1024];
    int Length;

    // SVG ルート + スタイル + タイトル
    const u32 Height = (SegmentCount + 1) * RowHeight + 40;
    Length = ::snprintf(
        Buffer, sizeof(Buffer),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%u\" height=\"%u\" viewBox=\"0 0 %u %u\">\n"
        "<style>text{font-family:monospace;font-size:14px;fill:#222;}.lbl{font-size:11px;fill:#fff;}</style>\n"
        "<rect width=\"100%%\" height=\"100%%\" fill=\"#f7f7fa\"/>\n"
        "<text x=\"10\" y=\"22\" font-weight=\"bold\">ACS Memory Snapshot</text>\n",
        Width, Height, Width, Height);
    if (!WriteFormatted(FileHandle, Buffer, sizeof(Buffer), Length)) {
        ::CloseHandle(FileHandle);
        return ACS_ERR_OS(IO, 3, "WriteFile failed (svg header)", ::GetLastError());
    }

    const u32 BarX = 120;
    // Width が小さいと Width - BarX - 10 が u32 アンダーフローして巨大値になる。下限ガード。
    const u32 BarWidth = (Width > BarX + 10) ? (Width - BarX - 10) : 0;

    for (u32 i = 0; i < SegmentCount; ++i) {
        const FSegmentStats& Statistics = StatisticsArray[i];
        const u32 y = 30 + i * RowHeight;

        // セグメント名ラベル
        Length = ::snprintf(Buffer, sizeof(Buffer), "<text x=\"10\" y=\"%u\">%s</text>\n", y + RowHeight / 2 + 5,
                            Statistics.segment_name);
        if (!WriteFormatted(FileHandle, Buffer, sizeof(Buffer), Length)) {
            ::CloseHandle(FileHandle);
            return ACS_ERR_OS(IO, 3, "WriteFile failed (svg label)", ::GetLastError());
        }

        // 背景バー（予約全体、グレー）
        Length = ::snprintf(Buffer, sizeof(Buffer),
                            "<rect x=\"%u\" y=\"%u\" width=\"%u\" height=\"%u\" fill=\"#ddd\"/>\n", BarX, y, BarWidth,
                            RowHeight - 4);
        if (!WriteFormatted(FileHandle, Buffer, sizeof(Buffer), Length)) {
            ::CloseHandle(FileHandle);
            return ACS_ERR_OS(IO, 3, "WriteFile failed (svg background)", ::GetLastError());
        }

        // 使用量バー（カラー、使用率に比例した幅）
        const u64 BarCapacity = Statistics.hard_budget_bytes > 0
                                    ? Statistics.hard_budget_bytes
                                    : (Statistics.peak_requested_bytes > 0 ? Statistics.peak_requested_bytes : 1);
        const u32 UsedWidth = CalculateBarWidth(BarWidth, Statistics.requested_bytes, BarCapacity);
        const FRgbColor& Color = kSegmentColor[(usize)Statistics.segment];
        Length = ::snprintf(Buffer, sizeof(Buffer),
                            "<rect x=\"%u\" y=\"%u\" width=\"%u\" height=\"%u\" fill=\"rgb(%u,%u,%u)\"/>\n", BarX, y,
                            UsedWidth, RowHeight - 4, Color.r, Color.g, Color.b);
        if (!WriteFormatted(FileHandle, Buffer, sizeof(Buffer), Length)) {
            ::CloseHandle(FileHandle);
            return ACS_ERR_OS(IO, 3, "WriteFile failed (svg usage)", ::GetLastError());
        }

        // ピーク位置を赤い縦線で示す
        const u32 PeakOffset = CalculateBarWidth(BarWidth, Statistics.peak_requested_bytes, BarCapacity);
        const u32 PeakX = BarX + PeakOffset;
        Length = ::snprintf(Buffer, sizeof(Buffer),
                            "<line x1=\"%u\" y1=\"%u\" x2=\"%u\" y2=\"%u\" stroke=\"#c00\" stroke-width=\"2\"/>\n",
                            PeakX, y, PeakX, y + RowHeight - 4);
        if (!WriteFormatted(FileHandle, Buffer, sizeof(Buffer), Length)) {
            ::CloseHandle(FileHandle);
            return ACS_ERR_OS(IO, 3, "WriteFile failed (svg peak)", ::GetLastError());
        }

        // バー右端に「要求量 / ハード予算 / 件数」を表示する。
        char RequestedText[32];
        char BudgetText[32];
        FormatBytes(RequestedText, sizeof(RequestedText), Statistics.requested_bytes);
        if (Statistics.hard_budget_bytes > 0) {
            FormatBytes(BudgetText, sizeof(BudgetText), Statistics.hard_budget_bytes);
        } else {
            ::snprintf(BudgetText, sizeof(BudgetText), "unlimited");
        }
        Length = ::snprintf(
            Buffer, sizeof(Buffer),
            "<text x=\"%u\" y=\"%u\" text-anchor=\"end\" class=\"lbl\" fill=\"#222\">%s / %s / %llu allocs</text>\n",
            BarWidth >= 4u ? BarX + BarWidth - 4u : BarX, y + RowHeight / 2 + 4u, RequestedText, BudgetText,
            static_cast<unsigned long long>(Statistics.outstanding_allocation_count));
        if (!WriteFormatted(FileHandle, Buffer, sizeof(Buffer), Length)) {
            ::CloseHandle(FileHandle);
            return ACS_ERR_OS(IO, 3, "WriteFile failed (svg statistics)", ::GetLastError());
        }
    }

    const char* const Footer = "</svg>\n";
    if (!WriteAll(FileHandle, Footer, ::strlen(Footer))) {
        ::CloseHandle(FileHandle);
        return ACS_ERR_OS(IO, 3, "WriteFile failed (svg footer)", ::GetLastError());
    }
    if (!::CloseHandle(FileHandle)) return ACS_ERR_OS(IO, 3, "CloseHandle failed (svg)", ::GetLastError());
    return Ok();
}

namespace {

// packed=1 でコンパイラの詰め物を抑制（BMP ヘッダ仕様通りに並べる）
#pragma pack(push, 1)
/** BMP ファイルヘッダ (14 バイト、仕様通りに packed)。 */
struct FBmpFileHeader {
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
struct FBmpInfoHeader {
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
 * @param Row ピクセル行の先頭。
 * @param PixelX 行内のピクセル位置。
 * @param Color 書き込む色。
 */
ACS_FORCEINLINE void PutPixel(u8* Row, u32 PixelX, FRgbColor Color) noexcept
{
    Row[PixelX * 3 + 0] = Color.b;
    Row[PixelX * 3 + 1] = Color.g;
    Row[PixelX * 3 + 2] = Color.r;
}

} // namespace

TResult<void> FMemorySnapshot::WriteBmp(const wchar_t* Path, u32 Width, u32 RowHeight) noexcept
{
    constexpr u64 kMaximumBitmapDimension = 0x7FFFFFFFull;
    constexpr u64 kBitmapHeaderBytes = sizeof(FBmpFileHeader) + sizeof(FBmpInfoHeader);
    if (!Path || Path[0] == L'\0' || Width == 0u || RowHeight < 3u ||
        static_cast<u64>(Width) > kMaximumBitmapDimension) {
        return ACS_ERR(Memory, 44, "invalid BMP dimensions or path");
    }

    FSegmentStats StatisticsArray[(usize)ESegment::_Count];
    const u32 SegmentCount = CMemorySystem::GetStats(StatisticsArray, (u32)ESegment::_Count);
    if (SegmentCount == 0) return ACS_ERR(Memory, 41, "CMemorySystem has no segments");

    const u64 Height64 = static_cast<u64>(SegmentCount) * RowHeight;
    const u64 RowBytes64 = static_cast<u64>(Width) * 3u;
    if (Height64 == 0u || Height64 > kMaximumBitmapDimension || RowBytes64 > (~u64(0)) - 3u) {
        return ACS_ERR(Memory, 44, "BMP dimensions overflow");
    }

    // BMP は 1 行を 4 バイト境界にアラインする
    const u64 Stride64 = (RowBytes64 + 3u) & ~3ull;
    if (Stride64 == 0u || Height64 > (~u64(0)) / Stride64) {
        return ACS_ERR(Memory, 44, "BMP pixel size overflow");
    }
    const u64 PixelBytes64 = Stride64 * Height64;
    if (Stride64 > ~u32(0) || PixelBytes64 > ~u32(0) ||
        PixelBytes64 > static_cast<u64>(~usize(0)) || PixelBytes64 > (~u32(0)) - kBitmapHeaderBytes) {
        return ACS_ERR(Memory, 44, "BMP file exceeds format limits");
    }

    const u32 Height = static_cast<u32>(Height64);
    const u32 Stride = static_cast<u32>(Stride64);
    const u32 PixelBytes = static_cast<u32>(PixelBytes64);

    // ヘッダ初期化
    FBmpFileHeader FileHeader{};
    FBmpInfoHeader InfoHeader{};
    FileHeader.type = 0x4D42;
    FileHeader.offbits = sizeof(FBmpFileHeader) + sizeof(FBmpInfoHeader);
    FileHeader.size = FileHeader.offbits + PixelBytes;

    InfoHeader.size = sizeof(FBmpInfoHeader);
    InfoHeader.width = static_cast<i32>(Width);
    InfoHeader.height = -static_cast<i32>(Height); // 負: 上→下スキャン
    InfoHeader.planes = 1;
    InfoHeader.bitcount = 24;
    InfoHeader.compression = 0;
    InfoHeader.sizeImage = PixelBytes;

    // ピクセルバッファをゼロクリア確保
    u8* const Pixels = static_cast<u8*>(::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, PixelBytes));
    if (!Pixels) return ACS_ERR(Memory, 42, "BMP pixel buffer alloc failed");

    // 全体を浅いグレー (#F7F7F7) で塗る
    ::memset(Pixels, 0xF7, PixelBytes);

    // 各セグメントのバーを描画
    for (u32 i = 0; i < SegmentCount; ++i) {
        const FSegmentStats& Statistics = StatisticsArray[i];
        const FRgbColor& Color = kSegmentColor[(usize)Statistics.segment];
        const u32 YStart = i * RowHeight;
        const u32 YEnd = (i + 1) * RowHeight - 2;
        const u64 BarCapacity = Statistics.hard_budget_bytes > 0
                                    ? Statistics.hard_budget_bytes
                                    : (Statistics.peak_requested_bytes > 0 ? Statistics.peak_requested_bytes : 1);
        u32 UsedWidth = CalculateBarWidth(Width, Statistics.requested_bytes, BarCapacity);
        const u32 ReserveWidth = Width;
        if (UsedWidth > ReserveWidth) UsedWidth = ReserveWidth;

        for (u32 y = YStart; y < YEnd; ++y) {
            u8* const Row = Pixels + y * Stride;
            // 背景色 → 使用量カラーの順に塗る
            for (u32 x = 0; x < ReserveWidth; ++x)
                PutPixel(Row, x, FRgbColor{0xDD, 0xDD, 0xDD});
            for (u32 x = 0; x < UsedWidth; ++x)
                PutPixel(Row, x, Color);
        }
    }

    // ファイル出力
    const HANDLE FileHandle = ::CreateFileW(Path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                            nullptr);
    if (FileHandle == INVALID_HANDLE_VALUE) {
        ::HeapFree(::GetProcessHeap(), 0, Pixels);
        return ACS_ERR_OS(IO, 2, "CreateFileW failed (bmp)", ::GetLastError());
    }
    const bool bWriteSucceeded = WriteAll(FileHandle, &FileHeader, sizeof(FileHeader)) &&
                                 WriteAll(FileHandle, &InfoHeader, sizeof(InfoHeader)) &&
                                 WriteAll(FileHandle, Pixels, PixelBytes);
    const DWORD WriteError = bWriteSucceeded ? ERROR_SUCCESS : ::GetLastError();
    const bool bCloseSucceeded = ::CloseHandle(FileHandle) != 0;
    const DWORD CloseError = bCloseSucceeded ? ERROR_SUCCESS : ::GetLastError();
    ::HeapFree(::GetProcessHeap(), 0, Pixels);
    if (!bWriteSucceeded) {
        return ACS_ERR_OS(IO, 4, "WriteFile failed (bmp)",
                          WriteError == ERROR_SUCCESS ? ERROR_WRITE_FAULT : WriteError);
    }
    if (!bCloseSucceeded) return ACS_ERR_OS(IO, 4, "CloseHandle failed (bmp)", CloseError);
    return Ok();
}

// stdout への簡易テキスト表（CI ログ・コンソール確認用）
void FMemorySnapshot::DumpToStdOut() noexcept
{
    FSegmentStats StatisticsArray[(usize)ESegment::_Count];
    const u32 SegmentCount = CMemorySystem::GetStats(StatisticsArray, (u32)ESegment::_Count);
    ::printf("[ACS Memory Snapshot]\n");
    ::printf("  %-12s | %-10s | %-12s | %-12s | %-12s | %-12s\n", "ESegment", "Allocator", "Budget", "Requested",
             "Peak", "Allocations");
    ::printf("  %s\n", "----------------------------------------------------------------------------------------");
    for (u32 i = 0; i < SegmentCount; ++i) {
        const FSegmentStats& Statistics = StatisticsArray[i];
        char BudgetText[16];
        char RequestedText[16];
        char PeakText[16];
        if (Statistics.hard_budget_bytes > 0) {
            FormatBytes(BudgetText, sizeof(BudgetText), Statistics.hard_budget_bytes);
        } else {
            ::snprintf(BudgetText, sizeof(BudgetText), "unlimited");
        }
        FormatBytes(RequestedText, sizeof(RequestedText), Statistics.requested_bytes);
        FormatBytes(PeakText, sizeof(PeakText), Statistics.peak_requested_bytes);
        ::printf("  %-12s | %-10s | %-12s | %-12s | %-12s | %-12llu\n", Statistics.segment_name,
                 Statistics.allocator_name, BudgetText, RequestedText, PeakText,
                 static_cast<unsigned long long>(Statistics.outstanding_allocation_count));
    }
}

} // namespace acs
