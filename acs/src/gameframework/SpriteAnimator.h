// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar C — SpriteAnimator (Phase 3 完結)
//
// 「現在フレーム index を時間から算出する」ロジックだけを担うコンポーネント。
// 画像 asset / 描画 API には一切触れず、利用者が CurrentFrame() を取り出して
// 自分の Sprite/Quad に適用する責務分離 = テスタブルかつ asset 層と独立にビルド可能。
//
// 機能:
//   ・PlayMode = Loop / PingPong / Once
//   ・Play / Pause / Stop / Tick / SetCurrentFrame / SetFps
//   ・PingPong は端で折り返す (例: 4 frame → 0,1,2,3,2,1,0,1,2,3,…)
//   ・Once は末尾で停止し IsFinished() = true
//   ・フレームイベント: 指定 frame に入った瞬間 1 度だけ関数ポインタを呼ぶ
//     (Loop の周回毎に再発火、std::function は使わない)
//
// 使い方:
//   SpriteAnimator anim;
//   anim.Init(/*frame_count=*/8, /*fps=*/12.0f, PlayMode::Loop);
//   anim.AddFrameEvent(4, [](void* ud) noexcept {
//       static_cast<MyActor*>(ud)->OnFootstep();
//   }, this);
//   anim.Play();
//   // 毎フレーム:
//   anim.Tick(dt);
//   u32 idx = anim.CurrentFrame(); // ← Sprite/Quad の UV 切替に使う
//
// 設計判断:
//   ・asset 非依存にすることで Pillar C (フレーム時間管理) と Pillar Q (視覚世界,
//     スプライトシート) の関心を分離。SpriteAnimator は時間→index 関数として
//     ユニットテスト可能。
//   ・std::function を避けるため frame event のコールバックは関数ポインタ + user 引数。
//     Lambda は capture 無しに限る (= ACS の関数ポインタ規約と整合)。
//   ・dt 駆動でラップアラウンドを正しく扱うため _elapsed は周期長を超えないよう
//     wrap (Loop/PingPong) または末尾に固定 (Once)。長時間プレイで _elapsed が
//     f32 精度を失う事故を防ぐ。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "math/Math.h"

namespace acs::game {

enum class PlayMode : u8 {
    Loop     = 0,  // 0→N-1→0→N-1→… と循環
    PingPong = 1,  // 0→N-1→0→N-1→… と折り返し (両端 1 度ずつ)
    Once     = 2,  // 0→N-1 で停止
};

class SpriteAnimator {
public:
    using FrameEventFn = void(*)(void* user) noexcept;

    SpriteAnimator() noexcept = default;
    ~SpriteAnimator() noexcept = default;

    // ACS 規約: アニメ状態を不意に複製しないよう非コピー・非ムーブ
    SpriteAnimator(const SpriteAnimator&)            = delete;
    SpriteAnimator& operator=(const SpriteAnimator&) = delete;
    SpriteAnimator(SpriteAnimator&&)                 = delete;
    SpriteAnimator& operator=(SpriteAnimator&&)      = delete;

    // 初期化。frame_count==0 や fps<=0 は安全な既定にフォールバック (frame=1, fps=1)。
    // 既存の frame event は維持される (= Reset 用途では再 Init せず Stop を使う)。
    void Init(u32 frame_count, f32 fps, PlayMode mode = PlayMode::Loop) noexcept;

    // 再生制御。Play は現在位置から再開、Stop は先頭に戻して停止、
    // Pause は位置維持で停止。
    void Play()  noexcept;
    void Pause() noexcept;
    void Stop()  noexcept;
    bool IsPlaying() const noexcept { return _playing; }

    // 経過時間を流す。dt <= 0 は no-op (= 巻き戻し非対応)。
    void Tick(f32 dt) noexcept;

    u32  CurrentFrame()    const noexcept { return _current_frame; }
    bool IsFinished()      const noexcept { return _finished; }
    // [0, 1] のフレーム進行率 (Loop/PingPong は周期内、Once は 0→1 の単調進行)
    f32  NormalizedTime()  const noexcept;

    // 強制シーク。frame_count を超える値は末尾にクランプ。
    // _elapsed は新 frame の先頭時刻に合わせて再計算され、
    // 同じ frame に再進入したものとして frame event は次回境界跨ぎで発火する。
    void SetCurrentFrame(u32 i) noexcept;

    // fps を差し替える。負値は 0 にクランプ (= 0 fps はフリーズと等価)。
    // _elapsed は維持されるが新しい fps で再評価されるため、見かけ上の
    // CurrentFrame は次 Tick から変わる。
    void SetFps(f32 fps) noexcept;

    f32      Fps()        const noexcept { return _fps; }
    u32      FrameCount() const noexcept { return _frame_count; }
    PlayMode Mode()       const noexcept { return _mode; }

    // frame に進入した瞬間 1 度だけ cb を呼ぶ。同じ frame に複数登録すれば
    // 登録順に発火。重複は許容 (= 利用者の責任で重複を避ける)。
    // frame が frame_count を超える登録は無視 (= debug ビルドでも crash させない)。
    // cb == nullptr は無視。
    void AddFrameEvent(u32 frame, FrameEventFn cb, void* user) noexcept;

private:
    struct FrameEvent {
        u32          frame = 0;
        FrameEventFn cb    = nullptr;
        void*        user  = nullptr;
    };

    // _elapsed から周期 (Loop / PingPong) を考慮した「論理 frame index」を計算。
    // 戻り値は [0, frame_count) に収まる。
    u32 ComputeFrame(f32 elapsed) const noexcept;

    // _current_frame が prev → next に変化したとき、間に挟まる全 frame の
    // event を発火する。Loop での周回 (next < prev) もハンドルする。
    void FireEventsBetween(u32 prev, u32 next, bool wrapped) noexcept;

    u32       _frame_count   = 1;
    f32       _fps           = 1.0f;
    PlayMode  _mode          = PlayMode::Loop;
    f32       _elapsed       = 0.0f;
    u32       _current_frame = 0;
    bool      _playing       = false;
    bool      _finished      = false;

    Array<FrameEvent> _events;
};

} // namespace acs::game
