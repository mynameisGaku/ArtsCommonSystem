// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar U Phase 2 — ContentModerator (UGC コンテンツモデレーション seam)
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
//   ・**Result<T, ErrorCode> で例外なし**: ACS 全体方針に沿う。Stub は同期で常に
//     Ok(...) を返し、エラーは API 通信失敗の seam 側でのみ発生。
//   ・**Tick(dt) 必須**: 非同期モデレーション (= REST 結果ポーリング) を Bridge 側に
//     畳み込めるよう、SteamworksBridge と同じ規約を採用。Stub では no-op。
//   ・**Stub は static singleton**: `GetModeratorStub()` で取得。実 SDK 未統合ビルドでも
//     `_moderator = &acs::game::GetModeratorStub();` だけでコンパイル可能。
//
// 範囲外 (Phase 2+ で):
//   ・実 SDK 実装 (Azure / Google / OpenAI / Hive)。
//   ・画像 NSFW スコア (= 画像分類器ローカル推論)。
//   ・多言語対応 (日本語 / 中国語 / アラビア語の NG 単語辞書)。
//   ・ユーザー単位レート制限・通報フロー・履歴 (BackendClient と連携で別レイヤ)。
//
// ACS 規約:
//   ・STL 不使用 / `<string>` 不使用 / 例外不使用 / 全 noexcept
//   ・非コピー・非ムーブ (state を 1 箇所にとどめる)
//
// 参考:
//   ・LlmSafetyPipeline (text validation pattern)
//   ・SteamworksBridge   (seam + Stub singleton pattern)
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"

namespace acs::game {

// ---- ErrorCode subcode 定義 (ErrCategory::Generic 配下) ----------------------
// SteamworksBridge と同じく `Generic + 安定 subcode` で未実装/未初期化を表現。
inline constexpr u16 kSubContentModeratorNotImplemented = 1301;  // Stub による未実装
inline constexpr u16 kSubContentModeratorBadArgument    = 1302;  // nullptr / size==0 等

// =============================================================================
// EModerationVerdict — UI フロー制御の 3 値判定
// -----------------------------------------------------------------------------
//   Allow = そのまま公開して良い
//   Warn  = 公開前にユーザー or モデレーターの確認が必要 (= "borderline")
//   Block = 公開してはならない (法令違反 / コミュニティガイドライン違反)
// =============================================================================
enum class EModerationVerdict : u8 {
    Allow = 0,
    Warn  = 1,
    Block = 2,
};

// =============================================================================
// EContentRating — レーティング軸 (年齢ゲート / Storefront age rating 表示用)
// -----------------------------------------------------------------------------
//   Safe                       = 全年齢
//   Mild                       = 軽度の暴力 / 風刺 等 (PG 相当)
//   Mature                     = 成人向け要素を含む (17+ 相当)
//   Explicit                   = 露骨な暴力 / 性的描写 (18+ 相当、要 age gate)
//   SexualMinor_HardcodedBlock = 児童性的搾取に該当 → **実装にかかわらず常に Block**
//
// 最後の `SexualMinor_HardcodedBlock` だけは特別: 実装 (Stub / 実 SDK / モック)
// がこのレーティングを返した場合、verdict は **必ず Block にハードコード** する。
// "Allow に降格する" 経路を残さないことで、法令遵守上の最重要ガードレールを
// シーム層 (= 本ファイル) で保証する。
// =============================================================================
enum class EContentRating : u8 {
    Safe                       = 0,
    Mild                       = 1,
    Mature                     = 2,
    Explicit                   = 3,
    SexualMinor_HardcodedBlock = 4,
};

// =============================================================================
// ModerationResult — 1 回の Moderate*() 呼び出しの結果
// -----------------------------------------------------------------------------
// `reason` は文字列を所有しない (= 静的リテラル or 内部 static バッファを指す)。
// 寿命は「次の Moderate*() 呼び出しまで」。呼び出し側はその前に消費すること。
// =============================================================================
struct ModerationResult {
    EModerationVerdict verdict = EModerationVerdict::Allow;
    EContentRating     rating  = EContentRating::Safe;
    const char*       reason  = nullptr;   // Warn/Block で有効、Allow では nullptr 可
};

// =============================================================================
// IContentModerator — 抽象 seam (二段 moderation)
// -----------------------------------------------------------------------------
// テキスト / 画像 / ユーザー名 をそれぞれ独立にモデレーションする 3 つの入口。
// 実装側 (Stub / 実 SDK) は ModerateText/Image/UserName をオーバーライドし、
// `Tick(dt)` で非同期処理 (REST ポーリング等) を回す。
//
// 典型使用:
//   class Game {
//       acs::game::IContentModerator* _mod = nullptr;
//       void OnStart() noexcept override {
//           _mod = &acs::game::GetModeratorStub();
//       }
//       void OnTick(f32 dt) noexcept override {
//           _mod->Tick(dt);
//       }
//       bool TryPostChat(const char* user_id, const char* text) noexcept {
//           auto r = _mod->ModerateText(user_id, text);
//           if (!r) { LogError(); return false; }
//           if (r.Value().verdict == EModerationVerdict::Block) { ShowBlockedUi(r.Value().reason); return false; }
//           if (r.Value().verdict == EModerationVerdict::Warn ) { ShowWarnUi (r.Value().reason); /* 続行可 */ }
//           PostChat(text);
//           return true;
//       }
//   };
// =============================================================================
class IContentModerator {
public:
    IContentModerator() noexcept = default;
    virtual ~IContentModerator() noexcept = default;

