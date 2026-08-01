// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar U — IMlRuntime / IUpscaler (AI/ML & Super-Resolution seams)
//
// 役割:
//   Pillar U (AI & ML 統合) のうち、外部 ML ランタイム (ONNX Runtime / DirectML /
//   NNAPI / CoreML / TensorFlow Lite 等) と、フレームのアップスケーラ
//   (AMD FSR / NVIDIA DLSS / Intel XeSS) を **interface seam** として隔離する。
//
//   両者ともゲームロジック側からは `IMlRuntime&` / `IUpscaler&` 越しに叩く前提で、
//   具象 backend (Onnx / DirectML / FSR2 / DLSS / XeSS) は **後段の独立モジュール**
//   ("MlOnnx" 等) で SDK 取り込みと差し替えを行う。本 header はその「窓口」だけを
//   定義するスケルトンで、stub 実装のみを提供する。
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
// 本 header で提供するもの:
//   ・`IMlRuntime` / `IUpscaler` の純粋仮想 interface 確定
//   ・`CMlRuntimeStub` / `CUpscalerStub` の **失敗側を返すだけの stub 実装**
//   ・global stub アクセサ `GetMlRuntimeStub()` / `GetUpscalerStub()`
//
// 本 header の範囲外:
//   ・ONNX Runtime / DirectML 連携の `FOnnxMlRuntime` 実装 (別モジュール)
//   ・FSR2 / DLSS / XeSS 連携の各具象 `IUpscaler` 実装 (別モジュール、SDK 同梱)
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

/**
 * 不透明な ML モデルハンドル。
 *
 * @details
 * 具象 backend (ONNX / DirectML / NNAPI 等) が内部的に保持するモデル参照を、
 * ゲーム層に対して不透明な u64 として公開する。m_Opaque == 0 を無効ハンドルとして
 * 予約し、LoadModel 成功時に backend が 0 以外を入れて返す。UnloadModel に渡した後は
 * 再利用禁止。
 */
struct FMlModelHandle {
    /** backend 固有の値 (0 は無効を表す予約値)。 */
    u64 m_Opaque = 0;

    /**
     * 有効なハンドルか (LoadModel から正常に返されたか) を判定する。
     *
     * @return m_Opaque が非 0 なら true。
     */
    bool IsValid() const noexcept { return m_Opaque != 0; }
};

/**
 * ML 推論ランタイムの抽象 interface。
 *
 * @details
 * ONNX Runtime / DirectML / NNAPI / CoreML / TFLite いずれかの backend を差し替える
 * ための seam で、全 method は決定論ゾーンの外で呼ばれる前提。ライフタイムは
 * Init() → (LoadModel / RunInference / UnloadModel)* → Shutdown()。1 instance は
 * シングルスレッドでのみ使う (並列推論は instance を複数持つ)。
 */
class IMlRuntime {
public:
    /** 派生 backend を正しく破棄するための仮想デストラクタ。 */
    virtual ~IMlRuntime() noexcept = default;

    /**
     * backend を初期化する (DLL 読み込み / セッション作成 / GPU 取得 等)。
     *
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> Init() noexcept = 0;

    /**
     * backend を破棄する (LoadModel 済みハンドルも全解放してよい)。
     */
    virtual void Shutdown() noexcept = 0;

    /**
     * モデルファイルを読み込んでハンドルを返す。
     *
     * @details model_path は呼び出し側が寿命を保証する文字列。nullptr 渡しは失敗とする。
     * @param model_path 読み込むモデルファイルのパス。
     * @return 成功ならモデルハンドル、失敗ならエラー。
     */
    virtual TResult<FMlModelHandle> LoadModel(const char* model_path) noexcept = 0;

    /**
     * ハンドルを解放する。
     *
     * @details 無効ハンドル渡しは no-op 相当 (Ok を返してよい)。
     * @param h 解放するモデルハンドル。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> UnloadModel(FMlModelHandle h) noexcept = 0;

    /**
     * 推論を 1 回実行する。
     *
     * @details
     * inputs / outputs は呼び出し側が確保する f32 配列。in_count / out_count は
     * モデル定義と一致する必要がある (mismatch は失敗)。この呼び出しはブロッキングで、
     * GPU バックエンドでも同期完了まで戻らない。
     * @param h 推論に使うモデルハンドル。
     * @param inputs 入力 f32 配列。
     * @param in_count 入力要素数。
     * @param outputs 出力を書き込む f32 配列。
     * @param out_count 出力要素数。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> RunInference(FMlModelHandle h,
                                      const f32* inputs,  u32 in_count,
                                      f32*       outputs, u32 out_count) noexcept = 0;

protected:
    /** 派生クラス専用の既定コンストラクタ。 */
    IMlRuntime() noexcept = default;

    /** コピー禁止。 */
    IMlRuntime(const IMlRuntime&)            = delete;

