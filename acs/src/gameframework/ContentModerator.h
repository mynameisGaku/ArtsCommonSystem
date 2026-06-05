// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar U — FContentModerator (UGC コンテンツモデレーション seam)
//
// 役割:
//   ユーザー生成コンテンツ (UGC: チャット文 / アップロード画像 / プロフィール名)
//   を「公開していい / 警告付きで公開 / ブロック」の 3 値で判定するための seam。
//   実モデレーション API (Azure Content Safety / Google Perspective / OpenAI Moderation
//   / Hive 等) は別モジュールで差し込む前提で、本ファイルは I/F + Stub のみ。
//
//   入力 → ModerationResult{ verdict, rating, reason } の 1 段判定。
//   verdict と rating の 2 軸を持つ「二段 moderation seam」:
//     - verdict (3 値): UI フロー制御 (Allow=即公開 / Warn=確認後公開 / Block=拒否)
//     - rating  (5 値): レーティング表示・年齢ゲート (Safe〜Explicit + ハードコード Block)
//
// 設計判断:
//   ・**seam (= 純粋仮想 I/F) として抽象化**: 実 API は SaaS 課金 + 個別 SDK のため、
//     本体ビルドからは切り離して `IContentModerator` 経由で参照する。Stub は
//     NG ワード contain チェックだけの最小実装で、ビルド独立 (依存ゼロ)。
//   ・**SexualMinor_HardcodedBlock は seam を無視して常に Block**: 法令遵守の最重要
//     ガードレールとして、どんな実装でも (= 実 SDK / Stub / モック いずれでも) この
//     レーティングに分類された入力は **必ず** Block を返す契約とする。実装側で
//     "Allow に降格する" 余地を残さない。詳細は EContentRating コメント参照。
//   ・**所有しない const char***: `<string>` 不使用 (ACS 規約)。`ModerationResult::reason`
//     は static literal or 実装内部の static thread_local バッファを指す。寿命は
//     「次の Moderate*() 呼び出しまで」を保証する。
//   ・**TResult<T, FErrorCode> で例外なし**: ACS 全体方針に沿う。Stub は同期で常に
//     Ok(...) を返し、エラーは API 通信失敗の seam 側でのみ発生。
//   ・**Tick(dt) 必須**: 非同期モデレーション (= REST 結果ポーリング) を Bridge 側に
//     畳み込めるよう、FSteamworksBridge と同じ規約を採用。Stub では no-op。
//   ・**Stub は static singleton**: `GetModeratorStub()` で取得。実 SDK 未統合ビルドでも
//     `m_Moderator = &acs::game::GetModeratorStub();` だけでコンパイル可能。
//
// 範囲外:
//   ・実 SDK 実装 (Azure / Google / OpenAI / Hive)。
//   ・画像 NSFW スコア (= 画像分類器ローカル推論)。
//   ・多言語対応 (日本語 / 中国語 / アラビア語の NG 単語辞書)。
//   ・ユーザー単位レート制限・通報フロー・履歴 (FBackendClient と連携で別レイヤ)。
//
// ACS 規約:
//   ・STL 不使用 / `<string>` 不使用 / 例外不使用 / 全 noexcept
//   ・非コピー・非ムーブ (state を 1 箇所にとどめる)
//
// 参考:
//   ・FLlmSafetyPipeline (text validation pattern)
//   ・FSteamworksBridge   (seam + Stub singleton pattern)
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"

namespace acs::game {

/** Stub による未実装を表す subcode (ErrCategory::Generic 配下)。 */
inline constexpr u16 kSubContentModeratorNotImplemented = 1301;

/** nullptr / size==0 等の不正引数を表す subcode (ErrCategory::Generic 配下)。 */
inline constexpr u16 kSubContentModeratorBadArgument    = 1302;

/**
 * UI フロー制御のための 3 値モデレーション判定。
 */
enum class EModerationVerdict : u8 {
    /** そのまま公開して良い。 */
    Allow = 0,

    /** 公開前にユーザー or モデレーターの確認が必要 (borderline)。 */
    Warn  = 1,