    // 非コピー・非ムーブ (state を 1 箇所にとどめる)
    IContentModerator(const IContentModerator&)            = delete;
    IContentModerator& operator=(const IContentModerator&) = delete;
    IContentModerator(IContentModerator&&)                 = delete;
    IContentModerator& operator=(IContentModerator&&)      = delete;

    // テキストモデレーション (チャット / プロフィール bio / レビュー文 等)。
    // `user_id` は通報履歴・レピュテーション参照用 (Stub では未使用、nullptr 可)。
    // `text` が nullptr / 空文字の場合の扱いは実装依存 (Stub は Allow)。
    virtual Result<ModerationResult> ModerateText(const char* user_id, const char* text) noexcept = 0;

    // 画像モデレーション (アバター / スクリーンショット投稿 / カスタムバナー 等)。
    // `image_data` は呼び出し側が所有する生バイト列 (PNG / JPEG / WebP 等の任意フォーマット)。
    // size==0 / nullptr は BadArgument エラーで返す (Stub も同様)。
    virtual Result<ModerationResult> ModerateImage(const char* user_id, const u8* image_data, u64 size) noexcept = 0;

    // ユーザー名モデレーション (アカウント作成 / 改名時)。
    // 通常 text モデレーションより厳しい (= 短い文字列内の侮辱・なりすまし検出を強化)。
    // Stub では text と同じ NG ワード辞書を流用する。
    virtual Result<ModerationResult> ModerateUserName(const char* name) noexcept = 0;

    // 非同期処理ポンプ (REST 結果ポーリング / 内部キュー dispatch)。
    // ゲームループから毎フレーム呼ぶこと。Stub は no-op。
    virtual void Tick(f32 dt) noexcept = 0;

    // 実 SDK 接続中 (= 同期 / 非同期どちらでも API を発火できる状態) なら true。
    // Stub は常に true (= 依存ゼロで即可用)。
    virtual bool IsAvailable() const noexcept = 0;
};

// =============================================================================
// ContentModeratorStub — 依存ゼロのデフォルト実装 (Phase 1)
// -----------------------------------------------------------------------------
// ・text: 内部の小さな NG 単語リスト (kBlockedWords[]) で contain チェック。
//   ヒット時は verdict=Block / rating=Mature が基本。但し `SexualMinor` 関連
//   keyword (kHardcodedBlockWords[]) にヒットした場合は **必ず**
//   verdict=Block / rating=SexualMinor_HardcodedBlock を返す (= ハードコード block)。
// ・image: 全部 Allow (TODO Phase 2 で NSFW 分類器導入)。size==0 / nullptr は BadArgument。
// ・username: text と同じ辞書を使用。
// ・Tick(dt) / IsAvailable() は no-op (常に true)。
// =============================================================================
class ContentModeratorStub final : public IContentModerator {
public:
    ContentModeratorStub() noexcept = default;
    ~ContentModeratorStub() noexcept override = default;

    Result<ModerationResult> ModerateText(const char* user_id, const char* text) noexcept override;
    Result<ModerationResult> ModerateImage(const char* user_id, const u8* image_data, u64 size) noexcept override;
    Result<ModerationResult> ModerateUserName(const char* name) noexcept override;
    void                     Tick(f32 dt) noexcept override;
    bool                     IsAvailable() const noexcept override { return true; }
};

// =============================================================================
// GetModeratorStub() — 全コードで共有できる static singleton
// -----------------------------------------------------------------------------
// 実 SDK 実装が DI される前のデフォルト。SteamworksBridge::GetStub() と同じ規約。
// =============================================================================
IContentModerator& GetModeratorStub() noexcept;

} // namespace acs::game