    /** コピー代入も禁止。 */
    IMlRuntime& operator=(const IMlRuntime&) = delete;

    /** ムーブ禁止。 */
    IMlRuntime(IMlRuntime&&)                 = delete;

    /** ムーブ代入も禁止。 */
    IMlRuntime& operator=(IMlRuntime&&)      = delete;
};

/**
 * 全 method が NotImplemented を返す IMlRuntime の stub 実装。
 *
 * @details
 * 具象 backend (ONNX/DirectML/NNAPI) と未統合の状況で、上位層が「ML 経路が常に
 * 失敗する」前提で正しくフォールバックを書けているかを検証するための実装。具象
 * backend が追加されると起動時に差し替わる。
 */
class CMlRuntimeStub final : public IMlRuntime {
public:
    /** stub を構築する。 */
    CMlRuntimeStub() noexcept = default;

    /** stub を破棄する (保持リソースなし)。 */
    ~CMlRuntimeStub() noexcept override = default;

    /**
     * 常に NotImplemented を返す。
     *
     * @return NotImplemented subcode 付きエラー。
     */
    TResult<void>            Init()                                       noexcept override;

    /** 何も保持しないため no-op。 */
    void                    Shutdown()                                   noexcept override;

    /**
     * 常に NotImplemented を返す。
     *
     * @param model_path 無視される (stub のため)。
     * @return NotImplemented subcode 付きエラー。
     */
    TResult<FMlModelHandle>   LoadModel(const char* model_path)            noexcept override;

    /**
     * 常に NotImplemented を返す。
     *
     * @param h 無視される (stub のため)。
     * @return NotImplemented subcode 付きエラー。
     */
    TResult<void>            UnloadModel(FMlModelHandle h)                 noexcept override;

    /**
     * 常に NotImplemented を返す。
     *
     * @param h 無視される (stub のため)。
     * @param inputs 無視される (stub のため)。
     * @param in_count 無視される (stub のため)。
     * @param outputs 無視される (stub のため)。
     * @param out_count 無視される (stub のため)。
     * @return NotImplemented subcode 付きエラー。
     */
    TResult<void>            RunInference(FMlModelHandle h,
                                         const f32* inputs,  u32 in_count,
                                         f32*       outputs, u32 out_count) noexcept override;
};

/**
 * 解像度アップスケーラの種別 (FSR / DLSS / XeSS)。
 */
enum class EUpscalerKind : u8 {
    /** 無効化 (ネイティブ解像度のままパススルー)。 */
    Off    = 0,

    /** AMD FidelityFX Super Resolution。 */
    FSR    = 1,

    /** NVIDIA Deep Learning Super Sampling。 */
    DLSS   = 2,

    /** Intel Xe Super Sampling。 */
    XeSS   = 3,

    /** ユーザー実装 (アプリ側で IUpscaler 派生クラスを差し込み)。 */
    Custom = 4,
};

/**
 * 解像度アップスケーラの抽象 interface (FSR / DLSS / XeSS)。
 *
 * @details
 * レンダラが低解像度で描画した最終バッファをターゲット解像度に拡大する後段ステージの
 * seam。具象は別モジュール (UpscalerFsr / UpscalerDlss / UpscalerXess) として SDK
 * 依存込みで実装する。ライフタイムは Init(kind) で 1 度だけ初期化、Shutdown() で 1 度
 * だけ破棄し、kind 切替は Shutdown → Init(new_kind) の 2 段で行う。
 */
class IUpscaler {
public:
    /** 派生 backend を正しく破棄するための仮想デストラクタ。 */
    virtual ~IUpscaler() noexcept = default;

    /**
     * 指定 kind で初期化する。
     *
     * @details Off の場合は no-op 成功でよい。Init 済みでの再呼出は実装定義 (推奨: 失敗)。
     * @param k 初期化するアップスケーラ種別。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> Init(EUpscalerKind k) noexcept = 0;

    /**
     * 現在 active な kind を返す。
     *
     * @return active なアップスケーラ種別 (Init 前 / Shutdown 後は Off)。
     */
    virtual EUpscalerKind ActiveKind() const noexcept = 0;

    /**
     * 破棄する (SDK セッション解放 / バッファ返却)。多重 Shutdown は no-op でよい。
     */
    virtual void Shutdown() noexcept = 0;

    /**
     * 入力 (低解像度バッファ) の幅を返す。
     *
     * @return 入力幅 (Init 前 / Off では 0 を返してよい)。
     */
    virtual u32 InputWidth()   const noexcept = 0;

    /**
     * 入力 (低解像度バッファ) の高さを返す。
     *
     * @return 入力高さ (Init 前 / Off では 0 を返してよい)。
     */
    virtual u32 InputHeight()  const noexcept = 0;

    /**
     * 出力 (アップスケール後の表示バッファ) の幅を返す。
     *
     * @return 出力幅 (Init 前 / Off では 0 を返してよい)。
     */
    virtual u32 OutputWidth()  const noexcept = 0;

