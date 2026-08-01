// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — CDebugOverlay (state holder)
//
// FPS / メモリ / シーン名 / カスタム watch をテキストで表示するための **状態保持
// クラス**。実描画 (FSpriteBatch / ImGui / `acs::easy::DrawString` 等) は呼出し側の
// 責務。本クラスは「何を出すべきか」を保持・更新するだけで、グラフィック層に依存
// しない (テスト / Headless 環境でも動作)。
//
// 使い方:
//   class FGameplayScene : public AScene {
//       acs::game::CDebugOverlay m_Overlay;
//       void OnEnter() noexcept override {
//           m_Overlay.Init();
//           m_Overlay.SetSceneName("Gameplay");
//           m_Overlay.AddWatch("Player.HP", m_HpText);     // 値文字列は caller 所有
//           m_Overlay.Show();
//       }
//       void OnUpdate(f32 dt) noexcept override {
//           m_Overlay.Tick(dt);
//           if (FInput::IsKeyPressed(EKey::F3)) m_Overlay.Toggle();
//       }
//       void OnDraw() noexcept override {
//           if (!m_Overlay.IsVisible()) return;
//           char line[128];
//           Format(line, "FPS %.1f (avg %.1f)", m_Overlay.CurrentFps(),
//                                                m_Overlay.AverageFps());
//           DrawString(8, 8, line);
//           // ...etc, watches を順に描画
//       }
//   };
//
// 設計選択 (GameFramework Pillar H):
//   ・**state holder のみ**: 描画 API を持ち込むと Diligent / DX12 raw / Headless で
//     スキニングが分かれてしまうので、ここでは値の保持と算出だけに留める。
//   ・**60 frame moving average**: dt から fps = 1/dt を算出し、循環バッファ 60 個を
//     保持。AverageFps() は履歴の算術平均、Min/Max も履歴から線形走査で取る。
//     最初の数フレームは履歴が満たない (size < cap) なので、有効分だけで平均する。
//   ・**dt <= 0 guard**: dt == 0 (一時停止) や負値は履歴に push しない (発散防止)。
//   ・**watches: pointer-only storage**: label / value はいずれも caller 所有
//     (typically 文字列リテラル or 別所有のバッファ)。本クラスは複製しない。caller が
//     value 用バッファを毎フレーム更新する想定 (snprintf 等)。pointer aliasing は
//     許容、label 重複は後勝ち上書き (重複検出ロジックは strcmp 線形走査)。
//   ・**RemoveWatch / WatchCount**: label を strcmp で検索して swap-remove。順序は
//     保証しない (描画レイアウトは caller が安定化する責務)。
//   ・**非コピー・非ムーブ**: 履歴バッファと watches 列の所有権を曖昧にしないため。
//   ・**STL 不使用**: `acs::TArray<f32>` / `acs::TArray<FWatch>` を使用、`<string>` 禁止。
//     実装側で `<cstring>` (strcmp / strlen) のみ許可。
//
// 範囲外 (本クラスでは持たない):
//   ・グラフ表示 (frame time 棒グラフ / 履歴折れ線)
//   ・メモリ使用量の自動取得 (現状は SceneName と並ぶ任意ラベルとして caller が watch
//     に流し込む)
//   ・国際化 (現状は ASCII / UTF-8 の生 char*、フォントレンダラ側で対応)
//   ・スレッドセーフ (Tick / Add 等は同一スレッド前提)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

/**
 * FPS / シーン名 / カスタム watch を保持・更新する状態保持クラス。
 *
 * @details
 * 実描画 (FSpriteBatch / ImGui / DrawString 等) は呼出し側の責務で、本クラスは
 * 「何を出すべきか」を保持・更新するだけでグラフィック層に依存しない (テスト /
 * Headless でも動作)。dt から瞬間 fps を算出し 60 frame の循環バッファで平均 /
 * 最小 / 最大を計測する。watch は label / value とも caller 所有のポインタを保持
 * するだけで複製しない。非コピー・非ムーブ。
 */
class CDebugOverlay {
public:
    /**
     * 1 行の watch エントリ。
     *
     * @details label / value とも caller 所有 (本クラスは複製しない)。value バッファは
     * caller が毎フレーム更新してよい (pointer は不変であること)。
     */
    struct FWatch {
        /** 表示ラベル (caller 所有、非所有参照)。 */
        const char* label = nullptr;

        /** 表示値の文字列 (caller 所有、非所有参照)。 */
        const char* value = nullptr;
    };

    /** 空状態で構築する (内部バッファは Init で確保)。 */
    CDebugOverlay() noexcept = default;

    /** 破棄する (TArray が内部バッファを解放)。 */
    ~CDebugOverlay() noexcept = default;

    /** コピー禁止 (履歴 / watches の所有権を曖昧にしないため)。 */
    CDebugOverlay(const CDebugOverlay&)            = delete;

    /** コピー代入も禁止。 */
    CDebugOverlay& operator=(const CDebugOverlay&) = delete;

