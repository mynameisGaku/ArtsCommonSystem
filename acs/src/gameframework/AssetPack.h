// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"
#include "gameframework/AssetPackReadRequest.h"

namespace acs::game {

// FErrorCode subcode 定義 (EErrCategory::Generic 配下)。
// ISteamworksBridge (1001-1099) / IWorkshopBridge (1101-1199) と subcode 空間が
// 重ならないよう、AssetPack は 1200 番台を使う。

/** Stub による未実装を表す subcode (Mount/Read 等が返す)。 */
inline constexpr u16 kSubAssetPackNotImplemented = 1201;

/** Mount() 前の API 呼び出しを表す subcode。 */
inline constexpr u16 kSubAssetPackNotMounted     = 1202;

/** 一括 read の要求数または配列が不正であることを表す subcode。 */
inline constexpr u16 kSubAssetPackInvalidBatch   = 1203;

/**
 * 現在マウント中の pak の最小情報。
 *
 * @details
 * Bridge は文字列を所有しない。file_path は呼び出し側 (or 実装内 static literal) の
 * メモリを参照するだけで、利用側でコピーしないこと。寿命は「次の Unmount/Mount を
 * 呼ぶまで」を保証する。
 */
struct FAssetPackInfo {
    /** Mount した pak ファイルの絶対 / 相対パス (借用、コピーしない)。 */
    const char* file_path    = nullptr;

    /** pak 全体の content hash (改竄検知用)。 */
    u64         content_hash = 0;

    /** AES-256-GCM 暗号化されているか。 */
    bool        encrypted    = false;

    /** 現在マウント中か。 */
    bool        mounted      = false;
};

/**
 * `.acpak` を読み出すための抽象インターフェース (ランタイム利用側)。
 *
 * @details 実装は本体外モジュール (or テスト) で override する。
 */
class IAssetPackReader {
public:
    /** 既定構築する。 */
    IAssetPackReader() noexcept = default;

    /** 派生クラスを正しく破棄するための仮想デストラクタ。 */
    virtual ~IAssetPackReader() noexcept = default;

    /** コピー禁止 (I/F は参照で扱う)。 */
    IAssetPackReader(const IAssetPackReader&)            = delete;

    /** コピー代入も禁止。 */
    IAssetPackReader& operator=(const IAssetPackReader&) = delete;

    /** ムーブ禁止。 */
    IAssetPackReader(IAssetPackReader&&)                 = delete;

    /** ムーブ代入も禁止。 */
    IAssetPackReader& operator=(IAssetPackReader&&)      = delete;

    /**
     * pak ファイルをマウントし、以降の API 呼び出しを有効にする。
     *
     * @details 多重 Mount は実装依存 (大抵は前の pak を自動 Unmount してから新規 Mount)。
     * @param pack_path マウントする pak ファイルのパス。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> Mount(const char* pack_path) noexcept = 0;

    /** 現在の pak をアンマウントする (Mount() 前に呼んでも安全な no-op)。 */
    virtual void Unmount() noexcept = 0;

    /**
     * 現在マウント中かを返す。
     *
     * @return Mount() 成功後かつ Unmount() 前なら true。
     */
    virtual bool IsMounted() const noexcept = 0;

    /**
     * 現在マウント中の pak に含まれるファイル数を返す。
     *
     * @return ファイル数、未マウント等は失敗エラー。
     */
    virtual TResult<u32> FileCount() noexcept = 0;

    /**
     * index 番目のファイル名 (UTF-8、pak 内仮想パス) を返す。
     *
     * @details 返り値の文字列の寿命は「次の Unmount/Mount を呼ぶまで」。
     * @param index ファイルのインデックス。
     * @return ファイル名、範囲外 index はエラー。
     */
    virtual TResult<const char*> FileName(u32 index) noexcept = 0;

    /**
     * 仮想ファイル名から復号 + 解凍後のオリジナルサイズを取得する。
     *
     * @details ReadFile に渡すバッファサイズの事前確保に使う。
     * @param name pak 内仮想ファイル名。
     * @return オリジナルサイズ (バイト)、未存在パスはエラー。
     */
    virtual TResult<u64> FileSize(const char* name) noexcept = 0;

    /**
     * 仮想ファイル名のデータを out_buffer に復号 + 解凍してコピーする。
     *
     * @param name pak 内仮想ファイル名。
     * @param out_buffer コピー先バッファ。
     * @param buffer_size out_buffer の容量 (FileSize() 戻り値以上必要)。
     * @return 成功なら空の TResult、容量不足等はエラー。
     */
    virtual TResult<void> ReadFile(const char* name, u8* out_buffer, u64 buffer_size) noexcept = 0;

