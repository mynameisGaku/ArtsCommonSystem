// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — FSpatialAudio (Phase 3: 3D positional + HRTF binaural seam)
//
// 3D 位置情報を持つ FAudioSource3D と単一の FAudioListener (= プレイヤ耳位置) から
// 距離減衰 (attenuation) / 左右パン (stereo pan) を算出する音声空間化レイヤ。
// HRTF (Head-Related Transfer Function) によるバイノーラル化は `IHrtfRenderer`
// interface seam として隔離する。`HrtfRendererStub` は constant-power stereo
// panning + 距離減衰を**実数学** (in-repo 完結) で行い、真のバイノーラル化
// (KEMAR 256-tap convolution、~140KB の埋め込み impulse response) のみが seam
// として残る (外部 IR データ必須のため Phase 2 で別モジュールとして差し込む)。
//
// FAudioDirector との関係:
//   FAudioDirector = master / bgm / sfx の音量バスと BGM クロスフェードのみを扱う
//   「2D 混音層」。本 FSpatialAudio は FAudioDirector の上に乗る「3D 空間化前段」で、
//   listener / source を保持し、毎フレーム attenuation と pan を算出する。
//   Phase 2 で FAudioEngine と接続したとき、各 source を FAudioEngine voice に
//   バインドして、FSpatialAudio が計算した volume * pan を per-voice に書き込む。
//
// 使い方:
//   class WorldScene : public Scene {
//       acs::game::FSpatialAudio m_Spatial;
//
//       void OnEnter() noexcept override {
//           // プレイヤ耳位置を listener として登録
//           m_Spatial.SetListener({{0,0,0}, {0,0,1}, {0,1,0}});
//           // 敵から鳴る連続音 (max 20m 圏内で減衰、線形カーブ)
//           u32 src = m_Spatial.RegisterSource({5,0,3}, 20.0f,
//                                             acs::game::EAttenuationCurve::Linear);
//           m_SrcEnemy = src;
//       }
//       void OnUpdate(f32 dt) noexcept override {
//           m_Spatial.UpdateSource(m_SrcEnemy, enemy.WorldPos(), enemy.Velocity());
//           m_Spatial.Tick(dt);
//           // Phase 2: FAudioEngine voice に volume/pan を書き込む。
//           // f32 vol = m_Spatial.ComputeAttenuatedVolume(m_SrcEnemy);
//           // f32 pan = m_Spatial.ComputePan(m_SrcEnemy);
//       }
//   };
//
// 設計選択 (Phase H-3):
//   ・**listener は 1 個** (プレイヤ耳位置 = カメラに同期するのが典型)。
//     スプリットスクリーンで複数 listener が必要になったら Phase 2 で配列化。
//   ・**source は AoS (TArray of Structures)**: SoA は FAudioEngine voice
//     バインドより後段の Phase 2 で検討。現状は ~32 source 規模の想定。
//   ・**source_id は単調増加 u32** (1..): 0 = 無効 ID。再利用しない (re-use しない)
//     ので update/remove に対する stale ID 検出が単純化する。
//   ・**Attenuation 3 curve**:
//       Linear:      vol = 1 - (d / max_d)                  (素朴な線形減衰)
//       Inverse:     vol = 1 / (1 + d / ref_d)              (距離 1 倍で 0.5、自然)
//       Exponential: vol = max(0, e^(-d * rolloff))         (急峻に落ちる)
//     d > max_distance は強制 0 (= culling)。
//   ・**Pan 計算**: listener の right = up × forward。
//     dir = normalize(source.pos - listener.pos)。
//     dot(dir, right) ∈ [-1, +1] をそのまま pan として返す。
//     左手系 / 右手系どちらでも Y+up 慣習なら X+ が右になり符号一貫。
//     listener の真後ろも +0 ± で連続 (head-shadow なしの単純化)。
//   ・**Pan→ゲイン変換**: constant-power (等パワー) パン則。
//     θ = (pan+1)·π/4、left = cos θ、right = sin θ。中央でも left²+right² = 1 を
//     満たし、左右に振っても知覚音量が一定 (linear pan の中央ディップを回避)。
//   ・**HRTF seam**: `IHrtfRenderer` 経由で convolution を差し替え可能に。
//     stub は constant-power パン + 距離減衰を実数学で行う (= 実空間化)。
//     真のバイノーラル化 (KEMAR IR convolution、外部データ必須) のみ Phase 2 で
//     別モジュールとして差し込む。
//   ・**非コピー・非ムーブ**: シーン局所 instance としての所有が前提。
//
// 範囲外 (Phase H-4+ で):
//   ・実 HRTF convolution (KEMAR 256-tap、~140KB embedded IR)
//   ・Doppler shift (velocity 比率から pitch 計算)
//   ・Occlusion / obstruction (壁越し減衰、レイキャスト)
//   ・複数 listener (split-screen)
//   ・3D reverb (リバーブゾーン)
//   ・FAudioEngine voice バインド (Phase 2 で FAudioDirector と統合)
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "math/Vec.h"
#include "container/Array.h"