    /** ムーブ禁止 (履歴 / watches の所有権を曖昧にしないため)。 */
    CDebugOverlay(CDebugOverlay&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CDebugOverlay& operator=(CDebugOverlay&&)      = delete;

    /**
     * 内部バッファを事前確保する。
     *
     * @details 60 frame 履歴ぶんの capacity を Reserve し、各カウンタを 0 始まりに戻す。
     * 再 Init は履歴をクリアする。
     */
    void Init() noexcept;

    /**
     * 1 フレーム進めて fps 履歴を更新する。
     *
     * @details dt <= 0 (一時停止 / 負値) は履歴汚染を避けるため push せず無視する。
     * 60 frame の循環バッファに fps = 1/dt を書き込み、瞬間 fps を更新する。
     * @param dt 前フレームからの経過秒。
     */
    void Tick(f32 dt) noexcept;

    /**
     * 履歴 / watches / scene name を初期状態に戻す。
     *
     * @details 可視状態 (m_Visible) は意図的に保持する (Reset で誤って非表示にしない)。
     */
    void Reset() noexcept;

    /** オーバーレイを可視にする。 */
    void Show() noexcept   { m_Visible = true; }

    /** オーバーレイを非可視にする。 */
    void Hide() noexcept   { m_Visible = false; }

    /** 可視状態を反転する。 */
    void Toggle() noexcept { m_Visible = !m_Visible; }

    /**
     * 可視状態を返す。
     *
     * @return 可視なら true。
     */
    bool IsVisible() const noexcept { return m_Visible; }

    /**
     * 最新フレームの瞬間 fps を返す。
     *
     * @return 直近 Tick で算出した 1/dt。Tick 未呼出時は 0。
     */
    f32 CurrentFps() const noexcept { return m_CurrentFps; }

    /**
     * 直近 60 frame の平均 fps を返す。
     *
     * @details 履歴が満たない場合は有効分だけで算術平均する。
     * @return 平均 fps。履歴空時は 0。
     */
    f32 AverageFps() const noexcept;

    /**
     * 履歴中の最小 fps を返す。
     *
     * @return 履歴を線形走査した最小 fps。履歴空時は 0。
     */
    f32 MinFps() const noexcept;

    /**
     * 履歴中の最大 fps を返す。
     *
     * @return 履歴を線形走査した最大 fps。履歴空時は 0。
     */
    f32 MaxFps() const noexcept;

    /**
     * シーン名を設定する。
     *
     * @param name 表示するシーン名 (caller 所有のリテラル想定、非所有参照)。
     */
    void SetSceneName(const char* name) noexcept { m_SceneName = name; }

    /**
     * 設定済みのシーン名を返す。
     *
     * @return シーン名 (未設定なら nullptr)。
     */
    const char* SceneName() const noexcept { return m_SceneName; }

    /**
     * watch を追加する (同名 label は後勝ちで value を差し替え)。
     *
     * @details label / value とも caller 所有で複製しない。同名 label があれば value だけ
     * 上書きする。label / value のいずれかが nullptr なら何もしない (安全)。
     * @param label 表示ラベル (caller 所有)。
     * @param value 表示値の文字列 (caller 所有)。
     */
    void AddWatch(const char* label, const char* value) noexcept;

    /**
     * label 一致する watch を削除する。
     *
     * @details swap-remove のため順序は保持しない。該当なしは no-op。
     * @param label 削除する watch のラベル。
     */
    void RemoveWatch(const char* label) noexcept;

    /** 全 watch を削除する。 */
    void ClearWatches() noexcept;

    /**
     * 登録済み watch 数を返す。
     *
     * @return watch の件数。
     */
    u32 WatchCount() const noexcept;

    /**
     * 全 watch を読み取り用に列挙する。
     *
     * @details 戻り値はクラス所有の内部バッファ (TArray<FWatch> の data) を指し、
     * Add/Remove/Clear 呼出しまで有効。
     * @param out_count watch 件数を書き込む先。
     * @return watch 配列の先頭ポインタ。空のときは nullptr (out_count = 0)。
     */
    const FWatch* AllWatches(u32& out_count) const noexcept;

private:
    /** fps 履歴の固定容量 (循環バッファのサンプル数)。 */
    static constexpr u32 kFpsHistoryCap = 60u;

    /** fps 履歴バッファ (size <= kFpsHistoryCap、要素は fps 値)。 */
    TArray<f32>    m_FpsHistory;

    /** 次に書き込むスロット (mod kFpsHistoryCap)。 */
    u32           m_FpsIndex    = 0u;

    /** 履歴が一周したか (true なら size == cap で上書きモード)。 */
    bool          m_bFpsFilled   = false;

    /** 最新フレームの瞬間 fps。 */
    f32           m_CurrentFps  = 0.0f;

    /** 表示するシーン名 (caller 所有、非所有参照)。 */
    const char*   m_SceneName   = nullptr;

    /** 登録済み watch 列 (label / value とも非所有参照)。 */
    TArray<FWatch>  m_Watches;

    /** 可視フラグ (false なら描画側が表示しない想定)。 */
    bool          m_Visible      = false;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FDebugOverlay = CDebugOverlay;

} // namespace acs::game
