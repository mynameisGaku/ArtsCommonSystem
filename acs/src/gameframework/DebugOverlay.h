// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — FDebugOverlay (state holder)
//
// FPS / メモリ / シーン名 / カスタム watch をテキストで表示するための **状態保持
// クラス**。実描画 (FSpriteBatch / ImGui / `acs::easy::DrawString` 等) は呼出し側の
// 責務。本クラスは「何を出すべきか」を保持・更新するだけで、グラフィック層に依存
// しない (テスト / Headless 環境でも動作)。
//
// 使い方:
//   class GameplayScene : public Scene {
//       acs::game::FDebugOverlay m_Overlay;
//       void OnEnter() noexcept override {
//           m_Overlay.Init();
//           m_Overlay.SetSceneName("Gameplay");
//           m_Overlay.AddWatch("Player.HP", m_HpText);     // 値文字列は caller 所有
//           m_Overlay.Show();
//       }
//       void OnUpdate(f32 dt) noexcept override {
//           m_Overlay.Tick(dt);
//           if (Input::IsKeyPressed(EKey::F3)) m_Overlay.Toggle();
//       }
//       void OnDraw() noexcept override {
//           if (!m_Overlay.IsVisible()) return;
//           char line[128];
//           EFormat(line, "FPS %.1f (avg %.1f)", m_Overlay.CurrentFps(),
//                                                m_Overlay.AverageFps());
//           DrawString(8, 8, line);
//           // ...etc, watches を順に描画
//       }
//   };
//
// 設計選択 (GameFramework Pillar H Phase 1):
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
//   ・**STL 不使用**: `acs::TArray<f32>` / `acs::TArray<Watch>` を使用、`<string>` 禁止。
//     実装側で `<cstring>` (strcmp / strlen) のみ許可。
//
// 範囲外 (Phase 2+ で):
//   ・グラフ表示 (frame time 棒グラフ / 履歴折れ線)
//   ・メモリ使用量の自動取得 (現状は SceneName と並ぶ任意ラベルとして caller が watch
//     に流し込む。Pillar A 完成後に `FAllocator` から bytes 取得して自動化予定)
//   ・国際化 (現状は ASCII / UTF-8 の生 char*、フォントレンダラ側で対応)
//   ・スレッドセーフ (Tick / Add 等は同一スレッド前提)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

class FDebugOverlay {
public:
    // ----- 公開型 -----
    // 1 行の watch エントリ。label / value とも caller 所有 (本クラスは複製しない)。
    // value バッファは caller が毎フレーム更新可 (pointer は不変であること)。
    struct Watch {
        const char* label = nullptr;
        const char* value = nullptr;
    };

    FDebugOverlay() noexcept = default;
    ~FDebugOverlay() noexcept = default;

    // 非コピー・非ムーブ (履歴 / watches の所有権を曖昧にしない)
    FDebugOverlay(const FDebugOverlay&)            = delete;
    FDebugOverlay& operator=(const FDebugOverlay&) = delete;
    FDebugOverlay(FDebugOverlay&&)                 = delete;
    FDebugOverlay& operator=(FDebugOverlay&&)      = delete;

    // ----- ライフサイクル -----
    // 内部バッファを事前確保 (60 frame 履歴)。再 Init は履歴をクリアする。
    void Init() noexcept;

    // 1 フレーム進める。dt <= 0 は履歴に push せず無視。
    // 60 frame moving average / min / max を更新する。
    void Tick(f32 dt) noexcept;

    // 履歴 / watches / scene name を初期状態に戻す。可視状態は保持。
    void Reset() noexcept;

    // ----- 可視状態 -----
    void Show() noexcept   { m_Visible = true; }
    void Hide() noexcept   { m_Visible = false; }
    void Toggle() noexcept { m_Visible = !m_Visible; }
    bool IsVisible() const noexcept { return m_Visible; }

    // ----- フレームレート計測 (Tick が更新) -----
    // 最新フレームの瞬間 fps (= 1/dt)。Tick 未呼出時は 0。
    f32 CurrentFps() const noexcept { return m_CurrentFps; }
    // 直近 60 frame の算術平均。履歴が満たない場合は有効分だけで平均。
    f32 AverageFps() const noexcept;
    // 履歴中の最小 fps。履歴空時は 0。
    f32 MinFps() const noexcept;
    // 履歴中の最大 fps。履歴空時は 0。
    f32 MaxFps() const noexcept;

    // ----- シーン名 (caller 所有のリテラル想定) -----
    void SetSceneName(const char* name) noexcept { m_SceneName = name; }
    const char* SceneName() const noexcept { return m_SceneName; }

    // ----- カスタム watch -----
    // label / value とも caller 所有。同名 label は後勝ち上書き (value 差し替え)。
    // label / value が nullptr の場合は何もしない (安全)。
    void AddWatch(const char* label, const char* value) noexcept;

    // label 一致する watch を削除 (swap-remove、順序非保持)。該当なしは no-op。
    void RemoveWatch(const char* label) noexcept;

    // 全 watch を削除。
    void ClearWatches() noexcept;

    // 登録済み watch 数。
    u32 WatchCount() const noexcept;

    // 全 watch を読み取り用に列挙。out_count に件数を書き込み、生ポインタを返す。
    // 戻り値はクラス所有の内部バッファ (`TArray<Watch>` の data)、Add/Remove/Clear
    // 呼出しまで有効。空のときは nullptr を返し、out_count = 0。
    const Watch* AllWatches(u32& out_count) const noexcept;

private:
    // 履歴は固定容量の循環バッファ。サンプル数 = 60。
    static constexpr u32 kFpsHistoryCap = 60u;

    TArray<f32>    m_FpsHistory;          // size <= kFpsHistoryCap、要素は fps 値
    u32           m_FpsIndex    = 0u;    // 次に書き込むスロット (mod kFpsHistoryCap)
    bool          m_FpsFilled   = false; // 履歴が一周したか (size == cap の意味)

    f32           m_CurrentFps  = 0.0f;
    const char*   m_SceneName   = nullptr;  // caller 所有

    TArray<Watch>  m_Watches;
    bool          m_Visible      = false;
};

} // namespace acs::game
