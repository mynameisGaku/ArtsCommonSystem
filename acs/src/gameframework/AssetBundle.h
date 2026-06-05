// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar G — FAssetBundle (シーンスコープのアセット集合非同期ロード)
//
// 役割:
//   シーンが「このシーンで使う N 個のアセット」をまとめて宣言し、BeginLoad() で
//   一括非同期ロード → 集約進捗 (Progress / IsLoaded / HasFailed) を確認する。
//   個別 path の FAssetFuture を散在管理する代わりに 1 つの bundle で扱える。
//
// 使い方 (典型例):
//   class GameplayScene : public Scene {
//       acs::game::FAssetBundle m_Bundle;
//       void OnEnter() noexcept override {
//           m_Bundle.Add("textures/hero.png");
//           m_Bundle.Add("audio/bgm.ogg");
//           m_Bundle.Add("meshes/level.glb");
//           m_Bundle.BeginLoad();
//       }
//       void OnUpdate(f32) noexcept override {
//           if (!m_Bundle.IsLoaded()) {
//               // ローディング画面: m_Bundle.Progress() を表示
//               return;
//           }
//           // 通常ゲームループ
//       }
//       void OnExit() noexcept override { m_Bundle.Unload(); }
//   };
//
// 設計方針:
//   ・シーン死亡で bundle 廃棄 → 内部 TSharedPtr<Asset> が drop → FAssetRegistry の refcount
//     が下がり、他参照がなければ実体メモリも解放される (GC 不要 / 決定的解放)。
//   ・path 文字列は呼び出し側が寿命を保証する (string literal / 永続バッファ前提)。
//     ACS 規約により <string> は使わない (const char* を TArray に保持)。
//   ・BeginLoad(registry) は FAssetRegistry::Load を使った同期ロード。戻った時点で
//     各 entry は Loaded / Failed 確定 (非同期分割は本 bundle の対象外)。
//   ・全 API noexcept (例外は使わない / TResult も bundle 表層では露出しない)。
#pragma once

#include "foundation/Types.h"
#include "foundation/Log.h"
#include "container/Array.h"
#include "memory/SharedPtr.h"
#include "asset/Asset.h"

namespace acs { class FAssetRegistry; }

namespace acs::game {

/**
 * シーンスコープのアセット集合を非同期ロードし集約進捗を提供する bundle。
 *
 * @details
 * ライフサイクルは Add* → BeginLoad → (poll Progress/IsLoaded) → ... → Unload。
 * Scene にメンバとして埋め込み、内部 TSharedPtr<Asset> の保持で Unload まで実体の
 * 生存を保証する。BeginLoad 後の Add はロード開始後の集合変更が未定義になるのを
 * 避けるため無視する。
 */
class FAssetBundle {
public:
    /** bundle 内の各 asset の進捗状態。 */
    enum class LoadStatus : u8 {
        /** BeginLoad 前、または BeginLoad 失敗で開始されなかった。 */
        Pending = 0,

        /** LoadAsync 発行済 / 完了待ち。 */
        Loading = 1,

        /** 完了 (成功)。 */
        Loaded  = 2,

        /** 完了 (失敗)。 */
        Failed  = 3,
    };

    /** 空の bundle を構築する (asset 未登録、未ロード)。 */
    FAssetBundle() noexcept = default;

    /** bundle を破棄する (内部 TSharedPtr が drop し refcount を落とす)。 */
    ~FAssetBundle() noexcept = default;

    /** コピー禁止 (内部 TArray 規約。bundle は Scene に単独所有させる)。 */
    FAssetBundle(const FAssetBundle&)            = delete;

    /** コピー代入も禁止。 */
    FAssetBundle& operator=(const FAssetBundle&) = delete;

    /**
     * bundle に asset パスを追加する (実体ロードはまだ走らず BeginLoad で開始)。
     *
     * @details
     * path 文字列は呼び出し側が FAssetBundle 寿命中ずっと有効に保つ必要がある
     * (string literal を想定。動的文字列は渡す側で保持する)。BeginLoad 後の Add は
     * no-op (警告ログのみ)。
     * @param asset_path 追加する asset の仮想パス (借用、コピーしない)。
     */
    void Add(const char* asset_path) noexcept;

    /**
     * 登録済み全 path を registry 経由で同期ロードし、結果を各 entry に保持する。
     *
     * @details
     * registry は app が所有するもの (FApplication::GetAssets() / FGame 経由) を渡し、
     * RegisterDefaultLoaders() 済みであること。FAssetRegistry::Load は同期なので戻った
     * 時点で各 entry は Loaded / Failed 確定。多重呼び出しは no-op (警告のみ)。
     * @param registry ロードに使う app 所有の asset レジストリ。
     */
    void BeginLoad(FAssetRegistry& registry) noexcept;

    /**
     * Add 順の index でロード済み asset を取得する。
     *
     * @param index Add した順序のインデックス。
     * @return ロード済み asset (未ロード / 範囲外 / 失敗は空 TSharedPtr)。
     */
    TSharedPtr<Asset> GetAsset(u32 index) const noexcept;

    /**
     * path 一致の asset を取得する。
     *
     * @param asset_path 探す asset の仮想パス。
     * @return path 一致の asset (見つからない / 失敗は空 TSharedPtr)。
     */
    TSharedPtr<Asset> FindAsset(const char* asset_path) const noexcept;

    /**
     * ロード完了割合を返す (ローディング画面のプログレスバー用途)。
     *
     * @return 完了割合 [0, 1]。BeginLoad 未呼び出しは 0、全 asset 完了 (成功/失敗問わず) で 1。
     */
    f32 Progress() const noexcept;

    /**
     * 全 asset がロード完了 (Loaded または Failed) しているかを返す。
     *
     * @return 全 asset 完了なら true。空 bundle も true。
     */
    bool IsLoaded() const noexcept;

    /**
     * 1 個でも Failed が含まれるかを返す (シーン側のエラーフォールバック判断用)。
     *
     * @return Failed の asset が 1 つでもあれば true。
     */
    bool HasFailed() const noexcept;

    /**
     * bundle に登録された asset 総数を返す。
     *
     * @return 登録済み asset 数。
     */
    u32 AssetCount() const noexcept;

    /**
     * ロード成功 (Loaded のみ、Failed は含まない) した asset 数を返す。
     *
     * @return Loaded 状態の asset 数。
     */
    u32 LoadedCount() const noexcept;

    /**
     * 全 asset を解放し、内部 entry を Pending に戻す。
     *
     * @details
     * 内部 TSharedPtr を drop して refcount を落とす。シーン破棄前に呼ぶと決定的な
     * タイミングで解放できる (デストラクタ任せでも良いが Scene::OnExit で明示するのが推奨)。
     */
    void Unload() noexcept;

private:
    /** bundle 内の 1 個の asset エントリ。 */
    struct Entry {
        /** asset の仮想パス (呼び出し側所有の文字列を借用、コピーしない)。 */
        const char* path   = nullptr;

        /** この asset の進捗状態。 */
        LoadStatus  status = LoadStatus::Pending;

        /** BeginLoad で registry からロードした実体 (失敗時は空)。Unload まで生存を保証する。 */
        TSharedPtr<Asset>  asset;
    };

    /** BeginLoad を 1 度でも実行したか (Add の閉鎖判定用)。 */
    bool          m_bBegun = false;

    /** 登録済み asset エントリの配列 (Add 順)。 */
    TArray<Entry>  m_Entries;
};

} // namespace acs::game