namespace acs::game {

// =============================================================================
// FAudioListener — プレイヤの耳位置と向き
// -----------------------------------------------------------------------------
// position: 世界座標。pan / attenuation の基準点。
// forward:  正規化済み前方ベクトル。pan 右ベクトル算出に使う。
// up:       正規化済み上方ベクトル。forward × up = right。
// 既定値は原点 + 標準 Z+ forward / Y+ up (DirectXMath 慣習)。
// =============================================================================
struct FAudioListener {
    FVec3 position = FVec3::Zero();
    FVec3 forward  = FVec3::Forward();
    FVec3 up       = FVec3::Up();
};

// =============================================================================
// FAudioSource3D — 1 個の 3D 音源
// -----------------------------------------------------------------------------
// source_id:    FSpatialAudio が払い出す一意 ID (0 = 無効、1.. = 有効)。
// position:     世界座標。
// velocity:     Doppler 用 (Phase H-3 では未使用、保持のみ)。
// volume:       基準ゲイン [0, 1]。距離減衰前の素の音量。
// max_distance: これより遠いと vol=0 (culling 距離)。
// active:       false なら attenuation / pan 計算をスキップ。
// curve:        attenuation curve 種別。
// =============================================================================
struct FAudioSource3D {
    u32                source_id    = 0;
    FVec3               position     = FVec3::Zero();
    FVec3               velocity     = FVec3::Zero();
    f32                volume       = 1.0f;
    f32                max_distance = 20.0f;
    bool               active       = true;
    // curve はメンバ末尾に置いて u8 詰めを促す。Phase H-3 では Linear がデフォルト。
    u8                 curve        = 0; // = EAttenuationCurve::Linear
};

// =============================================================================
// EAttenuationCurve — 距離 → 音量カーブ種別
// -----------------------------------------------------------------------------
// Linear:      シューティングの被弾音などゲーム的にわかりやすい
// Inverse:     物理に近い、自然な減衰 (1/r ベース)
// Exponential: 急峻に消える、足音や草擦れ音向け
// =============================================================================
enum class EAttenuationCurve : u8 {
    Linear      = 0,
    Inverse     = 1,
    Exponential = 2,
};

// =============================================================================
// IHrtfRenderer — HRTF 畳み込みの抽象 interface (seam)
// -----------------------------------------------------------------------------
// stub は単純 stereo panning のみ。Phase 2 で KEMAR 256-tap convolution を
// 別モジュールに実装し、本 interface 越しに差し替える。
//
// ライフタイム契約:
//   Init() → SetListener / ProcessSource* → Shutdown()
//   ・Init() 未呼出での ProcessSource は no-op を許容 (実装定義)。
//   ・Shutdown 後の再 Init は実装定義 (stub は再利用可)。
//
// スレッド契約:
//   ・1 instance はオーディオ専用スレッドからのみ叩く想定 (典型: WASAPI render
//     thread)。FSpatialAudio は game thread 側で source 状態を更新し、その
//     スナップショットを HRTF 側に渡す形を Phase 2 で確立する。
// =============================================================================
class IHrtfRenderer {
public:
    virtual ~IHrtfRenderer() noexcept = default;

    virtual TResult<void> Init() noexcept = 0;
    virtual void Shutdown() noexcept = 0;

    // HRTF が有効か (= 実 convolution が走るか) を上位層に通知。
    // stub は false を返す (panning だけで HRTF 効果は無い)。
    virtual bool IsHrtfEnabled() const noexcept = 0;

    // listener 状態を HRTF レイヤに伝える (Phase 2 で head orientation 角度を
    // KEMAR IR インデックスに変換する材料となる)。
    virtual void SetListener(const FAudioListener& listener) noexcept = 0;

