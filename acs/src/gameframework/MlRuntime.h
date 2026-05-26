// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar U — MlRuntime / Upscaler (AI/ML & Super-Resolution seams)
//
// 役割:
//   Pillar U (AI & ML 統合) のうち、外部 ML ランタイム (ONNX Runtime / DirectML /
//   NNAPI / CoreML / TensorFlow Lite 等) と、フレームのアップスケーラ
//   (AMD FSR / NVIDIA DLSS / Intel XeSS) を **interface seam** として隔離する。
//
//   両者ともゲームロジック側からは `IMlRuntime&` / `IUpscaler&` 越しに叩く前提で、
//   具象 backend (Onnx / DirectML / FSR2 / DLSS / XeSS) は **後段の独立モジュール**
//   ("MlOnnx" 等) で SDK 取り込みと差し替えを行う。本 header はその「窓口」だけを
//   定義するスケルトンで、Phase U-1 では stub 実装のみを提供する。
//
// なぜ seam で隔離するか (= ここに具象を書かない理由):
//   1. **決定論ゾーン (deterministic zone) との分離**:
//      ACS のゲームプレイ層 (Pillar B/C/F = sim / collision / physics) は
//      ティック決定論を保証する必要がある (replay / netcode / record-replay)。
//      ML 推論は浮動小数の非結合性、ドライバ依存、CUDA/DirectML ベンダ差により
//      **再現性が保証されない** 演算で、決定論ゾーンに混入させると即座に
//      desync を引き起こす。本 seam を使うコードは「決定論を捨ててよい所」
//      (グラフィックス・UI ヒント・LLM NPC のセリフ生成・アップスケール後の
//      表示バッファ) に限定すること、というルールを API レベルで強制する。
//   2. **オフラインフォールバック必須**:
//      ML モデルファイルや SDK ランタイムが配布パッケージに含まれない、
//      古いハードで FSR/DLSS が動かない、等のシナリオは出荷時に普通に起こる。
//      呼び出し側は `Init()` が失敗 / `ActiveKind() == Off` の場合に
//      **必ず通常の経路 (CPU パス / native 解像度) にフォールバック** すること。
//      stub 実装は最初からその「失敗側」を返すので、ゲーム側コードは day-0 から
//      フォールバックパスを書かざるを得なくなる (= 設計強制)。
//   3. **SDK ライセンスの隔離**:
//      DLSS / FSR / XeSS / ONNX Runtime はそれぞれ別のライセンス / NDA / 配布
//      制限を持つ。GameFramework 本体に直接リンクすると配布形態 (OSS 公開 /
//      クローズ SDK 同梱) が縛られるため、interface だけを本層に置き、
//      具象は別モジュール (オプションリンク) に追い出す。
//   4. **テスト容易性**:
//      `IMlRuntime` / `IUpscaler` の純粋仮想ポインタを差し替えるだけで
//      mock backend に置換でき、CI で ML SDK を持たないマシンでも上位層の
//      ロジック試験を回せる。
//
// Phase U-1 (本フェーズ) で提供するもの:
//   ・`IMlRuntime` / `IUpscaler` の純粋仮想 interface 確定
//   ・`FMlRuntimeStub` / `FUpscalerStub` の **失敗側を返すだけの stub 実装**
//   ・global stub アクセサ `GetMlRuntimeStub()` / `GetUpscalerStub()`
//
// Phase U-2+ で行うこと (本 header の範囲外):
//   ・ONNX Runtime / DirectML 連携の `OnnxMlRuntime` 実装 (別モジュール)
//   ・FSR2 / DLSS / XeSS 連携の各具象 `Upscaler` 実装 (別モジュール、SDK 同梱)
//   ・LLM NPC 安全パイプ (rate limit / content filter / 決定論なし宣言)
//
// ACS 規約遵守:
//   ・STL 不使用 / `<string>` 不使用 (文字列は `const char*` のみ)
//   ・例外不使用、エラーは `TResult<T, FErrorCode>` で伝搬
//   ・全 noexcept
//   ・stub class はシングルトン運用前提で コピー / ムーブ禁止
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"

namespace acs::game {

// =============================================================================
// FMlModelHandle — 不透明な ML モデルハンドル
// -----------------------------------------------------------------------------
// 具象 backend (ONNX / DirectML / NNAPI 等) が内部的に保持するモデル参照を、
// ゲーム層に対して **不透明な u64** として公開する。これにより上位は
// backend が変わってもコードを書き換えずに済む。
//
//   ・`_opaque == 0` を「無効ハンドル」として予約。
//   ・LoadModel が成功すると、backend は 0 以外の値を入れて返す。
//   ・UnloadModel に渡した後は再利用禁止 (use-after-free 検出は backend 任意)。
// =============================================================================
struct FMlModelHandle {
    u64 _opaque = 0;  // backend 固有の値。0 は無効を表す予約値。

