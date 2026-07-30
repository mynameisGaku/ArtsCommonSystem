// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"
#include "container/Array.h"
#include "threading/Mutex.h"

#include "assetpack/AcpakFormat.h"
#include "assetpack/AcpakCrypto.h"

namespace acs::assetpack {

/**
 * 複数のバラのファイルを 1 つの `.acpak` にまとめる Writer。
 *
 * @details
 * AddFile は実書き込みせず内部の pending list に仮想パスとデータをコピーし、
 * 実書き込みは Finalize 内で一気に行う。呼び出し側は AddFile 成功後に入力を
 * 直ちに再利用または解放できる。ツールビルド
 * (パッキングコマンド) から使う想定で、ランタイムは FAcpakReader だけで足りる。
 * ハンドル + pending list を所有するため non-copy / non-move。
 */
class FAcpakWriter {
public:
    /** 空状態で構築する (出力は Open で開く)。 */
    FAcpakWriter() noexcept;

    /**
     * 指定 allocator で pending list を持つ空状態を構築する。
     *
     * @param Allocator pending list の確保に使う allocator。
     */
    explicit FAcpakWriter(FAllocator& Allocator) noexcept;

    /** 破棄する (Open 済なら Close 相当の後始末を行う)。 */
    ~FAcpakWriter() noexcept;

    /** コピー禁止 (ハンドル + pending list を単独所有するため)。 */
    FAcpakWriter(const FAcpakWriter&) = delete;

    /** コピー代入も禁止。 */
    FAcpakWriter& operator=(const FAcpakWriter&) = delete;

    /** ムーブ禁止 (固定アドレスでライフタイムを管理するため)。 */
    FAcpakWriter(FAcpakWriter&&) = delete;

    /** ムーブ代入も禁止。 */
    FAcpakWriter& operator=(FAcpakWriter&&) = delete;

    /**
     * 出力ファイルを開き、ヘッダのプレースホルダを書く (既存は上書き)。
     *
     * @details
     * CREATE_ALWAYS で開き、file_count / file_table_offset を 0 にしたヘッダを先に
     * 書く (実値は Finalize で seek して上書き)。成功すると以降 AddFile / Finalize が
     * 呼べる。flags は AcpakFlagNone / AcpakFlagCompressed / AcpakFlagEncrypted の
     * 任意組み合わせ。Encrypted を立てる場合は Open より前に SetKey() で鍵を設定する
     * こと (鍵未設定でも Open 自体は成功し、Finalize 時に kAcpakSubCryptoKey を返す)。
     * 既に Open 状態なら kAcpakSubAlreadyOpen、未知 flag bit は kAcpakSubBadFlags。
     * @param OutputPath 出力する `.acpak` の UTF-16 パス。
     * @param Flags 書き出しオプション (圧縮 / 暗号化)。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    TResult<void> Open(const wchar_t* OutputPath, EAcpakFlags Flags) noexcept;

    /**
     * 暗号化用の鍵を設定する。
     *
     * @details
     * Open 前後どちらでも呼べる。flags に AcpakFlagEncrypted が含まれるとき
     * Finalize で AES-256-GCM 暗号化に使われる。Close すると 0 クリアされる。
     * @param Key 設定する AES-256 鍵。
     */
    void SetKey(const FAcpakKey& Key) noexcept;

    /**
     * ハンドルを閉じて内部状態をクリアする (多重 Close は no-op)。
     *
     * @details
     * Finalize 前に呼ぶと書きかけアーカイブがディスクに残る (DeleteFileW は
     * 呼ばない)。鍵情報も 0 クリアする。
     */
    void Close() noexcept;

    /**
     * 1 ファイルを pak に追加する (実書き込みはせず pending list に積む)。
     *
     * @details
     * virtual_path / data は呼び出し中に内部所有領域へコピーする。成功後は呼び出し側で
     * 直ちに再利用または解放できる。仮想 path は `/` 区切りの正規形だけを受け入れ、
     * hash を一度保持して重複候補だけを完全比較する。size 0 のファイルも追加できる。
     * Open 前 / Finalize 後に呼ぶと kAcpakSubNotOpen、data が null かつ size>0 は
     * kAcpakSubIOFailure。
     * @param VirtualPath pak 内の正規形仮想パス (UTF-16、完全一致で検索される)。
     * @param Data 追加するファイルの生バイト列。
     * @param Size Data のバイト数。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    TResult<void> AddFile(const wchar_t* VirtualPath, const void* Data, u64 Size) noexcept;

    /**
     * pak を確定し、header → 全ファイルデータ → file table の順で書き出す。
     *
     * @details
     * 各 entry を compress-then-encrypt パイプライン (圧縮 → 暗号化、flags 依存) に
     * 通し、CRC32 (元バイト基準) を file table に格納する。最後にヘッダを seek して
     * file_count / file_table_offset を確定し FlushFileBuffers する。成功時は
     * ハンドルが flush 済 (Close を呼んでよい)。Open 前 / 2 回目の呼び出しは
     * kAcpakSubNotOpen、暗号化 flag 立ちで鍵未設定は kAcpakSubCryptoKey。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    TResult<void> Finalize() noexcept;

private:
    /** AddFile が積み Finalize が消費する所有 entry。 */
    struct FPendingEntry {
        explicit FPendingEntry(FAllocator& Allocator) noexcept : Path(Allocator), Data(Allocator)
        {
        }

        FPendingEntry(const FPendingEntry&) = delete;
        FPendingEntry& operator=(const FPendingEntry&) = delete;
        FPendingEntry(FPendingEntry&&) noexcept = default;
        FPendingEntry& operator=(FPendingEntry&&) noexcept = default;

        /** NUL 終端を含む仮想パス。 */
        TArray<wchar_t> Path;

        /** 正規形 Path から AddFile 時に一度だけ計算した hash。 */
        u64 PathHash = 0u;

        /** 追加するファイルの生バイト列。 */
        TArray<u8> Data;
    };

    /** Finalize 後 / Open 失敗時に内部状態 (flags / pending / 鍵) をクリアする。 */
    void ResetState() noexcept;

    /** 全公開操作とファイルハンドルの寿命を直列化する。 */
    mutable FMutex m_LifecycleLock;

    /** pending entry と一時バッファに使う allocator。 */
    FAllocator* m_Allocator = nullptr;

    /** Win32 HANDLE 相当 (<windows.h> を header から外すため void* で保持)。 */
    void* m_FileHandle = nullptr;

    /** header.flags (encrypted / compressed)。 */
    u32 m_Flags = 0;

    /** Finalize 済フラグ (2 回目の AddFile / Finalize を弾く)。 */
    bool m_Finalized = false;

    /** AddFile が積んだ entry 群 (Finalize で消費)。 */
    TArray<FPendingEntry> m_Pending;

    /** 暗号化鍵 (AcpakFlagEncrypted のときに Finalize で使う、Close で 0 クリア)。 */
    FAcpakKey m_Key{};

    /** SetKey で鍵が設定されたか (Close で false にリセット)。 */
    bool m_HasKey = false;

    /** 原子的置換先。Open 成功から Finalize/Close まで所有する。 */
    wchar_t m_OutputPath[kAcpakMaxOutputPathLength + 1u] = {};

    /** 出力先と同じ directory に CREATE_NEW した一意な一時ファイル名。 */
    wchar_t m_TemporaryPath[kAcpakMaxOutputPathLength + 96u] = {};
};

} // namespace acs::assetpack