    // mono 入力 sample_count サンプルを、interleaved stereo (LRLR...) に変換。
    // stereo_output は sample_count * 2 要素分書き込む。
    // ・source.active == false の場合の動作は実装定義 (stub は 0 埋め)。
    // ・volume / pan の事前計算は本層で済ませてもよいし、実装内で計算してもよい。
    virtual void ProcessSource(const FAudioSource3D& source,
                               const f32* mono_input,
                               f32* stereo_output,
                               u32 sample_count) noexcept = 0;

protected:
    IHrtfRenderer() noexcept = default;
    IHrtfRenderer(const IHrtfRenderer&)            = delete;
    IHrtfRenderer& operator=(const IHrtfRenderer&) = delete;
    IHrtfRenderer(IHrtfRenderer&&)                 = delete;
    IHrtfRenderer& operator=(IHrtfRenderer&&)      = delete;
};

// =============================================================================
// HrtfRendererStub — constant-power stereo panning + 距離減衰 (HRTF なし)
// -----------------------------------------------------------------------------
// 真のバイノーラル化 (HRTF convolution) のみ stub。stereo 空間化は実数学:
//   pan      = FSpatialAudio::ComputePan と同じ右ベクトル投影 (dot(dir, right))。
//   L = mono * cos((pan+1)·π/4) * volume * atten,
//   R = mono * sin((pan+1)·π/4) * volume * atten   (constant-power パン則)。
//   atten    = source.curve に応じた距離減衰 (ComputeAttenuatedVolume と同式)。
// 現状これが唯一の `IHrtfRenderer` 実装。IsHrtfEnabled() は false (= 真の HRTF
// 効果は無いが、stereo パン + 距離減衰は本物)。
// =============================================================================
class HrtfRendererStub final : public IHrtfRenderer {
public:
    HrtfRendererStub() noexcept = default;
    ~HrtfRendererStub() noexcept override = default;

    TResult<void> Init() noexcept override;
    void         Shutdown() noexcept override;
    bool         IsHrtfEnabled() const noexcept override { return false; }
    void         SetListener(const FAudioListener& listener) noexcept override;
    void         ProcessSource(const FAudioSource3D& source,
                               const f32* mono_input,
                               f32* stereo_output,
                               u32 sample_count) noexcept override;

private:
    FAudioListener m_Listener {};
    bool          m_Initialized = false;
};

// =============================================================================
// FSpatialAudio — 3D listener + source の集中管理
// -----------------------------------------------------------------------------
// Scene 局所 instance としての所有を想定。1 listener + N source を保持し、
// 毎フレーム attenuation と pan を pull で取得できる API を提供する。
// =============================================================================
class FSpatialAudio {
public:
    // ----- 初期容量 (Reserve のヒント) -----
    static constexpr u32 kInitialSourceCapacity = 16;

    FSpatialAudio() noexcept;
    ~FSpatialAudio() noexcept = default;

    FSpatialAudio(const FSpatialAudio&)            = delete;
    FSpatialAudio& operator=(const FSpatialAudio&) = delete;
    FSpatialAudio(FSpatialAudio&&)                 = delete;
    FSpatialAudio& operator=(FSpatialAudio&&)      = delete;

    // ----- listener -----
    void                 SetListener(const FAudioListener& l) noexcept;
    const FAudioListener& GetListener() const noexcept { return m_Listener; }

    // ----- source 管理 -----
    // 新規 source を登録、source_id を返す (1.. の単調増加)。
    // max_distance <= 0 はデフォルト (20m) にクランプ。
    u32  RegisterSource(FVec3 pos, f32 max_distance,
                        EAttenuationCurve curve) noexcept;
    // 位置 / 速度を更新。stale ID は no-op (警告ログのみ)。
    void UpdateSource(u32 id, FVec3 pos, FVec3 vel = FVec3::Zero()) noexcept;
    // 基準ゲイン [0, 1] を変更。範囲外は clamp + 警告。
    void SetSourceVolume(u32 id, f32 v) noexcept;
    // source を削除 (= active=false にマーク、内部 slot は使い回さない)。
    void RemoveSource(u32 id) noexcept;

    // ----- 計算結果取得 (毎フレーム pull) -----
    // listener との距離 + curve から算出した最終 volume [0, 1]。
    // 無効 ID / inactive / dist > max_distance のいずれかで 0 を返す。
    f32  ComputeAttenuatedVolume(u32 id) const noexcept;
    // -1 = 完全左、0 = 正面 / 真後ろ、+1 = 完全右。
    // 無効 ID / inactive で 0 を返す。
    f32  ComputePan(u32 id) const noexcept;

    // ----- 統計・driver -----
    u32  SourceCount() const noexcept;  // active==true の数
    void Tick(f32 dt) noexcept;         // Phase H-3 は no-op (Phase 2 で Doppler 等)
    void Clear() noexcept;              // 全 source を空に (listener は保持)

private:
    // id → index の線形検索 helper。stale ID で m_Sources.Size() を返す。
    usize FindIndex(u32 id) const noexcept;

    FAudioListener     m_Listener {};
    TArray<FAudioSource3D> m_Sources;
    u32               m_NextSourceId = 1;  // 0 = 無効予約
};

} // namespace acs::game