    /** 公開してはならない (法令違反 / コミュニティガイドライン違反)。 */
    Block = 2,
};

/**
 * レーティング軸 (年齢ゲート / Storefront age rating 表示用)。
 *
 * @details
 * SexualMinor_HardcodedBlock だけは特別で、実装 (Stub / 実 SDK / モック) がこの
 * レーティングを返した場合 verdict は必ず Block にハードコードする。Allow へ降格する
 * 経路を残さず、法令遵守上の最重要ガードレールをシーム層で保証する。
 */
enum class EContentRating : u8 {
    /** 全年齢。 */
    Safe                       = 0,

    /** 軽度の暴力 / 風刺 等 (PG 相当)。 */
    Mild                       = 1,

    /** 成人向け要素を含む (17+ 相当)。 */
    Mature                     = 2,

    /** 露骨な暴力 / 性的描写 (18+ 相当、要 age gate)。 */
    Explicit                   = 3,

    /** 児童性的搾取に該当。実装にかかわらず verdict は常に Block。 */
    SexualMinor_HardcodedBlock = 4,
};

/**
 * 1 回の Moderate*() 呼び出しの結果。
 *
 * @details
 * reason は文字列を所有しない (静的リテラル or 内部 static バッファを指す)。寿命は
 * 「次の Moderate*() 呼び出しまで」で、呼び出し側はその前に消費する必要がある。
 */
struct ModerationResult {
    /** UI フロー制御の 3 値判定。 */
    EModerationVerdict verdict = EModerationVerdict::Allow;

    /** レーティング軸の判定。 */
    EContentRating     rating  = EContentRating::Safe;

    /** 判定理由 (Warn/Block で有効、Allow では nullptr 可。非所有)。 */
    const char*       reason  = nullptr;
};

/**
 * UGC モデレーションの抽象 seam (二段 moderation)。
 *
 * @details
 * テキスト / 画像 / ユーザー名をそれぞれ独立にモデレーションする 3 つの入口を持つ。
 * 実装側 (Stub / 実 SDK) は ModerateText/Image/UserName を override し、Tick(dt) で
 * 非同期処理 (REST ポーリング等) を回す。verdict (UI フロー制御) と rating (年齢ゲート)
 * の 2 軸を返す。
 */
class IContentModerator {
public:
    /** 既定構築する。 */
    IContentModerator() noexcept = default;

    /** 派生クラスを正しく破棄するための仮想デストラクタ。 */
    virtual ~IContentModerator() noexcept = default;

    /** コピー禁止 (state を 1 箇所にとどめるため)。 */
    IContentModerator(const IContentModerator&)            = delete;

    /** コピー代入も禁止。 */
    IContentModerator& operator=(const IContentModerator&) = delete;

    /** ムーブ禁止。 */
    IContentModerator(IContentModerator&&)                 = delete;

    /** ムーブ代入も禁止。 */
    IContentModerator& operator=(IContentModerator&&)      = delete;

    /**
     * テキストをモデレーションする (チャット / プロフィール bio / レビュー文 等)。
     *
     * @param user_id 通報履歴・レピュテーション参照用の id (Stub では未使用、nullptr 可)。
     * @param text 判定対象テキスト。nullptr / 空文字の扱いは実装依存 (Stub は Allow)。
     * @return 判定結果、または API 通信失敗時のエラー。
     */
    virtual TResult<ModerationResult> ModerateText(const char* user_id, const char* text) noexcept = 0;

    /**
     * 画像をモデレーションする (アバター / スクショ投稿 / カスタムバナー 等)。
     *
     * @param user_id 通報履歴・レピュテーション参照用の id (Stub では未使用、nullptr 可)。
     * @param image_data 呼出側所有の生バイト列 (PNG / JPEG / WebP 等)。
     * @param size image_data のバイト数。0 / nullptr は BadArgument。
     * @return 判定結果、または不正引数 / API 失敗時のエラー。
     */
    virtual TResult<ModerationResult> ModerateImage(const char* user_id, const u8* image_data, u64 size) noexcept = 0;

    /**
     * ユーザー名をモデレーションする (アカウント作成 / 改名時)。
     *
     * @details 通常 text より厳しい (短い文字列内の侮辱・なりすまし検出を強化)。Stub では
     * text と同じ NG ワード辞書を流用する。
     * @param name 判定対象のユーザー名。
     * @return 判定結果、または API 失敗時のエラー。
     */
    virtual TResult<ModerationResult> ModerateUserName(const char* name) noexcept = 0;

    /**
     * 非同期処理ポンプ (REST 結果ポーリング / 内部キュー dispatch)。
     *
     * @details ゲームループから毎フレーム呼ぶこと。Stub は no-op。
     * @param dt 経過秒。
     */
    virtual void Tick(f32 dt) noexcept = 0;

