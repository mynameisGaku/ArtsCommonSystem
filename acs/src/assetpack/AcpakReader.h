// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"
#include "container/Array.h"
#include "threading/Mutex.h"
#include "threading/RwLock.h"
#include "threading/Atomic.h"

#include "assetpack/AcpakFormat.h"
#include "assetpack/AcpakCrypto.h"
#include "assetpack/AcpakReadDiagnostics.h"

namespace acs::assetpack {

/**
 * 1 つの `.acpak` を開き、含まれる仮想ファイルを名前で取り出す Reader。
 *
 * @details
 * header と file table を読み込み、各 path を内部の文字列 pool に保持する。
 * manifest 読み込み時に正規形 path の hash を一度だけ保持し、検索時は hash
 * 一致候補だけを完全比較する。ファイルハンドル + 文字列 pool + entry 配列を
 * 所有するため non-copy / non-move で、固定アドレスでライフタイムを管理する。
 * GameFramework の IAssetPackReader とは独立に動作する。
 */
class CAcpakReader {
public:
    /** 空状態で構築する (ファイルは Open で開く)。 */
    CAcpakReader() noexcept;

    /**
     * 指定 allocator で file table と文字列 pool を持つ空状態を構築する。
     *
     * @param Allocator 内部配列の確保に使う allocator。
     */
    explicit CAcpakReader(FAllocator& Allocator) noexcept;

    /** 破棄する (Open 済なら Close 相当の後始末を行う)。 */
    ~CAcpakReader() noexcept;

    /** コピー禁止 (ハンドル + pool を単独所有するため)。 */
    CAcpakReader(const CAcpakReader&) = delete;

    /** コピー代入も禁止。 */
    CAcpakReader& operator=(const CAcpakReader&) = delete;

    /** ムーブ禁止 (entry.path が内部 pool を指すため)。 */
    CAcpakReader(CAcpakReader&&) = delete;

    /** ムーブ代入も禁止。 */
    CAcpakReader& operator=(CAcpakReader&&) = delete;

    /**
     * `.acpak` ファイルを開き、header と file table を読み出す。
     *
     * @details
     * file_path は UTF-16 (Windows convention)。成功すると以降の API が有効に
     * なる。多重 Open は新しい pak の検証完了後にだけ前回の状態を置換する。
     * 失敗時は現在の handle・manifest・鍵を維持し、未 Open なら未 Open のままにする。
     * 暗号化 pak (AcpakFlagEncrypted) は Open の
     * 前に SetKey() で鍵を与えておく必要がある (Open 自体は header と file table
     * = nonce/tag を含む までしか読まないので鍵不要だが、鍵なしで encrypted pak
     * の ReadFile を呼ぶと kAcpakSubCryptoKey を返す)。失敗時は
     * kAcpakSubIOFailure (CreateFileW/ReadFile 失敗) / kAcpakSubBadSize
     * (ヘッダより小さい) / kAcpakSubBadMagic / kAcpakSubBadVersion /
     * kAcpakSubBadFlags (未知 flags bit)。
     * @param FilePath 開く `.acpak` の UTF-16 パス。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    TResult<void> Open(const wchar_t* FilePath) noexcept;

    /**
     * 暗号化 pak の復号鍵を設定する。
     *
     * @details
     * Open の前後どちらでも呼べる。ReadFile 内で AES-256-GCM 復号に使われ、
     * flags=0 の pak では無視される。Close すると内部の鍵情報も 0 クリアされる。
     * @param Key 設定する AES-256 鍵。
     */
    void SetKey(const FAcpakKey& Key) noexcept;

    /**
     * ハンドルを閉じ、文字列 pool + entry 配列を解放する。
     *
     * @details Open 前 / 既に Close 済でも安全 (no-op)。鍵情報も 0 クリアする。
     */
    void Close() noexcept;

    /**
     * pak が開いているかを返す。
     *
     * @return Open 成功後かつ Close 前なら true。
     */
    bool IsOpen() const noexcept;