    /**
     * 出力 (アップスケール後の表示バッファ) の高さを返す。
     *
     * @return 出力高さ (Init 前 / Off では 0 を返してよい)。
     */
    virtual u32 OutputHeight() const noexcept = 0;

protected:
    /** 派生クラス専用の既定コンストラクタ。 */
    IUpscaler() noexcept = default;

    /** コピー禁止。 */
    IUpscaler(const IUpscaler&)            = delete;

    /** コピー代入も禁止。 */
    IUpscaler& operator=(const IUpscaler&) = delete;

    /** ムーブ禁止。 */
    IUpscaler(IUpscaler&&)                 = delete;

    /** ムーブ代入も禁止。 */
    IUpscaler& operator=(IUpscaler&&)      = delete;
};

/**
 * Off 状態のみを受け付ける IUpscaler の stub 実装。
 *
 * @details
 * SDK 同梱前でも上位層が「FSR/DLSS/XeSS が常に使えない」想定でフォールバック
 * (= ネイティブ描画) を書けるようにする placeholder。Init(Off) は成功、Init(非 Off) は
 * NotImplemented を返し、入出力サイズは常に 0。
 */
class CUpscalerStub final : public IUpscaler {
public:
    /** Off 状態で stub を構築する。 */
    CUpscalerStub() noexcept = default;

    /** stub を破棄する (保持リソースなし)。 */
    ~CUpscalerStub() noexcept override = default;

    /**
     * Off なら成功、それ以外は NotImplemented を返す。
     *
     * @param k 初期化要求するアップスケーラ種別。
     * @return Off なら成功、非 Off なら NotImplemented subcode 付きエラー。
     */
    TResult<void> Init(EUpscalerKind k)     noexcept override;

    /**
     * 現在の kind を返す。
     *
     * @return 保持している EUpscalerKind (常に Off)。
     */
    EUpscalerKind ActiveKind()       const noexcept override { return m_Kind; }

    /** kind を Off に戻す。 */
    void         Shutdown()               noexcept override { m_Kind = EUpscalerKind::Off; }

    /**
     * 入力幅を返す。
     *
     * @return 常に 0。
     */
    u32 InputWidth()   const noexcept override { return 0; }

    /**
     * 入力高さを返す。
     *
     * @return 常に 0。
     */
    u32 InputHeight()  const noexcept override { return 0; }

    /**
     * 出力幅を返す。
     *
     * @return 常に 0。
     */
    u32 OutputWidth()  const noexcept override { return 0; }

    /**
     * 出力高さを返す。
     *
     * @return 常に 0。
     */
    u32 OutputHeight() const noexcept override { return 0; }

private:
    /** 現在のアップスケーラ種別 (常に Off)。 */
    EUpscalerKind m_Kind = EUpscalerKind::Off;
};

/**
 * process 内で 1 個だけ存在する ML ランタイム stub への参照を返す。
 *
 * @details static 単一インスタンス (process lifetime)。スレッド安全性は呼び出し側責務。
 * @return 共有 CMlRuntimeStub への参照。
 */
IMlRuntime& GetMlRuntimeStub() noexcept;

/**
 * process 内で 1 個だけ存在する IUpscaler stub への参照を返す。
 *
 * @details static 単一インスタンス (process lifetime)。スレッド安全性は呼び出し側責務。
 * @return 共有 CUpscalerStub への参照。
 */
IUpscaler&  GetUpscalerStub()  noexcept;

/**
 * 既定 IMlRuntime を返す provider 関数の型 (実 backend モジュールが登録する)。
 */
using MlRuntimeProvider = IMlRuntime& (*)() noexcept;

/**
 * 既定 IMlRuntime provider を登録する (実 backend モジュールの Install* から呼ぶ)。
 *
 * @details nullptr 登録で stub に戻す。後勝ち。
 * @param provider 登録する provider 関数 (nullptr で解除)。
 */
void SetMlRuntimeProvider(MlRuntimeProvider provider) noexcept;

/**
 * 既定 IMlRuntime を返す。
 *
 * @return provider 登録済みならその実 runtime、未登録なら GetMlRuntimeStub()。
 */
IMlRuntime& GetDefaultMlRuntime() noexcept;

/**
 * ML seam が返すエラー subcode。
 *
 * @details 上位層が switch 分岐できるよう SaveSlot.h と同じ「subcode = u16 で番号固定」の規約に揃える。
 */
namespace ml_err {
    /** 未実装 (stub / backend 未統合のため返される)。 */
    inline constexpr u16 kSub_NotImplemented = 99;

    /** 無効引数 (nullptr / 無効ハンドル / in_count == 0 等)。 */
    inline constexpr u16 kSub_InvalidArg     = 1;
} // namespace ml_err

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FMlRuntimeStub = CMlRuntimeStub;

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FUpscalerStub = CUpscalerStub;

} // namespace acs::game