    /**
     * 複数ファイルを要求順に読み取る。
     *
     * @details 既定実装は ReadFile を順番に呼び、既存 backend の互換性を維持する。
     * 後続要素の失敗時も、それ以前の出力は commit 済みである。
     * @param Requests 入力名、出力先、出力容量を持つ要求配列。
     * @param Count Requests の要素数。
     * @param CompletedCount 任意。成功済み要素数を常に書き戻す。
     * @return 全要求を読めた場合は成功、引数不正または個々の ReadFile 失敗時はエラー。
     */
    virtual TResult<void> ReadFiles(const FAssetPackReadRequest* Requests, u32 Count, u32* CompletedCount = nullptr) noexcept;
};

/**
 * `.acpak` を新規作成するための抽象インターフェース (ツール側のみ利用)。
 *
 * @details ランタイムは Reader だけで足りる。Writer はパッキングコマンドで使う。
 */
class IAssetPackWriter {
public:
    /** 既定構築する。 */
    IAssetPackWriter() noexcept = default;

    /** 派生クラスを正しく破棄するための仮想デストラクタ。 */
    virtual ~IAssetPackWriter() noexcept = default;

    /** コピー禁止 (I/F は参照で扱う)。 */
    IAssetPackWriter(const IAssetPackWriter&)            = delete;

    /** コピー代入も禁止。 */
    IAssetPackWriter& operator=(const IAssetPackWriter&) = delete;

    /** ムーブ禁止。 */
    IAssetPackWriter(IAssetPackWriter&&)                 = delete;

    /** ムーブ代入も禁止。 */
    IAssetPackWriter& operator=(IAssetPackWriter&&)      = delete;

    /**
     * 出力 pak ファイルを開いて書き込みを開始する。
     *
     * @details output_path 既存ファイルの扱いは実装依存 (大抵は truncate 上書き)。
     * FinishPack と対で使う。
     * @param output_path 書き込み先 pak ファイルのパス。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> BeginPack(const char* output_path) noexcept = 0;

    /**
     * 1 ファイルを pak に追加する (実装は compress-then-encrypt 順で構築)。
     *
     * @details 実装は virtual_name と data を呼び出し中に取り込む。成功後は呼び出し側が
     * 両方の入力領域を直ちに再利用または解放してよい。
     * @param virtual_name pak 内仮想パス。
     * @param data オリジナル (非圧縮 / 非暗号) バイト列。
     * @param size data のバイトサイズ。
     * @return 成功なら空の TResult、BeginPack 前の呼び出し等はエラー。
     */
    virtual TResult<void> AddFile(const char* virtual_name, const u8* data, u64 size) noexcept = 0;

    /**
     * pak を確定して書き込みを終える。
     *
     * @details TOC を暗号化し、ヘッダの content_hash を確定する。以降の AddFile はエラー。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> FinishPack() noexcept = 0;
};

/**
 * AssetPack 未統合ビルド / ユニットテスト用の no-op な Reader 実装。
 *
 * @details
 * Mount() / FileCount() / FileName() / FileSize() / ReadFile() は全て
 * ACS_ERR(Generic, kSubAssetPackNotImplemented) を返す。Unmount() は副作用なし、
 * IsMounted() は常に false。
 */
class CAssetPackReaderStub final : public IAssetPackReader {
public:
    /** 既定構築する。 */
    CAssetPackReaderStub() noexcept = default;

    /** 破棄する。 */
    ~CAssetPackReaderStub() noexcept override = default;

    /**
     * NotImplemented を返す (stub)。
     *
     * @param pack_path 無視される。
     * @return 常に kSubAssetPackNotImplemented エラー。
     */
    TResult<void>         Mount(const char* pack_path) noexcept override;

    /** 副作用なしの no-op (stub)。 */
    void                 Unmount() noexcept override;

    /**
     * 常に未マウントを返す (stub)。
     *
     * @return 常に false。
     */
    bool                 IsMounted() const noexcept override { return false; }

    /**
     * NotImplemented を返す (stub)。
     *
     * @return 常に kSubAssetPackNotImplemented エラー。
     */
    TResult<u32>          FileCount() noexcept override;