    /**
     * API を発火できる状態かを返す。
     *
     * @return 実 SDK 接続中 (同期 / 非同期どちらでも発火可) なら true。Stub は常に true。
     */
    virtual bool IsAvailable() const noexcept = 0;
};

/**
 * 依存ゼロのデフォルト実装 (ローカルテキストフィルタ + 肌色比率画像判定)。
 *
 * @details
 * text/username は NG 単語リストに対し (a) 生文字列の ASCII 大小文字非感受 contain と
 * (b) 正規化文字列 (小文字化 + leet 置換 + 空白/記号除去) の contain の 2 経路で照合し、
 * `f u c k` / `f.u.c.k` / `sh1t` 風の変種も捕捉する。SexualMinor 関連 keyword にヒット
 * した場合は必ず verdict=Block / rating=SexualMinor_HardcodedBlock を返す。image は外部
 * SDK 無しで肌色比率 heuristic (Kovac et al.) を使い、露出度から verdict/rating を判定する
 * (size==0 / nullptr は BadArgument)。Tick / IsAvailable は no-op (常に true)。
 */
class ContentModeratorStub final : public IContentModerator {
public:
    /** 既定構築する。 */
    ContentModeratorStub() noexcept = default;

    /** 破棄する。 */
    ~ContentModeratorStub() noexcept override = default;

    /**
     * テキストを NG ワード辞書で判定する。
     *
     * @param user_id Stub では未使用 (nullptr 可)。
     * @param text 判定対象テキスト (nullptr / 空文字は Allow)。
     * @return 判定結果 (常に Ok)。
     */
    TResult<ModerationResult> ModerateText(const char* user_id, const char* text) noexcept override;

    /**
     * 画像をローカルデコードし肌色比率 heuristic で判定する。
     *
     * @param user_id Stub では未使用 (nullptr 可)。
     * @param image_data 判定対象の生バイト列。
     * @param size image_data のバイト数 (0 / nullptr / デコード不能は BadArgument)。
     * @return 判定結果、または不正引数 / デコード失敗時のエラー。
     */
    TResult<ModerationResult> ModerateImage(const char* user_id, const u8* image_data, u64 size) noexcept override;

    /**
     * ユーザー名を text と同じ NG ワード辞書で判定する。
     *
     * @param name 判定対象のユーザー名。
     * @return 判定結果 (常に Ok)。
     */
    TResult<ModerationResult> ModerateUserName(const char* name) noexcept override;

    /**
     * 非同期キューを持たないため no-op。
     *
     * @param dt 経過秒 (未使用)。
     */
    void                     Tick(f32 dt) noexcept override;

    /**
     * 常に利用可能であることを返す。
     *
     * @return 常に true (依存ゼロで即可用)。
     */
    bool                     IsAvailable() const noexcept override { return true; }

    /**
     * デコード済み RGBA8 ピクセル列を直接 NSFW 判定する。
     *
     * @details エンジンが CPU 側にテクスチャを持つ場合に再デコードを避けられる。肌色比率
     * heuristic (Kovac et al.) で露出度を推定する。
     * @param rgba RGBA8 ピクセル列 (4ch interleaved)。
     * @param width 画像幅 (px)。0 は BadArgument。
     * @param height 画像高さ (px)。0 は BadArgument。
     * @return 判定結果、または不正引数時のエラー。
     */
    TResult<ModerationResult> ModerateImageRgba8(const u8* rgba, u32 width, u32 height) noexcept;

private:
    /**
     * 肌色比率を verdict/rating のしきい値で判定する (ModerateImage / RGBA 直接判定で共用)。
     *
     * @param skin_ratio 不透明ピクセルに対する肌色ピクセルの比率 [0,1]。
     * @return しきい値に応じた判定結果 (常に Ok)。
     */
    TResult<ModerationResult> ClassifyImageRatio(f32 skin_ratio) const noexcept;
};

/**
 * 全コードで共有できる Stub の static singleton を返す。
 *
 * @details 実 SDK 実装が DI される前のデフォルト。FSteamworksBridge::GetStub() と同じ規約。
 * @return 共有 ContentModeratorStub への参照。
 */
IContentModerator& GetModeratorStub() noexcept;

} // namespace acs::game
