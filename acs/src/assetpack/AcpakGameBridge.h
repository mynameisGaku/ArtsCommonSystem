// SPDX-License-Identifier: Apache-2.0
// Concrete GameFramework bridge for `.acpak`.
#pragma once

#include "assetpack/AcpakReader.h"
#include "assetpack/AcpakWriter.h"
#include "container/Array.h"
#include "gameframework/AssetPack.h"
#include "threading/RwLock.h"

namespace acs::assetpack {

/**
 * gameframework の IAssetPackReader を実 `.acpak` Reader で実装する具象 backend。
 *
 * @details
 * gameframework が UTF-8 const char* でやり取りする API を受け、内部の
 * FAcpakReader (UTF-16 wchar_t* ベース) へ橋渡しする。ファイル名は Mount ごとの
 * UTF-8 pool に保持し、戻り値を次の Mount / Unmount まで安定させる。non-copy / non-move。
 */
class FAcpakGameReader final : public acs::game::IAssetPackReader {
public:
    /** 空状態で構築する (pak は Mount で開く)。 */
    FAcpakGameReader() noexcept;

    /** 指定 allocator を内部 Reader の配列に使う空状態で構築する。 */
    explicit FAcpakGameReader(acs::FAllocator& Allocator) noexcept;

    /** 破棄する (FAcpakReader が開いていれば自動 Close)。 */
    ~FAcpakGameReader() noexcept override;

    /** コピー禁止 (FAcpakReader を単独所有するため)。 */
    FAcpakGameReader(const FAcpakGameReader&) = delete;

    /** コピー代入も禁止。 */
    FAcpakGameReader& operator=(const FAcpakGameReader&) = delete;

    /** ムーブ禁止。 */
    FAcpakGameReader(FAcpakGameReader&&) = delete;

    /** ムーブ代入も禁止。 */
    FAcpakGameReader& operator=(FAcpakGameReader&&) = delete;

    /**
     * UTF-8 パスを UTF-16 に変換して `.acpak` を開く。
     *
     * @param PackPath マウントする pak ファイルの UTF-8 パス。
     * @return 成功なら空の TResult、変換失敗 / Open 失敗ならエラー。
     */
    acs::TResult<void> Mount(const char* PackPath) noexcept override;

    /** 現在の pak をアンマウントする (FAcpakReader::Close、未 Mount でも安全)。 */
    void Unmount() noexcept override;

    /**
     * pak がマウント済みかを返す。
     *
     * @return 開いていれば true。
     */
    bool IsMounted() const noexcept override;

    /**
     * 現在マウント中の pak のファイル数を返す。
     *
     * @return ファイル数、未 Mount ならエラー (kSubAssetPackNotMounted)。
     */
    acs::TResult<acs::u32> FileCount() noexcept override;

    /**
     * Index 番目のファイル名を UTF-8 で返す。
     *
     * @details Mount 時に構築した UTF-8 pool を返す。戻り値は次の Mount / Unmount
     * まで有効。
     * @param Index ファイルのインデックス。
     * @return UTF-8 ファイル名、未 Mount / 範囲外 / 変換失敗ならエラー。
     */
    acs::TResult<const char*> FileName(acs::u32 Index) noexcept override;

    /**
     * 仮想ファイル名から復号 + 解凍後のサイズを返す。
     *
     * @param Name 取得する pak 内仮想ファイル名 (UTF-8)。
     * @return uncompressed サイズ、変換失敗 / 未存在ならエラー。
     */
    acs::TResult<acs::u64> FileSize(const char* Name) noexcept override;

    /**
     * 仮想ファイルのデータを復号 + 解凍して OutBuffer にコピーする。
     *
     * @param Name 読み出す pak 内仮想ファイル名 (UTF-8)。
     * @param OutBuffer 出力先バッファ。
     * @param BufferSize OutBuffer のバイト数 (FileSize 以上必要)。
     * @return 成功なら空の TResult、変換失敗 / バッファ不足 / I/O 失敗ならエラー。
     */
    acs::TResult<void> ReadFile(const char* Name, acs::u8* OutBuffer, acs::u64 BufferSize) noexcept override;

private:
    /** NUL 終端を含む UTF-16 パスの最大容量。 */
    static constexpr acs::u32 kPathCapacity = kAcpakMaxPathLength + 1u;

    /** Reader entry 群から Mount 寿命の UTF-8 ファイル名 pool を構築する。 */
    acs::TResult<void> BuildFileNamePool() noexcept;

    /** UTF-8 ファイル名 pool と offset 表を解放する。 */
    void ReleaseFileNamePool() noexcept;