    // 有効なハンドルか (= LoadModel から正常に返されたか) を判定。
    bool IsValid() const noexcept { return _opaque != 0; }
};

// =============================================================================
// IMlRuntime — ML 推論ランタイムの抽象 interface
// -----------------------------------------------------------------------------
// ONNX Runtime / DirectML / NNAPI / CoreML / TFLite いずれかの backend を
// 差し替えるための seam。**全 method が決定論ゾーンの外** で呼ばれる前提。
//
// ライフタイム契約:
//   Init() → (LoadModel / RunInference / UnloadModel)* → Shutdown()
//   ・Init() 未呼出での LoadModel は失敗を返してよい。
//   ・Shutdown() 後の再 Init() は実装定義 (stub は常に NotImplemented)。
//
// スレッド契約:
//   ・1 つの IMlRuntime instance は **シングルスレッドでのみ** 使う。
//     並列推論したい場合は instance を複数持つこと (= backend 側のセッション
//     を分ける)。本 interface でロックは要求しない。
// =============================================================================
class IMlRuntime {
public:
    virtual ~IMlRuntime() noexcept = default;

    // backend を初期化 (DLL 読み込み / セッション作成 / GPU 取得 等)。
    virtual TResult<void> Init() noexcept = 0;

    // backend を破棄 (LoadModel 済みハンドルも全解放してよい)。
    virtual void Shutdown() noexcept = 0;

    // モデルファイルを読み込んでハンドルを返す。
    // ・`model_path` は呼び出し側が寿命を保証する文字列リテラル / 長寿命バッファ。
    // ・`nullptr` 渡しは失敗とする (実装側で防御)。
    virtual TResult<FMlModelHandle> LoadModel(const char* model_path) noexcept = 0;

    // ハンドルを解放。無効ハンドル渡しは no-op 相当 (Ok を返してよい)。
    virtual TResult<void> UnloadModel(FMlModelHandle h) noexcept = 0;

    // 推論を 1 回実行。
    // ・`inputs` / `outputs` は呼び出し側が確保する f32 配列。
    // ・`in_count` / `out_count` はモデル定義と一致する必要がある (mismatch は失敗)。
    // ・**この呼び出しはブロッキング**。GPU バックエンドでも同期完了まで戻らない。
    //   非同期推論は Phase U-2 で別 API として追加予定。
    virtual TResult<void> RunInference(FMlModelHandle h,
                                      const f32* inputs,  u32 in_count,
                                      f32*       outputs, u32 out_count) noexcept = 0;

protected:
    IMlRuntime() noexcept = default;
    IMlRuntime(const IMlRuntime&)            = delete;
    IMlRuntime& operator=(const IMlRuntime&) = delete;
    IMlRuntime(IMlRuntime&&)                 = delete;
    IMlRuntime& operator=(IMlRuntime&&)      = delete;
};

// =============================================================================
// FMlRuntimeStub — 全 method NotImplemented を返す stub
// -----------------------------------------------------------------------------
// ONNX/DirectML/NNAPI と未統合の状況で、上位層が「ML 経路が常に失敗する」
// 前提で正しくフォールバックを書けているかを検証するための実装。
//
// Phase U-1 ではこれが唯一の `IMlRuntime` 実装。Phase U-2 で具象 backend が
// 追加されると、起動時に backend が選ばれて差し替わる構造になる。
// =============================================================================
class FMlRuntimeStub final : public IMlRuntime {
public:
    FMlRuntimeStub() noexcept = default;
    ~FMlRuntimeStub() noexcept override = default;

    TResult<void>            Init()                                       noexcept override;
    void                    Shutdown()                                   noexcept override;
    TResult<FMlModelHandle>   LoadModel(const char* model_path)            noexcept override;
    TResult<void>            UnloadModel(FMlModelHandle h)                 noexcept override;
    TResult<void>            RunInference(FMlModelHandle h,
                                         const f32* inputs,  u32 in_count,
                                         f32*       outputs, u32 out_count) noexcept override;
};

// =============================================================================
// IUpscaler — 解像度アップスケーラの抽象 interface (FSR / DLSS / XeSS)
// -----------------------------------------------------------------------------
// レンダラが低解像度で描画した最終バッファを、ターゲット解像度に拡大する
// 後段ステージの seam。具象は別モジュール (`UpscalerFsr` / `UpscalerDlss` /
// `UpscalerXess`) として SDK 依存込みで実装する。
//
// ライフタイム契約:
//   Init(kind) で 1 度だけ初期化、Shutdown() で 1 度だけ破棄。
//   kind 切替は Shutdown → Init(new_kind) の 2 段で行う。
// =============================================================================
enum class EUpscalerKind : u8 {
    Off    = 0,   // 無効化 (ネイティブ解像度のままパススルー)
    FSR    = 1,   // AMD FidelityFX Super Resolution
    DLSS   = 2,   // NVIDIA Deep Learning Super Sampling
    XeSS   = 3,   // Intel Xe Super Sampling
    Custom = 4,   // ユーザー実装 (アプリ側で IUpscaler 派生クラスを差し込み)
};

class IUpscaler {
public:
    virtual ~IUpscaler() noexcept = default;