    /**
     * 現在開いている pak に含まれる仮想ファイル数を返す。
     *
     * @return 仮想ファイル数 (未 Open なら 0)。
     */
    u32 FileCount() const noexcept;

    /**
     * index 番目の entry を返す。
     *
     * @details 返り値の寿命は次の Close まで。
     * @param Index 取得する entry のインデックス。
     * @return entry へのポインタ (範囲外 / 未 Open なら nullptr)。
     */
    const FAcpakFileEntry* GetEntry(u32 Index) const noexcept;

    /**
     * 正規形の仮想パスから entry を探す。
     *
     * @details
     * path hash を一回計算し、manifest 読み込み時に保持した hash と一致する
     * 候補だけを wcscmp 相当で完全比較する。
     * @param Path 探す正規形仮想パス (UTF-16)。
     * @return 見つかった entry (無い / 未 Open なら nullptr)。
     */
    const FAcpakFileEntry* FindEntry(const wchar_t* Path) const noexcept;

    /**
     * 仮想パスのファイルを out_buffer に読み出す (復号 + 解凍 + CRC 検証)。
     *
     * @details
     * buffer_size は GetUncompressedSize() の返す値以上必要 (不足は
     * kAcpakSubBufferTooSmall)。読み出し後 CRC32 を entry.crc32 と照合し、不一致は
     * kAcpakSubBadCrc を返す (エラー時の buffer 内容は使わないこと)。
     * @param Path 読み出す仮想パス (UTF-16)。
     * @param OutBuffer 読み出し先バッファ。
     * @param BufferSize OutBuffer の容量バイト数。
     * @return 実際に書き込んだバイト数 (= size_uncompressed)、失敗ならエラー。
     */
    TResult<u64> ReadFile(const wchar_t* Path, void* OutBuffer, u64 BufferSize) noexcept;

    /**
     * 複数ファイルを要求順に読み取る。
     *
     * @details ライフサイクルロックと不変マッピングを一括共有し、要求順と検証順を
     * 変えない。後続要素の失敗時も、それ以前の出力は commit 済みである。
     * @param Paths 読み出す UTF-16 仮想パスの配列。
     * @param OutBuffers 各要求の出力先バッファ配列。
     * @param BufferSizes 各出力先の容量配列。
     * @param Count 各配列の要素数。
     * @param CompletedCount 任意。成功済み要素数を常に書き戻す。
     * @return 全要求を読めた場合は総バイト数、引数不正、容量不足、検証失敗、I/O 失敗時はエラー。
     */
    TResult<u64> ReadFiles(const wchar_t* const* Paths, void* const* OutBuffers, const u64* BufferSizes, u32 Count, u32* CompletedCount = nullptr) noexcept;

    /** 読み取りを完了境界で止め、relaxed カウンタ群を一括集約する。 */
    FAcpakReadDiagnostics ReadDiagnostics() const noexcept;

    /** 進行中の読み取り完了後、診断カウンタを決定的に初期化する。 */
    void ResetReadDiagnostics() noexcept;

    /**
     * 仮想パスの復号 + 解凍後のバイト数を返す。
     *
     * @details ReadFile 用バッファの事前確保に使う。
     * @param Path サイズを問い合わせる仮想パス (UTF-16)。
     * @return size_uncompressed バイト数 (未存在パスは kAcpakSubNotFound)。
     */
    TResult<u64> GetUncompressedSize(const wchar_t* Path) const noexcept;

    /**
     * ヘッダから読み取った flags をそのまま返す。
     *
     * @details encrypted / compressed のどのビットが立っているかを診断する用途。
     * @return header.flags の値。
     */
    u32 Flags() const noexcept;

private:
    /**
     * ヘッダ + file table をハンドルから読み出して内部状態を構築する。
     *
     * @details Open() からのみ呼ばれる。失敗時は呼び出し側 (Open) が Close する。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    TResult<void> LoadHeaderAndTable() noexcept;

    /**
     * 空の Reader に OS handle と manifest を読み込む。
     * public Open() はこの関数を一時 Reader に対して呼び、成功時だけ状態を入れ替える。
     */
    TResult<void> OpenIntoEmptyUnlocked(const wchar_t* FilePath) noexcept;