    /** Mount/Unmount と全読み取り API の寿命を同期する。 */
    mutable RwLock m_LifecycleLock;

    /** 実 `.acpak` 読み出しを担う Reader。 */
    FAcpakReader m_Reader;

    /** FileName が返す UTF-8 文字列の連結 pool。 */
    TArray<char> m_Utf8NamePool;

    /** entry index ごとの m_Utf8NamePool 内 offset。 */
    TArray<usize> m_Utf8NameOffsets;
};

/**
 * gameframework の IAssetPackWriter を実 `.acpak` Writer で実装する具象 backend。
 *
 * @details
 * gameframework の UTF-8 const char* API を受け、内部の FAcpakWriter
 * (UTF-16 wchar_t* ベース) へ橋渡しする。BeginPack は AcpakFlagNone (無圧縮 / 無暗号)
 * で開く。変換先は各呼び出しのローカル領域に置く。non-copy / non-move。
 */
class FAcpakGameWriter final : public acs::game::IAssetPackWriter {
public:
    /** 空状態で構築する (pak は BeginPack で開く)。 */
    FAcpakGameWriter() noexcept = default;

    /** 指定 allocator を内部 Writer の pending list に使う空状態で構築する。 */
    explicit FAcpakGameWriter(acs::FAllocator& allocator) noexcept : m_Writer(allocator)
    {
    }

    /** 破棄する (FAcpakWriter が開いていれば自動 Close)。 */
    ~FAcpakGameWriter() noexcept override = default;

    /** コピー禁止 (FAcpakWriter を単独所有するため)。 */
    FAcpakGameWriter(const FAcpakGameWriter&) = delete;

    /** コピー代入も禁止。 */
    FAcpakGameWriter& operator=(const FAcpakGameWriter&) = delete;

    /** ムーブ禁止。 */
    FAcpakGameWriter(FAcpakGameWriter&&) = delete;

    /** ムーブ代入も禁止。 */
    FAcpakGameWriter& operator=(FAcpakGameWriter&&) = delete;

    /**
     * 出力 pak を AcpakFlagNone で開いて書き込みを開始する。
     *
     * @param OutputPath 出力 pak ファイルの UTF-8 パス。
     * @return 成功なら空の TResult、変換失敗 / Open 失敗ならエラー。
     */
    acs::TResult<void> BeginPack(const char* OutputPath) noexcept override;

    /**
     * 1 ファイルを pak に追加する。
     *
     * @param VirtualName pak 内仮想ファイル名 (UTF-8)。
     * @param Data オリジナル (非圧縮 / 非暗号) バイト列。
     * @param Size Data のバイト数。
     * @return 成功なら空の TResult、変換失敗 / 追加失敗ならエラー。
     */
    acs::TResult<void> AddFile(const char* VirtualName, const acs::u8* Data, acs::u64 Size) noexcept override;

    /**
     * pak を確定して書き込みを終える (Finalize → Close)。
     *
     * @return Finalize の結果 (成功なら空の TResult、失敗ならエラー)。
     */
    acs::TResult<void> FinishPack() noexcept override;

private:
    /** NUL 終端を含む UTF-16 パスの最大容量。 */
    static constexpr acs::u32 kPathCapacity = kAcpakMaxPathLength + 1u;

    /** 実 `.acpak` 書き込みを担う Writer。 */
    FAcpakWriter m_Writer;
};

/**
 * プロセス共有の既定 Acpak Reader を返す (Meyers singleton)。
 *
 * @details
 * gameframework は本 backend モジュールに依存できないため、結線は backend 側から
 * acs::game::SetAssetPackReaderProvider() に本関数を登録して行う
 * (InstallAcpakReaderAsDefault が登録する)。以降は backend 非依存に
 * acs::game::GetDefaultAssetPackReader() でこの実装が得られる。
 * @return プロセス共有の FAcpakGameReader への参照。
 */
acs::game::IAssetPackReader& GetDefaultAcpakReader() noexcept;

/** GetDefaultAcpakReader を gameframework の既定 Reader provider として登録する。 */
void InstallAcpakReaderAsDefault() noexcept;

/**
 * プロセス共有の既定 Acpak Writer を返す (Meyers singleton)。
 *
 * @return プロセス共有の FAcpakGameWriter への参照。
 */
acs::game::IAssetPackWriter& GetDefaultAcpakWriter() noexcept;

/** GetDefaultAcpakWriter を gameframework の既定 Writer provider として登録する。 */
void InstallAcpakWriterAsDefault() noexcept;

} // namespace acs::assetpack