    // 指定 kind で初期化。`Off` の場合は no-op 成功でよい。
    // 既に Init 済みの状態で再呼出した場合の挙動は実装定義 (推奨: 失敗を返す)。
    virtual TResult<void> Init(EUpscalerKind k) noexcept = 0;

    // 現在 active な kind。Init 前 / Shutdown 後は Off を返す。
    virtual EUpscalerKind ActiveKind() const noexcept = 0;

    // 破棄 (SDK セッション解放 / バッファ返却)。多重 Shutdown は no-op で良い。
    virtual void Shutdown() noexcept = 0;

    // 入力解像度 = レンダラが描画している低解像度バッファのサイズ。
    // Init 前 / Off の場合は 0 を返してよい。
    virtual u32 InputWidth()   const noexcept = 0;
    virtual u32 InputHeight()  const noexcept = 0;

    // 出力解像度 = アップスケール後の表示バッファサイズ。
    // Init 前 / Off の場合は 0 を返してよい。
    virtual u32 OutputWidth()  const noexcept = 0;
    virtual u32 OutputHeight() const noexcept = 0;

protected:
    IUpscaler() noexcept = default;
    IUpscaler(const IUpscaler&)            = delete;
    IUpscaler& operator=(const IUpscaler&) = delete;
    IUpscaler(IUpscaler&&)                 = delete;
    IUpscaler& operator=(IUpscaler&&)      = delete;
};

// =============================================================================
// FUpscalerStub — Off 状態のみを返す stub
// -----------------------------------------------------------------------------
// SDK 同梱前の状態でも上位層が「FSR/DLSS/XeSS が常に使えない」想定で
// フォールバック (= ネイティブ描画) を書けるようにするための placeholder。
//
// 仕様:
//   ・`Init(EUpscalerKind::Off)` は成功。
//   ・`Init(非 Off)` は NotImplemented を返す。
//   ・入出力サイズは常に 0 (使われない側として安全な値)。
// =============================================================================
class FUpscalerStub final : public IUpscaler {
public:
    FUpscalerStub() noexcept = default;
    ~FUpscalerStub() noexcept override = default;

    TResult<void> Init(EUpscalerKind k)     noexcept override;
    EUpscalerKind ActiveKind()       const noexcept override { return _kind; }
    void         Shutdown()               noexcept override { _kind = EUpscalerKind::Off; }

    u32 InputWidth()   const noexcept override { return 0; }
    u32 InputHeight()  const noexcept override { return 0; }
    u32 OutputWidth()  const noexcept override { return 0; }
    u32 OutputHeight() const noexcept override { return 0; }

private:
    EUpscalerKind _kind = EUpscalerKind::Off;
};

// =============================================================================
// global stub アクセサ
// -----------------------------------------------------------------------------
// process 内で 1 個だけ存在する静的 stub への参照を返す。Phase U-1 では
// `FGame` / `FScene` 側からの ML / Upscaler 問い合わせはすべてこの 2 つを通る。
// Phase U-2 以降、具象 backend が選ばれるとアクセサは差し替えられる。
//
// ※ static 単一インスタンス = process lifetime。スレッド安全性は呼び出し側責務。
// =============================================================================
IMlRuntime& GetMlRuntimeStub() noexcept;
IUpscaler&  GetUpscalerStub()  noexcept;

// NotImplemented 等の subcode を上位層が switch 分岐できるよう、
// SaveSlot.h と同じ「subcode = u16 で番号固定」の規約を踏襲する。
namespace ml_err {
    // 「未実装」: stub だから / Phase U-2 まで backend 未統合だから返される。
    inline constexpr u16 kSub_NotImplemented = 99;
    // 「無効引数」: nullptr / 無効ハンドル / in_count == 0 等。
    inline constexpr u16 kSub_InvalidArg     = 1;
} // namespace ml_err

} // namespace acs::game