    /** ライフサイクルロック取得済みで内部状態を空に戻す。 */
    void CloseUnlocked() noexcept;

    /** ライフサイクル共有ロック取得済みで entry を検索する。 */
    const FAcpakFileEntry* FindEntryUnlocked(const wchar_t* Path) const noexcept;

    /** ライフサイクル共有ロック取得済みで一つの entry を読み取る。 */
    TResult<u64> ReadEntryUnlocked(const FAcpakFileEntry& Entry, void* OutBuffer, u64 BufferSize) noexcept;

    /** 現在の不変ファイルスナップショットへ読み取り専用マッピングを試す。 */
    void TryCreateReadMappingUnlocked() noexcept;

    /** 読み取り経路の relaxed 診断カウンタ群。 */
    struct FReadDiagnosticCounters {
        /** mapping から読んだ回数。 */
        TAtomic<u64> MappedReadCount{0u};
        /** mapping から読んだ格納 byte 数。 */
        TAtomic<u64> MappedReadBytes{0u};
        /** Win32 ReadFile で読んだ回数。 */
        TAtomic<u64> BufferedReadCount{0u};
        /** Win32 ReadFile で読んだ格納 byte 数。 */
        TAtomic<u64> BufferedReadBytes{0u};
        /** 保持 scratch を再利用できた回数。 */
        TAtomic<u64> ScratchReuseCount{0u};
        /** 局所 scratch へ fallback した回数。 */
        TAtomic<u64> ScratchFallbackCount{0u};
        /** batch read 呼び出し数。 */
        TAtomic<u64> BatchCount{0u};
        /** batch read で処理した要求数。 */
        TAtomic<u64> BatchEntryCount{0u};
    };

    /** Open/Close と読み出し処理の寿命を同期する。 */
    mutable FRwLock m_LifecycleLock;

    /** SetFilePointerEx と ReadFile の組を不可分にする。 */
    mutable FMutex m_IoLock;

    /** Win32 HANDLE 相当 (<windows.h> を header から外すため void* で保持)。 */
    void* m_FileHandle = nullptr;

    /** 読み取り専用 file mapping handle。作成失敗時は null。 */
    void* m_FileMappingHandle = nullptr;

    /** パッケージ全体の読み取り専用 view。作成失敗時は null。 */
    const u8* m_MappedView = nullptr;

    /** CreateFileW 直後に GetFileSizeEx で得たファイル長。 */
    u64 m_FileSize = 0;

    /** header.flags (encrypted / compressed)。 */
    u32 m_Flags = 0;

    /** header.file_table_offset。 */
    u64 m_TableOffset = 0;

    /** file table の in-memory 表現 (entry.path は m_StringPool を指す)。 */
    TArray<FAcpakFileEntry> m_Entries;

    /** m_Entries と同じ index で保持する正規形仮想 path の hash。 */
    TArray<u64> m_PathHashes;

    /** path 文字列の連結 pool (NUL 区切り)。 */
    TArray<wchar_t> m_StringPool;

    /** 暗号化 pak の復号鍵 (flags=0 のときは未使用、Close で 0 クリア)。 */
    FAcpakKey m_Key{};

    /** SetKey で鍵が設定されたか (Close で false にリセット)。 */
    bool m_HasKey = false;

    /** 小中規模の格納データを再利用する一時領域。 */
    TArray<u8> m_StoredScratch;

    /** 検証完了まで出力を保持する再利用領域。 */
    TArray<u8> m_FinalScratch;

    /** 保持一時領域を一呼び出しだけに貸し出す。 */
    mutable FMutex m_ScratchLock;

    /** correctness から独立した累積診断値。 */
    FReadDiagnosticCounters m_ReadDiagnosticCounters;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FAcpakReader = CAcpakReader;

} // namespace acs::assetpack