    /**
     * NotImplemented を返す (stub)。
     *
     * @param index 無視される。
     * @return 常に kSubAssetPackNotImplemented エラー。
     */
    TResult<const char*>  FileName(u32 index) noexcept override;

    /**
     * NotImplemented を返す (stub)。
     *
     * @param name 無視される。
     * @return 常に kSubAssetPackNotImplemented エラー。
     */
    TResult<u64>          FileSize(const char* name) noexcept override;

    /**
     * NotImplemented を返す (stub)。
     *
     * @param name 無視される。
     * @param out_buffer 無視される。
     * @param buffer_size 無視される。
     * @return 常に kSubAssetPackNotImplemented エラー。
     */
    TResult<void>         ReadFile(const char* name, u8* out_buffer, u64 buffer_size) noexcept override;
};

/**
 * AssetPack 未統合ビルド / ユニットテスト用の no-op な Writer 実装。
 *
 * @details 全 API が NotImplemented (kSubAssetPackNotImplemented) を返す。
 */
class CAssetPackWriterStub final : public IAssetPackWriter {
public:
    /** 既定構築する。 */
    CAssetPackWriterStub() noexcept = default;

    /** 破棄する。 */
    ~CAssetPackWriterStub() noexcept override = default;

    /**
     * NotImplemented を返す (stub)。
     *
     * @param output_path 無視される。
     * @return 常に kSubAssetPackNotImplemented エラー。
     */
    TResult<void>  BeginPack(const char* output_path) noexcept override;

    /**
     * NotImplemented を返す (stub)。
     *
     * @param virtual_name 無視される。
     * @param data 無視される。
     * @param size 無視される。
     * @return 常に kSubAssetPackNotImplemented エラー。
     */
    TResult<void>  AddFile(const char* virtual_name, const u8* data, u64 size) noexcept override;

    /**
     * NotImplemented を返す (stub)。
     *
     * @return 常に kSubAssetPackNotImplemented エラー。
     */
    TResult<void>  FinishPack() noexcept override;
};

/**
 * 既定 stub の Reader (Meyer's singleton) を返す。
 *
 * @return プロセス共有の CAssetPackReaderStub 参照。
 */
IAssetPackReader& GetReaderStub() noexcept;

/**
 * 既定 stub の Writer (Meyer's singleton) を返す。
 *
 * @return プロセス共有の CAssetPackWriterStub 参照。
 */
IAssetPackWriter& GetWriterStub() noexcept;

// 既定 AssetPack の provider 結線 (実 backend モジュールへの委譲点)。
// gameframework は実 backend モジュール (ACS::AssetPack / CAcpakGameReader /
// CAcpakGameWriter) に依存できない (循環依存になる: backend 側が本 interface に
// 依存する)。そこで実 backend 側が SetAssetPackReaderProvider() /
// SetAssetPackWriterProvider() で「既定 Reader/Writer を返す関数」を登録し、
// ゲームコードは GetDefaultAssetPackReader() / GetDefaultAssetPackWriter() を
// 通じて backend 非依存に既定実装を取得する。provider 未登録なら Stub を返す。

/** 既定 Reader を返す provider 関数ポインタ型。 */
using AssetPackReaderProvider = IAssetPackReader& (*)() noexcept;

/** 既定 Writer を返す provider 関数ポインタ型。 */
using AssetPackWriterProvider = IAssetPackWriter& (*)() noexcept;

/**
 * 既定 Reader provider を登録する (実 backend モジュールの Install* から呼ぶ)。
 *
 * @param provider 登録する provider 関数 (nullptr で stub に戻す)。後勝ち。
 */
void SetAssetPackReaderProvider(AssetPackReaderProvider provider) noexcept;

/**
 * 既定 Reader を返す。
 *
 * @return provider 登録済みならその実装、未登録なら GetReaderStub()。
 */
IAssetPackReader& GetDefaultAssetPackReader() noexcept;

/**
 * 既定 Writer provider を登録する (実 backend モジュールの Install* から呼ぶ)。
 *
 * @param provider 登録する provider 関数 (nullptr で stub に戻す)。後勝ち。
 */
void SetAssetPackWriterProvider(AssetPackWriterProvider provider) noexcept;

/**
 * 既定 Writer を返す。
 *
 * @return provider 登録済みならその実装、未登録なら GetWriterStub()。
 */
IAssetPackWriter& GetDefaultAssetPackWriter() noexcept;

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FAssetPackReaderStub = CAssetPackReaderStub;

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FAssetPackWriterStub = CAssetPackWriterStub;

} // namespace acs::game
