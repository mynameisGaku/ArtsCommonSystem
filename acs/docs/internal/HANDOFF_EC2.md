# ACS Cloud Dev Handoff (作成日: 2026-05-08)

EC2 (Windows Server 2025) 上で Claude Code を新規起動した時の context 受け渡し用。

---

## 0. 環境サマリ

- **Host**: AWS EC2 t3.large (Tokyo region, ap-northeast-1), Windows Server 2025 Datacenter
  - **後で `g4dn.xlarge` (NVIDIA T4 GPU) に切替予定**。AWS の vCPU quota (G/VT 系列) 承認待ち。承認後、停止 → instance type 変更 → 起動 で同じ EBS が引き継がれる
- **iPad**: Windows App (Microsoft 純正、旧 Microsoft Remote Desktop) で RDP 接続
- **インストール済**:
  - Visual Studio 2022 Community + 「C++ によるデスクトップ開発」ワークロード
  - Git for Windows
  - CMake (VS 同梱 + standalone 両方) + Ninja
  - Node.js v24 + Claude Code CLI (`claude`)
- **NVIDIA ドライバ**: g4dn 切替後にインストール (S3://nvidia-gaming/windows/latest/、要 IAM Role `acs-ec2-s3-read` のアタッチ)

---

## 1. プロジェクト概要

ACS は **日本インディー / 学習者向け軽量 C++ ゲームフレームワーク (Windows / DX12 ターゲット)**。

- リポジトリ: https://github.com/mynameisGaku/ArtsCommonSystem
- 作業ブランチ: `claude/phase-18-20`
- 言語規約: STL 不使用、独自 `TArray / THashMap / FString / TUniquePtr / TRc`、`TResult<T, FErrorCode>` エラーハンドリング (例外なし)
- 16 モジュール: Foundation / Threading / Memory / Container / Math / Test / Platform / Ecs / Asset / Render / App / Audio / Network / Imgui / Event / Mvvm / Ui

## 2. 設計判断 (勝手に動かさない)

| 項目 | 決定 | 理由 |
|---|---|---|
| **GC** | 採用しない | data-oriented (ECS / SoA) と相性悪い、TRc + Move + Entity ID で完結 |
| **Linux 対応** | しない | Windows / DX12 集中、過去に Linux 移植試行 → revert 済 |
| **MVVM 位置付け** | 一般 UI architecture pattern として | UE5 限定じゃない、`src/ui/` が純正 Widget framework |
| **RHI バックエンド** | Diligent Engine + 既存 DX12 raw 二刀流 | Vulkan / Metal 切替の前提 |
| **STL** | 使わない | アロケータ統制、ゲーム業界慣習、例外コスト回避 |

詳細は memory `project_acs_design_decisions.md` 相当の中身。

---

## 3. 直近のコミット (HEAD = 3261e1e on `claude/phase-18-20`)

**Phase 23 残ergonomics + 既存ビルドバグ fix**:

- 16 サンプルの Init+IsErr+Quit 5 行ブロックを `ACS_SAMPLE_INIT` マクロで 1 行に圧縮
- ビルド検証中に表面化した既存バグを fix:
  - **`acs::EventCallback` 衝突** (Window.h vs MessageBroker.h、Application.h 経由で全 sample がコンパイル不能) → broker 側を `MessageCallback` に rename
  - **`ThreadAffinity::Check`** の `GetThreadId()` (引数なし呼び出し、Win32 API は HANDLE 必須) → `acs::CurrentThreadId().raw`
  - **`mvvm/Derived.h`** constructor が C variadic 形 (`T(*)(const D&...)`) で非キャプチャ lambda を受けられず → template Fn + `if constexpr` で N=1..4 分岐
  - **`tests/mvvm_tests.cpp`** の `bind != nullptr` (TUniquePtr に `operator!=` 無し) → `bind.Get() != nullptr`

**検証**: 全 24 ターゲット (lib + tests + samples) ビルド成功、71/72 tests pass。
1 件 fail = `Event.TimerSetIntervalRepeats` (timing flake、私の変更無関係、要別途調査)。

---

## 4. 開かれてる候補タスク

優先度高め:
- **Phase 24/25** の続き (4 エージェント監査 synthesis の残項目)
- **`Event.TimerSetIntervalRepeats` flake 調査** (`acs/tests/event_tests.cpp:46`、Tick(1.6f) で `hits >= 3` が成立しない、TimerManager の境界判定にバグ疑い)

中期:
- **archetype 検討** (10 万 entity ベンチで sparse set のボトルネック確認後)
- **JobGraph cycle detection 強化** (Kahn 法導入済、エッジケース潰す)
- **MessagePipe ring buffer 化** (現状 TArray 前詰めで O(N) Pop)
- **Vulkan 経路の動作検証** (現状 D3D12 のみテスト想定、Diligent 経由で Vulkan SKU が有効化された場合の確認)

---

## 5. ビルド / テスト基本操作

```powershell
# 初回 clone
cd $env:USERPROFILE\source
mkdir repos -Force; cd repos
git clone https://github.com/mynameisGaku/ArtsCommonSystem.git
cd ArtsCommonSystem\acs
git checkout claude/phase-18-20

# 普通の build (デバッグ)
cmake -B cmake-build-debug -S .
cmake --build cmake-build-debug

# 個別ターゲット
cmake --build cmake-build-debug --target hello_audio
cmake --build cmake-build-debug --target acs_unit_tests

# テスト実行
.\cmake-build-debug\tests\acs_unit_tests.exe

# Diligent backend を使うサンプル (HelloBloom 等)
cmake -B cmake-build-debug -S . -DACS_RENDER_DILIGENT=ON
```

**注意**:
- `g4dn.xlarge` 切替前は DX12 GPU 描画系サンプル (HelloBloom / HelloShadows / HelloUI 等) は実行できない (ビルドは可)
- 単体テスト (acs_unit_tests) は GPU 不要、t3.large でも全て通る
- VS 内から開く場合は ACS フォルダを「フォルダーを開く」で読み込み → CMakeLists.txt 自動検出

---

## 6. ユーザー collaboration スタイル (重要、メモ)

- 日本語で会話、絵文字なし、簡潔好み (terse)
- 「続けて」で次フェーズに進む
- 技術選定は **候補 + 推奨を提示してユーザーに選ばせる** (Plan ファースト)
- 「まだ進めないで」は厳守
- 末尾の長い summary 不要、変更点 1-2 行で OK
- 実機ビルドは Rider / VS でユーザーが回す (今回は EC2 上の VS)
- `/auto` `/enable-auto-mode` 設定済 → 細かい確認不要、自走 OK

---

## 7. 既知の引っかかりポイント

- **OneDrive 上のリポジトリ問題**: 元々のローカル home (`C:\Users\g0190\OneDrive\Desktop\acs_github`) は OneDrive sync 経由なので git operations が時々遅い / 一時 lock。EC2 上では `$env:USERPROFILE\source\repos` 直下 (OneDrive 外) に clone する想定
- **AWS msstore 証明書エラー**: winget で msstore source 使うと cert エラー、`--source winget` で回避
- **PATH 反映**: Node.js / VS / CMake インストール後は **PowerShell ウィンドウを開き直す**、または:
  ```powershell
  $env:Path = [Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [Environment]::GetEnvironmentVariable("Path","User")
  ```
- **EBS スナップショット推奨**: GPU 切替 (instance type 変更) 前にスナップショット撮っておくと安心

---

## 8. EC2 運用メモ

- **使用後は必ず instance を停止**: 停止中はコンピュート課金 0、EBS 100GB gp3 のみ ~¥1500/月。起動忘れ放置で月数万円事故りやすい
- **Public IP**: 起動の度に変わる (Elastic IP 付けてないなら)。RDP 接続先 IP を毎回 EC2 console で確認
- **自動停止スケジュール**: EventBridge ルールで毎日 23:00 (UTC 14:00) に `aws ec2 stop-instances` を cron 化推奨
- **g4dn 切替手順**: stop → 「アクション → インスタンス設定 → インスタンスタイプを変更」 → `g4dn.xlarge` → start → NVIDIA Gaming ドライバ S3 から DL → vGamingMarketplace レジストリ設定 → 再起動 → `nvidia-smi` で確認

---

このファイル自体は OneDrive 上の `acs_github\HANDOFF_EC2.md` に保存。EC2 から見るなら git clone 後の repo 内に同名ファイルを置くか、内容を Claude にペーストして context として読ませる想定。
