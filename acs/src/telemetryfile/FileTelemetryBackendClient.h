// SPDX-License-Identifier: Apache-2.0
// File-backed telemetry sink for acs::game::IBackendClient.
#pragma once

#include "gameframework/BackendClient.h"

namespace acs::telemetryfile {

/**
 * テレメトリイベントをファイルへ追記する IBackendClient 実装。
 *
 * @details
 * Connect で "file://" スキーム付き (またはそのまま) のパスを開き ("ab" 追記)、
 * SendTelemetry ごとに {"event":...,"payload":...} を 1 行ずつ書いて即 flush する。
 * server_url は呼出側が寿命を保証する非所有文字列で、本実装は内部固定長バッファに
 * コピーして保持する。non-copy / non-move 型。
 */
class FFileTelemetryBackendClient final : public acs::game::IBackendClient {
public:
    /** 空のクライアントを構築する (ファイルは未オープン)。 */
    FFileTelemetryBackendClient() noexcept = default;

    /** 破棄する (オープン中ならファイルを flush・close する)。 */
    ~FFileTelemetryBackendClient() noexcept override;

    /** コピー禁止 (ファイルハンドルを単独所有するため)。 */
    FFileTelemetryBackendClient(const FFileTelemetryBackendClient&)            = delete;

    /** コピー代入も禁止。 */
    FFileTelemetryBackendClient& operator=(const FFileTelemetryBackendClient&) = delete;

    /** ムーブ禁止。 */
    FFileTelemetryBackendClient(FFileTelemetryBackendClient&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FFileTelemetryBackendClient& operator=(FFileTelemetryBackendClient&&)      = delete;

    /**
     * 出力先ファイルを開く (追記モード)。
     *
     * @details
     * server_url から "file://" スキームを剥がしたパスを開く。空パスは
     * kSub_BadArgument、オープン失敗は kSubTelemetryFileOpenFailed を返す。
     * 既にオープン中なら先に Disconnect してから開き直す。
     * @param server_url 出力先パス ("file://" スキーム付き可、呼出側が寿命を保証)。
     * @return 成功なら空の TResult、引数不正やオープン失敗ならエラー。
     */
    acs::TResult<void> Connect(const char* server_url) noexcept override;

    /** オープン中のファイルを flush・close する (未接続なら no-op、べき等)。 */
    void               Disconnect() noexcept override;

    /**
     * ファイルがオープン中かを返す。
     *
     * @return オープン中なら true。
     */
    bool               IsConnected() const noexcept override;

    /**
     * テレメトリイベントを 1 行 JSON で追記し、即 flush する。
     *
     * @details
     * {"event":"<escaped event_name>","payload":<json_payload>} を書いて改行で閉じる。
     * event_name は文字列としてエスケープ (", \\, 改行 等) されるが json_payload は
     * そのまま書かれる。未接続は kSub_NotConnected、引数が nullptr は kSub_BadArgument、
     * 書き込み失敗は kSubTelemetryFileWriteFailed を返す。成功時は書き込み件数を加算。
     * @param event_name イベント名キー。
     * @param json_payload イベント本体の JSON 文字列 (エスケープなしでそのまま書かれる)。
     * @return 成功なら空の TResult、未接続・引数不正・書き込み失敗ならエラー。
     */
    acs::TResult<void> SendTelemetry(const char* event_name,
                                      const char* json_payload) noexcept override;

    /**
     * 非同期 pump フック (本実装は何もしない)。
     *
     * @param dt 前フレームからの経過秒 (未使用)。
     */
    void               Tick(acs::f32 dt) noexcept override;

    /**
     * 現在開いている出力先パスを返す。
     *
     * @return オープン中のファイルパス (未接続なら空文字列)。
     */
    const char* Path() const noexcept { return m_Path; }

    /**
     * これまでに書き込んだイベント件数を返す。
     *
     * @return 成功した SendTelemetry の累計回数。
     */
    acs::u32    WrittenCount() const noexcept { return m_WrittenCount; }

private:
    /** ファイルオープン失敗を表すサブコード。 */
    static constexpr acs::u16 kSubTelemetryFileOpenFailed = 2101;

    /** ファイル書き込み失敗を表すサブコード。 */
    static constexpr acs::u16 kSubTelemetryFileWriteFailed = 2102;

    /**
     * URL 先頭の "file://" スキームを剥がしてパス部分を返す。
     *
     * @param server_url 入力 URL (nullptr 可)。
     * @return スキームを除いたパス (スキームが無ければ入力そのまま、nullptr なら nullptr)。
     */
    const char* StripFileScheme(const char* server_url) const noexcept;

    /**
     * src を dst へ NUL 終端付きでコピーする (固定長バッファ用)。
     *
     * @details dst が nullptr / dst_size が 0 なら何もしない。src が nullptr なら空文字列にする。
     * @param dst コピー先バッファ。
     * @param dst_size dst の容量 (NUL 終端を含む)。
     * @param src コピー元文字列 (nullptr 可)。
     */
    void CopyText(char* dst, acs::usize dst_size, const char* src) noexcept;

    /**
     * 文字列を JSON 文字列としてエスケープしながら現在のファイルへ書き込む。
     *
     * @details ", \\ を \\ でエスケープし、改行を \\n、復帰を \\r に変換して書く。text が nullptr なら空文字列を書く。
     * @param text 書き込む文字列 (nullptr 可)。
     * @return すべて書き込めたら true、書き込み失敗なら false。
     */
    bool WriteEscaped(const char* text) noexcept;

    /** 出力先 FILE* (未接続なら nullptr)。 */
    void*    m_File = nullptr;

    /** 現在開いている出力先パス (NUL 終端、未接続なら空文字列)。 */
    char     m_Path[260] = {};

    /** 成功した SendTelemetry の累計件数。 */
    acs::u32 m_WrittenCount = 0;
};

/**
 * プロセス共有の既定 FFileTelemetryBackendClient singleton を返す。
 *
 * @details InstallFileTelemetryAsDefault が gameframework の backend provider に登録する provider 実体。
 * @return ファイルバック実装の singleton への参照。
 */
acs::game::IBackendClient& GetDefaultFileTelemetryBackendClient() noexcept;

/**
 * 本実装を gameframework の既定 backend provider として登録する。
 *
 * @details
 * アプリ起動時に一度だけ呼ぶと、以降 acs::game::GetDefaultBackendClient() が
 * GetDefaultFileTelemetryBackendClient() のファイルバック実装を返すようになり、
 * 上位コードは backend 非依存に既定実装を取得できる。
 */
void InstallFileTelemetryAsDefault() noexcept;

} // namespace acs::telemetryfile
